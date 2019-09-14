#
# Copyright 2018-2019 Ettus Research, a National Instruments Company
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""
E310 implementation module
"""

from __future__ import print_function
import bisect
import copy
import re
import threading
from six import iteritems, itervalues
from usrp_mpm.components import ZynqComponents
from usrp_mpm.dboard_manager import E31x_db
from usrp_mpm.mpmtypes import SID
from usrp_mpm.mpmutils import assert_compat_number, str2bool
from usrp_mpm.periph_manager import PeriphManagerBase
from usrp_mpm.rpc_server import no_rpc
from usrp_mpm.sys_utils import dtoverlay
from usrp_mpm.sys_utils.sysfs_thermal import read_sysfs_sensors_value
from usrp_mpm.sys_utils.udev import get_spidev_nodes
from usrp_mpm.xports import XportMgrLiberio
from usrp_mpm.periph_manager.e31x_periphs import MboardRegsControl
from usrp_mpm.sys_utils.udev import get_eeprom_paths
from usrp_mpm import e31x_legacy_eeprom

E310_DEFAULT_CLOCK_SOURCE = 'internal'
E310_DEFAULT_TIME_SOURCE = 'internal'
E310_DEFAULT_ENABLE_FPGPIO = True
E310_FPGA_COMPAT = (4,0)
E310_DBOARD_SLOT_IDX = 0

###############################################################################
# Transport managers
###############################################################################

class E310XportMgrLiberio(XportMgrLiberio):
    " E310-specific Liberio configuration "
    max_chan = 5

###############################################################################
# Main Class
###############################################################################
class e31x(ZynqComponents, PeriphManagerBase):
    """
    Holds E310 specific attributes and methods
    """
    #########################################################################
    # Overridables
    #
    # See PeriphManagerBase for documentation on these fields
    #########################################################################
    description = "E300-Series Device"
    # 0x77d2 and 0x77d3
    pids = {0x77D2: 'e310_sg1', #sg1
            0x77D3: 'e310_sg3'} #sg3
    mboard_eeprom_addr = "e0004000.i2c"
    mboard_eeprom_offset = 0
    mboard_eeprom_max_len = 64
    # We have two nvem paths on the E310.
    # This one ensures that we get the right path for the MB.
    mboard_eeprom_path_index = 1
    mboard_info = {"type": "e3xx"}
    mboard_sensor_callback_map = {
        'ref_locked': 'get_ref_lock_sensor',
        'temp_fpga' : 'get_fpga_temp_sensor',
        'temp_mb' : 'get_mb_temp_sensor',
    }
    dboard_eeprom_addr = "e0004000.i2c"

    # Actual DB EEPROM bytes are just 28. Reading just a couple more.
    # Refer e300_eeprom_manager.hpp
    dboard_eeprom_max_len = 32

    max_num_dboards = 1

    # We're on a Zynq target, so the following two come from the Zynq standard
    # device tree overlay (tree/arch/arm/boot/dts/zynq-7000.dtsi)
    dboard_spimaster_addrs = ["e0006000.spi"]
    # E310-specific settings
    # Label for the mboard UIO
    mboard_regs_label = "mboard-regs"
    # Override the list of updateable components
    updateable_components = {
        'fpga': {
            'callback': "update_fpga",
            'path': '/lib/firmware/{}.bin',
            'reset': True,
        },
        'dts': {
            'callback': "update_dts",
            'path': '/lib/firmware/{}.dts',
            'output': '/lib/firmware/{}.dtbo',
            'reset': False,
        },
    }
    # This class removes the overlay in tear_down() resulting
    # in stale references to methods in the RPC server. Setting
    # this to True ensures that the RPC server clears all registered
    # methods on unclaim() and registers them on the following claim().
    clear_rpc_method_registry_on_unclaim = True

    @classmethod
    def generate_device_info(cls, eeprom_md, mboard_info, dboard_infos):
        """
        Generate dictionary describing the device.
        """
        # Add the default PeriphManagerBase information first
        device_info = super().generate_device_info(
            eeprom_md, mboard_info, dboard_infos)
        # Then add E31x-specific information
        mb_pid = eeprom_md.get('pid')
        device_info['product'] = cls.pids.get(mb_pid, 'unknown')
        return device_info

    @staticmethod
    def list_required_dt_overlays(device_info):
        """
        Lists device tree overlays that need to be applied before this class can
        be used. List of strings.
        Are applied in order.

        eeprom_md -- Dictionary of info read out from the mboard EEPROM
        device_args -- Arbitrary dictionary of info, typically user-defined
        """
        return [device_info['product']]


    @staticmethod
    def get_idle_dt_overlay(device_info):
        """
        Overlay to be applied to enter low power idle state.
        """
        # e.g. e310_sg3_idle
        idle_overlay = device_info['product'] + '_idle'
        return idle_overlay

    ###########################################################################
    # Ctor and device initialization tasks
    ###########################################################################
    def __init__(self, args):
        """
        Does partial initialization which loads low power idle image
        """
        super(e31x, self).__init__()
        # Start clean by removing MPM-owned overlays.
        active_overlays = self.list_active_overlays()
        mpm_overlays = self.list_owned_overlays()
        for overlay in active_overlays:
            if overlay in mpm_overlays:
                dtoverlay.rm_overlay(overlay)
        # Apply idle overlay on boot to save power until
        # an application tries to use the device.
        self.args_cached = args
        self.apply_idle_overlay()
        self._device_initialized = False

    def _init_normal(self):
        """
        Does full initialization
        """
        if self._device_initialized:
            return
        if self.is_idle():
            self.remove_idle_overlay()
        self.overlay_apply()
        self.init_dboards(self.args_cached)
        if not self._device_initialized:
            # Don't try and figure out what's going on. Just give up.
            return
        # Initialize _do_not_reload with value from _default_args (mpm.conf)
        self._do_not_reload = str2bool(self._default_args.get("no_reload_fpga", "False"))
        self._tear_down = False
        self._clock_source = None
        self._time_source = None
        self._available_endpoints = list(range(256))
        self.dboard = self.dboards[E310_DBOARD_SLOT_IDX]
        try:
            self._init_peripherals(self.args_cached)
        except Exception as ex:
            self.log.error("Failed to initialize motherboard: %s", str(ex))
            self._initialization_status = str(ex)
            self._device_initialized = False

    def _init_dboards(self, dboard_infos, override_dboard_pids, default_args):
        """
        Initialize all the daughterboards

        dboard_infos -- List of dictionaries as returned from
                       PeriphManagerBase._get_dboard_eeprom_info()
        override_dboard_pids -- List of dboard PIDs to force
        default_args -- Default args
        """
        # Override the base class's implementation in order to avoid initializing our one "dboard"
        # in the same way that, for example, N310's dboards are initialized. Specifically,
        # - skip dboard EEPROM setup (we don't have one)
        # - change the way we handle SPI devices
        if override_dboard_pids:
            self.log.warning("Overriding daughterboard PIDs with: {}"
                             .format(override_dboard_pids))
            raise NotImplementedError("Can't override dboard pids")
        # We have only one dboard
        dboard_info = dboard_infos[0]
        # Set up the SPI nodes
        spi_nodes = []
        for spi_addr in self.dboard_spimaster_addrs:
            for spi_node in get_spidev_nodes(spi_addr):
                bisect.insort(spi_nodes, spi_node)

        self.log.trace("Found spidev nodes: {0}".format(spi_nodes))

        if not spi_nodes:
            self.log.warning("No SPI nodes for dboard %d.", E310_DBOARD_SLOT_IDX)
        else:
            dboard_info.update({
                    'spi_nodes': spi_nodes,
                    'default_args': default_args,
                })

        self.dboards.append(E31x_db(E310_DBOARD_SLOT_IDX, **dboard_info))
        self.log.info("Found %d daughterboard(s).", len(self.dboards))

    def _check_fpga_compat(self):
        " Throw an exception if the compat numbers don't match up "
        actual_compat = self.mboard_regs_control.get_compat_number()
        self.log.debug("Actual FPGA compat number: {:d}.{:d}".format(
            actual_compat[0], actual_compat[1]
        ))
        assert_compat_number(
            E310_FPGA_COMPAT,
            self.mboard_regs_control.get_compat_number(),
            component="FPGA",
            fail_on_old_minor=True,
            log=self.log
        )

    def _init_ref_clock_and_time(self, default_args):
        """
        Initialize clock and time sources. After this function returns, the
        reference signals going to the FPGA are valid.
        """
        if not self.dboards:
            self.log.warning(
                "No dboards found, skipping setting clock and time source "
                "configuration."
            )
            self._clock_source = E310_DEFAULT_CLOCK_SOURCE
            self._time_source = E310_DEFAULT_TIME_SOURCE
        else:
            self.set_clock_source(
                default_args.get('clock_source', E310_DEFAULT_CLOCK_SOURCE)
            )
            self.set_time_source(
                default_args.get('time_source', E310_DEFAULT_TIME_SOURCE)
            )

    def _init_peripherals(self, args):
        """
        Turn on all peripherals. This may throw an error on failure, so make
        sure to catch it.

        Peripherals are initialized in the order of least likely to fail, to most
        likely.
        """
        # Sanity checks
        assert self.mboard_info.get('product') in self.pids.values(), \
            "Device product could not be determined!"
        # Init Mboard Regs
        self.mboard_regs_control = MboardRegsControl(
            self.mboard_regs_label, self.log)
        self.mboard_regs_control.get_git_hash()
        self.mboard_regs_control.get_build_timestamp()
        self._check_fpga_compat()
        self._update_fpga_type()
        # Init clocking
        self._init_ref_clock_and_time(args)
        # Init CHDR transports
        self._xport_mgrs = {
            'liberio': E310XportMgrLiberio(self.log.getChild('liberio')),
        }
        # Init complete.
        self.log.debug("mboard info: {}".format(self.mboard_info))

    def _read_mboard_eeprom(self):
        """
        Read out mboard EEPROM.
        Returns a tuple: (eeprom_dict, eeprom_rawdata), where the the former is
        a de-serialized dictionary representation of the data, and the latter
        is a binary string with the raw data.

        If no EEPROM is defined, returns empty values.
        """
        if len(self.mboard_eeprom_addr):
            (eeprom_head, eeprom_rawdata) = e31x_legacy_eeprom.read_eeprom(
                True, # isMotherboard
                get_eeprom_paths(self.mboard_eeprom_addr)[self.mboard_eeprom_path_index],
                self.mboard_eeprom_offset,
                e31x_legacy_eeprom.MboardEEPROM.eeprom_header_format,
                e31x_legacy_eeprom.MboardEEPROM.eeprom_header_keys,
                self.mboard_eeprom_max_len
            )
            self.log.trace("Found EEPROM metadata: `{}'"
                           .format(str(eeprom_head)))
            self.log.trace("Read {} bytes of EEPROM data."
                           .format(len(eeprom_rawdata)))
            return eeprom_head, eeprom_rawdata
        # Nothing defined? Return defaults.
        self.log.trace("No mboard EEPROM path defined. "
                       "Skipping mboard EEPROM readout.")
        return {}, b''

    def _get_dboard_eeprom_info(self):
        """
        Read back EEPROM info from the daughterboards
        """
        if self.dboard_eeprom_addr is None:
            self.log.debug("No dboard EEPROM addresses given.")
            return []
        dboard_eeprom_addrs = self.dboard_eeprom_addr \
                              if isinstance(self.dboard_eeprom_addr, list) \
                              else [self.dboard_eeprom_addr]
        dboard_eeprom_paths = []
        self.log.trace("Identifying dboard EEPROM paths from addrs `{}'..."
                       .format(",".join(dboard_eeprom_addrs)))
        for dboard_eeprom_addr in dboard_eeprom_addrs:
            self.log.trace("Resolving %s...", dboard_eeprom_addr)
            dboard_eeprom_paths += get_eeprom_paths(dboard_eeprom_addr)
        self.log.trace("Found dboard EEPROM paths: {}"
                       .format(",".join(dboard_eeprom_paths)))
        if len(dboard_eeprom_paths) > self.max_num_dboards:
            self.log.warning("Found more EEPROM paths than daughterboards. "
                             "Ignoring some of them.")
            dboard_eeprom_paths = dboard_eeprom_paths[:self.max_num_dboards]
        dboard_info = []
        for dboard_idx, dboard_eeprom_path in enumerate(dboard_eeprom_paths):
            self.log.debug("Reading EEPROM info for dboard %d...", dboard_idx)
            dboard_eeprom_md, dboard_eeprom_rawdata = e31x_legacy_eeprom.read_eeprom(
                False, # is not motherboard.
                dboard_eeprom_path,
                self.dboard_eeprom_offset,
                e31x_legacy_eeprom.DboardEEPROM.eeprom_header_format,
                e31x_legacy_eeprom.DboardEEPROM.eeprom_header_keys,
                self.dboard_eeprom_max_len
            )
            self.log.trace("Found dboard EEPROM metadata: `{}'"
                           .format(str(dboard_eeprom_md)))
            self.log.trace("Read %d bytes of dboard EEPROM data.",
                           len(dboard_eeprom_rawdata))
            db_pid = dboard_eeprom_md.get('pid')
            if db_pid is None:
                self.log.warning("No dboard PID found in dboard EEPROM!")
            else:
                self.log.debug("Found dboard PID in EEPROM: 0x{:04X}"
                               .format(db_pid))
            dboard_info.append({
                'eeprom_md': dboard_eeprom_md,
                'eeprom_rawdata': dboard_eeprom_rawdata,
                'pid': db_pid,
            })
        return dboard_info

    ###########################################################################
    # Session init and deinit
    ###########################################################################
    def claim(self):
        """
        Fully initializes a device when the rpc_server claim()
        gets called to revive the device from idle state to be used
        by an UHD application
        """
        super(e31x, self).claim()
        try:
             self._init_normal()
        except Exception as ex:
                self.log.error("e31x claim() failed: %s", str(ex))

    def init(self, args):
        """
        Calls init() on the parent class, and then programs the Ethernet
        dispatchers accordingly.
        """
        if not self._device_initialized:
            self.log.warning(
                "Cannot run init(), device was never fully initialized!")
            return False
        if args.get("clock_source", "") != "":
            self.set_clock_source(args.get("clock_source"))
        if args.get("time_source", "") != "":
            self.set_time_source(args.get("time_source"))
        if "no_reload_fpga" in args:
            self._do_not_reload = str2bool(args.get("no_reload_fpga")) or args.get("no_reload_fpga") == ""
        result = super(e31x, self).init(args)
        for xport_mgr in itervalues(self._xport_mgrs):
            xport_mgr.init(args)
        return result


    def apply_idle_overlay(self):
        """
        Load all overlays required to go into idle power savings mode.
        """
        idle_overlay = self.get_idle_dt_overlay(self.device_info)
        self.log.debug("Motherboard requests device tree overlay for Idle power savings mode: {}".format(
            idle_overlay
        ))
        dtoverlay.apply_overlay_safe(idle_overlay)

    def remove_idle_overlay(self):
        """
        Remove idle mode overlay.
        """
        idle_overlay = self.get_idle_dt_overlay(self.device_info)
        self.log.trace("Removing Idle overlay: {}".format(
            idle_overlay
        ))
        dtoverlay.rm_overlay(idle_overlay)

    def list_owned_overlays(self):
        """
        Lists all overlays that can be possibly applied by MPM.
        """
        all_overlays = self.list_required_dt_overlays(self.device_info)
        all_overlays.append(self.get_idle_dt_overlay(self.device_info))
        return all_overlays

    def deinit(self):
        """
        Clean up after a UHD session terminates.
        """
        if not self._device_initialized:
            self.log.warning(
                "Cannot run deinit(), device was never fully initialized!")
            return
        super(e31x, self).deinit()
        for xport_mgr in itervalues(self._xport_mgrs):
            xport_mgr.deinit()
        self.log.trace("Resetting SID pool...")
        self._available_endpoints = list(range(256))
        if not self._do_not_reload:
            self.tear_down()
        # Reset back to value from _default_args (mpm.conf)
        self._do_not_reload = str2bool(self._default_args.get("no_reload_fpga", "False"))

    def tear_down(self):
        """
        Tear down all members that need to be specially handled before
        deconstruction.
        For E310, this means the overlay.
        """
        self.log.trace("Tearing down E310 device...")
        self._tear_down = True
        self.dboards = []
        self.dboard = None
        self.mboard_regs_control = None
        self._device_initialized = False
        active_overlays = self.list_active_overlays()
        self.log.trace("E310 has active device tree overlays: {}".format(
            active_overlays
        ))
        for overlay in active_overlays:
            dtoverlay.rm_overlay(overlay)
        self.apply_idle_overlay()

    def is_idle(self):
        """
        Determine if the device is in the idle state.
        """
        active_overlays = self.list_active_overlays()
        idle_overlay = self.get_idle_dt_overlay(self.device_info)
        is_idle = idle_overlay in active_overlays
        if is_idle:
            self.log.trace("Found idle overlay: %s", idle_overlay)
        return is_idle


    ###########################################################################
    # Transport API
    ###########################################################################
    def get_chdr_link_types(self):
        """
        See PeriphManagerBase.get_chdr_link_types() for docs.
        """
        return ['liberio']

    def get_chdr_link_options(self, xport_type):
        """
        See PeriphManagerBase.get_chdr_link_options() for docs.
        """
        if xport_type == 'liberio':
            return self._xport_mgrs['liberio'].get_chdr_link_options()

    ###########################################################################
    # Device info
    ###########################################################################
    def get_device_info_dyn(self):
        """
        Append the device info with current IP addresses.
        """
        if not self._device_initialized:
            return {}
        device_info = {}
        device_info.update({
            'fpga_version': "{}.{}".format(
                *self.mboard_regs_control.get_compat_number()),
            'fpga_version_hash': "{:x}.{}".format(
                *self.mboard_regs_control.get_git_hash()),
            'fpga': self.updateable_components.get('fpga', {}).get('type', ""),
        })
        return device_info

    def set_device_id(self, device_id):
        """
        Sets the device ID for this motherboard.
        The device ID is used to identify the RFNoC components associated with
        this motherboard.
        """
        self.log.debug("Setting device ID to `{}'".format(device_id))
        self.mboard_regs_control.set_device_id(device_id)

    def get_device_id(self):
        """
        Gets the device ID for this motherboard.
        The device ID is used to identify the RFNoC components associated with
        this motherboard.
        """
        return self.mboard_regs_control.get_device_id()

    ###########################################################################
    # Clock/Time API
    ###########################################################################
    def get_clock_sources(self):
        " Lists all available clock sources. "
        self.log.trace("Listing available clock sources...")
        return ('internal',)

    def get_clock_source(self):
        " Returns the currently selected clock source "
        return self._clock_source

    def set_clock_source(self, *args):
        """
        Switch reference clock.

        Throws if clock_source is not a valid value.
        """
        clock_source = args[0]
        assert clock_source in self.get_clock_sources()
        self.log.debug("Setting clock source to `{}'".format(clock_source))
        if clock_source == self.get_clock_source():
            self.log.trace("Nothing to do -- clock source already set.")
            return
        self._clock_source = clock_source
        self.mboard_regs_control.set_clock_source(clock_source)

    def get_time_sources(self):
        " Returns list of valid time sources "
        return ['internal', 'external', 'gpsdo']

    def get_time_source(self):
        " Return the currently selected time source "
        return self._time_source

    def set_time_source(self, time_source):
        " Set a time source "
        assert time_source in self.get_time_sources()
        if time_source == self.get_time_source():
            self.log.trace("Nothing to do -- time source already set.")
            return
        self._time_source = time_source
        self.mboard_regs_control.set_time_source(time_source)

    ###########################################################################
    # Hardware peripheral controls
    ###########################################################################

    def set_fp_gpio_master(self, value):
        """set driver for front panel GPIO
        Arguments:
            value {unsigned} -- value is a single bit bit mask of 12 pins GPIO
        """
        self.mboard_regs_control.set_fp_gpio_master(value)

    def get_fp_gpio_master(self):
        """get "who" is driving front panel gpio
           The return value is a bit mask of 8 pins GPIO.
           0: means the pin is driven by PL
           1: means the pin is driven by PS
        """
        return self.mboard_regs_control.get_fp_gpio_master()

    def set_fp_gpio_radio_src(self, value):
        """set driver for front panel GPIO
        Arguments:
            value {unsigned} -- value is 2-bit bit mask of 8 pins GPIO
           00: means the pin is driven by radio 0
           01: means the pin is driven by radio 1
        """
        self.mboard_regs_control.set_fp_gpio_radio_src(value)

    def get_fp_gpio_radio_src(self):
        """get which radio is driving front panel gpio
           The return value is 2-bit bit mask of 8 pins GPIO.
           00: means the pin is driven by radio 0
           01: means the pin is driven by radio 1
        """
        return self.mboard_regs_control.get_fp_gpio_radio_src()

    def set_channel_mode(self, channel_mode):
        "Set channel mode in FPGA and select which tx channel to use"
        self.mboard_regs_control.set_channel_mode(channel_mode)

    ###########################################################################
    # Sensors
    ###########################################################################
    def get_ref_lock_sensor(self):
        """
        #TODO: Where is ref lock signal coming from?
        """
        self.log.trace("Querying ref lock status.")
        lock_status = bool(self.mboard_regs_control.get_refclk_lock())
        return {
            'name': 'ref_locked',
            'type': 'BOOLEAN',
            'unit': 'locked' if lock_status else 'unlocked',
            'value': str(lock_status).lower(),
        }

    def get_mb_temp_sensor(self):
        """
        Get temperature sensor reading of the E310.
        """
        self.log.trace("Reading temperature.")
        temp = '-1'
        raw_val = {}
        data_probes = ['temp1_input']
        try:
            for data_probe in data_probes:
                raw_val[data_probe] = read_sysfs_sensors_value('jc-42.4-temp', data_probe, 'hwmon', 'name')[0]
            temp = str(raw_val['temp1_input'] / 1000)
        except ValueError:
            self.log.warning("Error when converting temperature value")
        except KeyError:
            self.log.warning("Can't read temp on thermal_zone".format(sensor))
        return {
            'name': 'temp_mb',
            'type': 'REALNUM',
            'unit': 'C',
            'value': temp
        }

    def get_fpga_temp_sensor(self):
        """
        Get temperature sensor reading of the E310.
        """
        self.log.trace("Reading temperature.")
        temp = '-1'
        raw_val = {}
        data_probes = ['in_temp0_raw', 'in_temp0_scale', 'in_temp0_offset']
        try:
            for data_probe in data_probes:
                raw_val[data_probe] = read_sysfs_sensors_value('xadc', data_probe, 'iio', 'name')[0]
            temp = str((raw_val['in_temp0_raw'] + raw_val['in_temp0_offset']) * raw_val['in_temp0_scale'] / 1000)
        except ValueError:
            self.log.warning("Error when converting temperature value")
        except KeyError:
            self.log.warning("Can't read temp on thermal_zone".format(sensor))
        return {
            'name': 'temp_fpga',
            'type': 'REALNUM',
            'unit': 'C',
            'value': temp
        }

    ###########################################################################
    # EEPROMs
    ###########################################################################
    def get_mb_eeprom(self):
        """
        Return a dictionary with EEPROM contents.

        All key/value pairs are string -> string.

        We don't actually return the EEPROM contents, instead, we return the
        mboard info again. This filters the EEPROM contents to what we think
        the user wants to know/see.
        """
        return self.mboard_info

    def set_mb_eeprom(self, eeprom_vals):
        """
        See PeriphManagerBase.set_mb_eeprom() for docs.
        """
        self.log.warn("Called set_mb_eeprom(), but not implemented!")
        raise NotImplementedError

    def get_db_eeprom(self, dboard_idx):
        """
        See PeriphManagerBase.get_db_eeprom() for docs.
        """
        if dboard_idx != E310_DBOARD_SLOT_IDX:
            self.log.warn("Trying to access invalid dboard index {}. "
                          "Using the only dboard.".format(dboard_idx))
        db_eeprom_data = copy.copy(self.dboard.device_info)
        return db_eeprom_data

    def set_db_eeprom(self, dboard_idx, eeprom_data):
        self.log.warn("Called set_db_eeprom(), but not implemented!")
        raise NotImplementedError

    ###########################################################################
    # Component updating
    ###########################################################################
    # Note: Component updating functions defined by ZynqComponents
    @no_rpc
    def _update_fpga_type(self):
        """Update the fpga type stored in the updateable components"""
        fpga_type = self.mboard_regs_control.get_fpga_type()
        self.log.debug("Updating mboard FPGA type info to {}".format(fpga_type))
        self.updateable_components['fpga']['type'] = fpga_type

    #######################################################################
    # Timekeeper API
    #######################################################################
    def get_num_timekeepers(self):
        """
        Return the number of timekeepers
        """
        return self.mboard_regs_control.get_num_timekeepers()

    def get_timekeeper_time(self, tk_idx, last_pps):
        """
        Get the time in ticks

        Arguments:
        tk_idx: Index of timekeeper
        next_pps: If True, get time at last PPS. Otherwise, get time now.
        """
        return self.mboard_regs_control.get_timekeeper_time(tk_idx, last_pps)

    def set_timekeeper_time(self, tk_idx, ticks, next_pps):
        """
        Set the time in ticks

        Arguments:
        tk_idx: Index of timekeeper
        ticks: Time in ticks
        next_pps: If True, set time at next PPS. Otherwise, set time now.
        """
        self.mboard_regs_control.set_timekeeper_time(tk_idx, ticks, next_pps)

    def set_tick_period(self, tk_idx, period_ns):
        """
        Set the time per tick in nanoseconds (tick period)

        Arguments:
        tk_idx: Index of timekeeper
        period_ns: Period in nanoseconds
        """
        self.mboard_regs_control.set_tick_period(tk_idx, period_ns)

    def get_clocks(self):
        """
        Gets the RFNoC-related clocks present in the FPGA design
        """
        return [
            {
                'name': 'radio_clk',
                'freq': str(self.dboard.get_master_clock_rate()),
                'mutable': 'true'
            },
            {
                'name': 'bus_clk',
                'freq': str(100e6),
            }
        ]
