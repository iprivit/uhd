"""
Copyright 2019 Ettus Research, A National Instrument Brand

SPDX-License-Identifier: GPL-3.0-or-later

RFNoC image builder: All the algorithms required to turn either a YAML
description or a GRC file into an rfnoc_image_core.v file.
"""

from collections import deque
from collections import OrderedDict

import logging
import os
import re
import sys

import six
import mako.lookup
import mako.template
from ruamel import yaml


def split(iterable, function):
    """
    Split an iterable by condition. Matching items are returned in the first
    deque of the returned tuple unmatched in the second
    :param iterable: an iterable to split
    :param function: an expression that returns True/False for iterable values
    :return: 2-tuple with deque for matching/non-matching items
    """
    dq_true = deque()
    dq_false = deque()

    deque(((dq_true if function(item) else dq_false).append(item)
           for item in iterable), maxlen=0)

    return dq_true, dq_false


def expand_io_port_desc(io_ports, signatures):
    """
    Add a wires entry to each io port dictionary entry which contains a
    complete list of wires for the specific port according to the information
    in signature file. Each wire entry contains:
    * fully resolved wire name (wire name as in signature or replaced name
      with respect to regular expression if some is given, regular expression
      should use back references to retain original wire name).
    * width in bits
    * direction as input/output depending on whether the port is a
      master/broadcaster or slave/listener and the wire is described as from
      or to master
    :param io_ports: io port dictionary from yml configuration
    :param signatures: signature description from yml configuration
    :return: None
    """
    for name, io_port in six.iteritems(io_ports):
        wires = []
        for signature in signatures[io_port["type"]]["ports"]:
            width = signature.get("width", 1)
            wire_type = signature.get("type", None)
            drive = io_port["drive"]
            direction = {"master": {"from-master": "input ", "to-master": "output"},
                         "slave":  {"from-master": "output", "to-master": "input "},
                         "broadcaster":  {None: "input "},
                         "listener":  {None: "output"}}[drive][wire_type]

            signature_name = signature["name"]
            if "rename" in io_port:
                signature_name = re.sub(io_port["rename"]["pattern"],
                                        io_port["rename"]["repl"],
                                        signature_name, 1)

            wires.append({"direction": direction,
                          "width": width,
                          "name": signature_name})
        io_port["wires"] = wires

# pylint: disable=too-few-public-methods
class IOConfig:
    """
    Class containing configuration from a yml file.

    Each top level entry is translated into a class member variable. If the
    configuration contains an io_ports section the ports get an wire list which
    is derived from the signature file. This allows easier processing of IO
    ports in the mako templates and failures from yml configuration files fail
    in this script rather than during template processing which is easier to
    track and debug.
    """
    def __init__(self, config, signatures):
        # read configuration from config dictionary
        self.__dict__.update(**config)
        if hasattr(self, "io_ports"):
            expand_io_port_desc(getattr(self, "io_ports"), signatures)


class ImageBuilderConfig:
    """
    Holds the complete image configuration settings. This includes
    * the image configuration itself as it is passed to the script
    * all noc block configurations found by the script
    * device configuration information as found in the bsp.yml of the device
      information passed to the script.
    """
    # pylint: disable=too-many-instance-attributes
    def __init__(self, config, blocks, device):
        self.noc_blocks = OrderedDict()
        self.stream_endpoints = OrderedDict()
        self.connections = []
        self.clk_domains = []
        self.block_ports = {}
        self.io_ports = OrderedDict()
        self.clocks = OrderedDict()
        self.block_con = []
        self.io_port_con_ms = []
        self.io_port_con_bl = []
        self.clk_domain_con = []
        # read configuration from config dictionary
        self.__dict__.update(**config)
        self.blocks = blocks
        self.device = device
        self._update_sep_defaults()
        self._set_indices()
        self._collect_noc_ports()
        self._collect_io_ports()
        self._collect_clocks()
        self.pick_connections()
        self.pick_clk_domains()

    def _update_sep_defaults(self):
        """
        Update any missing stream endpoint attributes with default values
        """
        for sep in self.stream_endpoints:
            if "num_data_i" not in self.stream_endpoints[sep]:
                self.stream_endpoints[sep]["num_data_i"] = 1
            if "num_data_o" not in self.stream_endpoints[sep]:
                self.stream_endpoints[sep]["num_data_o"] = 1

    def _set_indices(self):
        """
        Add an index for each port of each stream endpoint and noc block.
        These indices are used to generate static_router.hex
        """
        start = 1
        i = 0
        for i, sep in enumerate(self.stream_endpoints.values()):
            sep["index"] = i + start
        start = start + i + 1
        for i, block in enumerate(self.noc_blocks.values()):
            block["index"] = start + i

    def _collect_noc_ports(self):
        """
        Create lookup table for noc blocks. The key is a tuple of block
        name, port name and flow direction.
        """
        for name, block in six.iteritems(self.noc_blocks):
            desc = self.blocks[block["block_desc"]]
            ports = desc.data["inputs"]
            self.block_ports.update({(name, port, "input"):
                                     ports[port] for port in ports})
            ports = desc.data["outputs"]
            self.block_ports.update({(name, port, "output"):
                                     ports[port] for port in ports})
            # Update per-instance parameters
            if not hasattr(desc, "parameters"):
                setattr(desc, "parameters", {})
            if "parameters" not in block:
                block["parameters"] = {}
            for key in block["parameters"].keys():
                if key not in desc.parameters:
                    logging.error("Unknown parameter %s for block %s", key, name)
                    del block["parameters"][key]
            for param, value in six.iteritems(desc.parameters):
                if param not in block["parameters"]:
                    block["parameters"][param] = value
        ports = self.stream_endpoints
        for sep in self.stream_endpoints:
            inputs = {(sep, "in%d" % port, "input") :
                      ports[sep] for port in range(ports[sep]["num_data_i"])}
            self.block_ports.update(inputs)
            outputs = {(sep, "out%d" % port, "output") :
                       ports[sep] for port in range(ports[sep]["num_data_o"])}
            self.block_ports.update(outputs)

    def _collect_io_ports(self):
        """
        Create lookup table for io ports. The key is a tuple of block name
        (_device_ for io ports of the bsp), the io port name and flow
        direction.
        """
        for name, block in six.iteritems(self.noc_blocks):
            desc = self.blocks[block["block_desc"]]
            if hasattr(desc, "io_ports"):
                self.io_ports.update({
                    (name, io, desc.io_ports[io]["drive"]):
                    desc.io_ports[io] for io in desc.io_ports})
        self.io_ports.update({
            ("_device_", io, self.device.io_ports[io]["drive"]):
            self.device.io_ports[io] for io in self.device.io_ports})

    def _collect_clocks(self):
        """
        Create lookup table for clocks. The key is a tuple of block name
        (_device_ for clocks of the bsp), the clock name and flow
        direction
        """
        for name, block in six.iteritems(self.noc_blocks):
            desc = self.blocks[block["block_desc"]]
            if hasattr(desc, "clocks"):
                self.clocks.update({
                    (name, clk["name"]): clk for clk in desc.clocks})
        if hasattr(self.device, "clocks"):
            self.clocks.update({
                ("_device_", clk["name"]): clk for clk in self.device.clocks})
        # Add the implied clocks for the BSP
        self.clocks[("_device_", "rfnoc_ctrl")] = {"freq": '[]', "name": "rfnoc_ctrl"}
        self.clocks[("_device_", "rfnoc_chdr")] = {"freq": '[]', "name": "rfnoc_chdr"}

    def pick_clk_domains(self):
        """
        Filter clock domain list into a local list for easier access.
        Remaining connection items are printed as error and execution is
        aborted. Likewise, checks for unconnected clocks.
        """
        (self.clk_domain_con, self.clk_domains) = split(
            self.clk_domains, lambda con:
            (con["srcblk"], con["srcport"]) in self.clocks and
            (con["dstblk"], con["dstport"]) in self.clocks)

        # Check if there are unconnected clocks
        connected = [(con["dstblk"], con["dstport"]) for con in self.clk_domain_con]
        unconnected = []
        for clk in self.clocks:
            if clk[0] != "_device_" and \
               clk[1] not in ["rfnoc_ctrl", "rfnoc_chdr"] and \
               clk not in connected:
                unconnected.append(clk)
        if unconnected:
            logging.error("%d unresolved clk domain(s)", len(unconnected))
            for clk in unconnected:
                logging.error("    %s:%s", clk[0], clk[1])
            logging.error("Please specify the clock(s) to connect")
            sys.exit(1)

        if self.clk_domains:
            logging.error("%d Unresolved clk domain(s)", len(self.clk_domains))

            for connection in self.clk_domains:
                logging.error("    (%s-%s -> %s-%s)",
                              connection["srcblk"], connection["srcport"],
                              connection["dstblk"], connection["dstport"])
            logging.error("Source or destination domain not found")
            sys.exit(1)

    def pick_connections(self):
        """
        Sort connection list into three local lists for
         * input => output (block port to block port)
         * master => slave (io port to io port)
         * broadcaster => listener (io port to io port)
        Remaining connection items are printed as error and execution is
        aborted. Possible reasons are
         * undeclared block or io port
         * connection direction wrong (e.g. output => input)
         * mixed connection type (e.g. master => listener)
        """
        block_types = lambda type: filter(lambda key: key[2] == type, self.block_ports)
        io_types = lambda type: filter(lambda key: key[2] == type, self.io_ports)
        (self.block_con, self.connections) = split(
            self.connections, lambda con:
            (con["srcblk"], con["srcport"], "output") in block_types("output") and
            (con["dstblk"], con["dstport"], "input") in block_types("input"))
        (self.io_port_con_ms, self.connections) = split(
            self.connections, lambda con:
            (con["srcblk"], con["srcport"], "master") in io_types("master") and
            (con["dstblk"], con["dstport"], "slave") in  io_types("slave"))
        (self.io_port_con_bl, self.connections) = split(
            self.connections, lambda con:
            (con["srcblk"], con["srcport"], "broadcaster") in io_types("broadcaster") and
            (con["dstblk"], con["dstport"], "listener") in io_types("listener"))

        if self.connections:
            logging.error("%d Unresolved connection(s)", len(self.connections))

            for connection in self.connections:
                logging.error("    (%s-%s -> %s-%s)",
                              connection["srcblk"], connection["srcport"],
                              connection["dstblk"], connection["dstport"])
            logging.debug("    Make sure block ports are connected output "
                          "(src) to input (dst)")
            logging.debug("    Available block ports for connections:")
            for block in self.block_ports:
                logging.debug("        %s", (block,))
            logging.debug("    Make sure io ports are connected master      "
                          "(src) to slave    (dst)")
            logging.debug("                                  or broadcaster "
                          "(src) to listener (dst)")
            logging.debug("    Available io ports for connections:")
            for io_port in self.io_ports:
                logging.info("        %s", (io_port,))
            sys.exit(1)

def load_config(filename):
    """
    Loads yml configuration from filename.

    Configuration files are searched in folder returned by get_get_config_path.
    This method logs error and exits on IO failure

    :param filename: yml configuration to load
    :return: IO signatures as dictionary
    """
    dirname, basename = os.path.split(filename)
    try:
        with open(filename) as stream:
            logging.info(
                "Using %s from %s.", basename, os.path.normpath(dirname))
            config = yaml.safe_load(stream)
        return config
    except IOError:
        logging.error("%s misses %s", os.path.normpath(dirname), basename)
        sys.exit(1)

def device_config(config_path, device):
    """
    Load device config from bsp.yml

    Location of bsp.yml is derived from the device chosen in the arguments

    :param config_path: location of core configuration files
    :param device: device to build for
    :return: device configuration as dictionary
    """
    return load_config(os.path.join(config_path, "%s_bsp.yml" % device.lower()))


def io_signatures(config_path):
    """
    Load IO signatures from io_signatures.yml

    :param config_path: location of core configuration files
    :return: IO signatures as dictionary
    """
    return load_config(os.path.join(config_path, "io_signatures.yml"))


def read_grc_block_configs(path):
    """
    Reads RFNoC config block used by Gnuradio Companion
    :param path: location of grc block configuration files
    :return: dictionary of block (id mapped to description)
    """
    result = {}

    for root, dirs, names in os.walk(path):
        for name in names:
            if re.match(r".*\.block\.yml", name):
                with open (os.path.join(root, name)) as stream:
                    config = yaml.safe_load(stream)
                    result[config["id"]] = config

    return result


def convert_to_image_config(grc, grc_config_path):
    """
    Converts Gnuradio Companion grc into image configuration.
    :param grc:
    :return: image configuration as it would be returned by image_config(args)
    """
    grc_blocks = read_grc_block_configs(grc_config_path)
    #filter all blocks that have no block representation
    all = {item["name"]: item for item in grc["blocks"]}
    seps = {item["name"]: item for item in grc["blocks"] if item["parameters"]["type"] == 'sep'}
    blocks = {item["name"]: item for item in grc["blocks"] if item["parameters"]["type"] == 'block'}
    device = [item for item in grc["blocks"] if item["parameters"]["type"] == 'device']
    if len(device) == 1:
        device = device[0]
    else:
        logging.error("More than one or no device found in grc file")
        return None

    result = {
        "schema": "rfnoc_imagebuilder",
        "copyright": "Ettus Research, A National Instruments Brand",
        "license": "SPDX-License-Identifier: LGPL-3.0-or-later",
        "version": 1.0,
        "rfnoc_version": 1.0}
    # for param in [item for item in grc["blocks"] if item["id"] == "parameter"]:
    #     result[param["name"]] = {
    #         "str": lambda value: str,
    #         "": lambda value: str,
    #         "complex": str,
    #         "intx": int,
    #         "long": int,
    #     }[param["parameters"]["type"]](param["parameters"]["value"])

    result["stream_endpoints"] = {}
    for sep in seps.values():
        result["stream_endpoints"][sep["name"]] = { "ctrl": bool(sep["parameters"]["ctrl"]),
                                                    "data": bool(sep["parameters"]["data"]),
                                                    "buff_size": int(sep["parameters"]["buff_size"]) }

    result["noc_blocks"] = {}
    for block in blocks.values():
        result["noc_blocks"][block["name"]] = { "block_desc": block["parameters"]["desc"] }

    device_clocks = {port["id"]: port for port in grc_blocks[device['id']]["outputs"] if port["dtype"] == "message"}

    for connection in grc["connections"]:
        if connection[0] == device["name"]:
            connection[0] = "_device_"
        if connection[2] == device["name"]:
            connection[2] = "_device_"
    device["name"] = "_device_"

    (clk_connections, connections) = split(
        grc["connections"], lambda con:
        con[0] == device["name"] and con[1] in device_clocks)

    result["connections"] = []
    for connection in connections:
        result["connections"].append(
            {"srcblk":  connection[0],
             "srcport": connection[1],
             "dstblk":  connection[2],
             "dstport": connection[3]}
        )

    result["clk_domains"] = []
    for connection in clk_connections:
        result["clk_domains"].append(
            {"srcblk":  connection[0],
             "srcport": connection[1],
             "dstblk":  connection[2],
             "dstport": connection[3]}
        )

    return result


def collect_module_paths(config_path):
    """
    Create a list of directorties that contain noc block configuration files.
    :param config_path: root path holding configuration files
    :return: list of noc block directories
    """
    # rfnoc blocks
    result = [os.path.join(config_path, 'rfnoc', 'blocks')]
    # additional OOT blocks
    # TODO parse modules from external includes as well
    return result


def read_block_descriptions(signatures, *paths):
    """
    Recursive search all pathes for block definitions.
    :param signatures: signature passed to IOConfig initialization
    :param paths: paths to be searched
    :return: dictionary of noc blocks. Key is filename of the block, value
             is an IOConfig object
    """
    blocks = {}
    for path in paths:
        for root, dirs, files, in os.walk(path):
            for filename in files:
                if re.match(r".*\.yml$", filename):
                    with open(os.path.join(root, filename)) as stream:
                        block = yaml.safe_load(stream)
                        if "schema" in block and \
                                block["schema"] == "rfnoc_modtool_args":
                            logging.info("Adding block description from "
                                         "%s (%s).", filename, os.path.normpath(root))
                            blocks[filename] = IOConfig(block, signatures)
            for dirname in dirs:
                blocks.update(read_block_descriptions(
                    os.path.join(root, dirname)))
    return blocks


def write_edges(config, destination):
    """
    Write edges description files. The file is a simple text file. Each line
    contains 8 hexadecimal digits.
    First line is the number of following entries.
    Starting with the second line each line describes a port to port connection
    The 32 bit value has 16 bit for each node where the node is represented by
    10 bit for the block number and 6 bit for the port number.
    :param config: ImageBuilderConfig derived from script parameter
    :param destination: folder to write the file (next to device top level files
    :return: None
    """
    logging.info("Writing static routing table to %s", destination)
    with open(destination, "w") as stream:
        stream.write("%08X\n" % len(config.block_con))
        for connection in config.block_con:
            if connection["srcblk"] in config.stream_endpoints:
                sep = config.stream_endpoints[connection["srcblk"]]
                index_match = re.match(r"out(\d)", connection["srcport"])
                if not index_match:
                    logging.error("Port %s is invalid on endpoint %s",
                                  connection["srcport"], connection["srcblk"])
                port_index = int(index_match.group(1))
                # Verify index < num_data_o
                if port_index >= sep["num_data_o"]:
                    logging.error("Port %s exceeds num_data_o for endpoint %s",
                                  connection["srcport"], connection["srcblk"])
                src = (sep["index"], port_index)
            else:
                key = (connection["srcblk"], connection["srcport"], "output")
                src = (config.noc_blocks[connection["srcblk"]]["index"],
                       config.block_ports[key]["index"])
            if connection["dstblk"] in config.stream_endpoints:
                sep = config.stream_endpoints[connection["dstblk"]]
                index_match = re.match(r"in(\d)", connection["dstport"])
                if not index_match:
                    logging.error("Port %s is invalid on endpoint %s",
                                  connection["dstport"], connection["dstblk"])
                # Verify index < num_data_i
                port_index = int(index_match.group(1))
                if port_index >= sep["num_data_i"]:
                    logging.error("Port %s exceeds num_data_i for endpoint %s",
                                  connection["dstport"], connection["dstblk"])
                dst = (sep["index"], port_index)
            else:
                key = (connection["dstblk"], connection["dstport"], "input")
                dst = (config.noc_blocks[connection["dstblk"]]["index"],
                       config.block_ports[key]["index"])
            logging.debug("%s-%s (%d,%d) => %s-%s (%d,%d)",
                          connection["srcblk"], connection["srcport"],
                          src[0], src[1],
                          connection["dstblk"], connection["dstport"],
                          dst[0], dst[1])
            stream.write("%08x\n" %
                         ((((src[0] << 6) | src[1]) << 16) |
                          ((dst[0] << 6) | dst[1])))


def write_verilog(config, destination, source, source_hash):
    """
    Generates rfnoc_image_core.v file for the device.

    Mako templates from local template folder are used to generate the image
    core file. The template engine does not do any computation on the script
    parameter. Instead all necessary dependencies are resolved in this script
    to enforce early failure which is easier to track than errors in the
    template engine.
    :param config: ImageBuilderConfig derived from script parameter
    :param destination: Filepath to write to
    :return: None
    """
    template_dir = os.path.join(os.path.dirname(__file__), "templates")
    lookup = mako.lookup.TemplateLookup(directories=[template_dir])
    tpl_filename = os.path.join(template_dir, "rfnoc_image_core.v.mako")
    tpl = mako.template.Template(
        filename=tpl_filename,
        lookup=lookup,
        strict_undefined=True)

    block = tpl.render(**{
        "config": config,
        "source": source,
        "source_hash": source_hash,
        })

    logging.info("Writing image core to %s", destination)
    with open(destination, "w") as image_core_file:
        image_core_file.write(block)


def write_build_env():
    """
    # TODO update Makefile entries according to used blocks
    :return:
    """


def build(fpga_path, device, image_core_path, edge_file, **args):
    """
    Call FPGA toolchain to actually build the image

    :param fpga_path: A path that holds the FPGA IP sources.
    :param device: The device to build for.
    :param **args: Additional options
                   target: The target to build (leave empty for default).
                   clean_all: passed to Makefile
                   GUI: passed to Makefile
                   source: The source of the build (YAML or GRC file path)
    :return: exit value of build process
    """
    ret_val = 0
    cwd = os.path.dirname(__file__)
    build_dir = os.path.join(get_top_path(fpga_path), target_dir(device))
    if not os.path.isdir(build_dir):
        logging.error("Not a valid directory: %s", build_dir)
        return 1
    logging.debug("Changing temporarily working directory to %s", build_dir)
    os.chdir(build_dir)
    make_cmd = ". ./setupenv.sh "
    if "clean_all" in args and args["clean_all"]:
        make_cmd = make_cmd + "&& make cleanall "
    target = args["target"] if "target" in args else ""
    make_cmd = make_cmd + "&& make " + default_target(device, target)
    make_cmd += " IMAGE_CORE={} EDGE_FILE={}".format(image_core_path,
                                                     edge_file)
    if "GUI" in args and args["GUI"]:
        make_cmd = make_cmd + " GUI=1"
    logging.info("Launching build with the following settings:")
    logging.info(" * Build Directory: %s", build_dir)
    logging.info(" * Target: %s", target)
    logging.info(" * Image Core File: %s", image_core_path)
    logging.info(" * Edge Table File: %s", edge_file)
    logging.debug("Calling build with '%s'", make_cmd)
    # Wrap it into a bash call:
    make_cmd = '/bin/bash -c "{0}"'.format(make_cmd)
    ret_val = os.system(make_cmd)
    os.chdir(cwd)
    return ret_val


def target_dir(device):
    """
    Target directory derived from chosen device
    :param device: device to build for
    :return: target directory (relative path)
    """
    target_dir_lookup = {
        'x300': 'x300',
        'x310': 'x300',
        'e300': 'e300',
        'e310': 'e31x',
        'e320': 'e320',
        'n300': 'n3xx',
        'n310': 'n3xx',
        'n320': 'n3xx',
    }
    if not device.lower() in target_dir_lookup:
        logging.error("Unsupported device %s. Supported devices are %s",
                      device, target_dir_lookup.keys())
        sys.exit(1)
    return target_dir_lookup[device.lower()]


def default_target(device, target):
    """
    If no target specified, selects the default building target based on the
    targeted device
    """
    if target is None:
        default_target_lookup = {
            'x300': 'X300_HG',
            'x310': 'X310_HG',
            'e310': 'E310_SG3',
            'e320': 'E320_1G',
            'n300': 'N300_HG',
            'n310': 'N310_HG',
            'n320': 'N320_XG',
        }
        return default_target_lookup[device.lower()]

    return target


def get_top_path(fpga_root):
    """
    returns the path where FPGA top level sources reside
    """
    return os.path.join(fpga_root, 'usrp3', 'top')


def get_core_config_path(config_path):
    """
    returns the path where core configuration files are stored
    """
    return os.path.join(config_path, 'rfnoc', 'core')

def generate_image_core_path(output_path, source):
    """
    Creates the path where the image core file gets to be stored.

    output_path: If not None, this is returned
    source: Otherwise, this path is returned with a .v argument
    """
    if output_path is not None:
        return output_path
    source = os.path.splitext(os.path.normpath(source))[0]
    return source + '.v'

def generate_edge_file_path(output_path, device, source):
    """
    Creates a valid path for the edge file to get stored.
    """
    if output_path is not None:
        return output_path
    edge_path = os.path.split(os.path.abspath(os.path.normpath(source)))[0]
    return os.path.join(edge_path, "{}_static_router.hex".format(device))


def build_image(config, fpga_path, config_path, device, **args):
    """
    Generate image dependent Verilog code and trigger Xilinx toolchain, if
    requested.

    :param config: A dictionary containing the image configuration options.
                   This must obey the rfnoc_imagebuilder_args schema.
    :param fpga_path: A path that holds the FPGA IP sources.
    :param device: The device to build for.
    :param **args: Additional options including
                   target: The target to build (leave empty for default).
                   generate_only: Do not build the code after generation.
                   clean_all: passed to Makefile
                   GUI: passed to Makefile
    :return: Exit result of build process or 0 if generate-only is given.
    """
    logging.info("Selected device %s", device)
    image_core_path = \
        generate_image_core_path(args.get('output_path'), args.get('source'))
    edge_file = \
        generate_edge_file_path(
            args.get('router_hex_path'), device, args.get('source'))

    logging.debug("Image core output file: %s", image_core_path)
    logging.debug("Edge output file: %s", edge_file)

    core_config_path = get_core_config_path(config_path)
    signatures_conf = io_signatures(core_config_path)
    device_conf = IOConfig(device_config(core_config_path, device),
                           signatures_conf)

    block_paths = collect_module_paths(config_path)
    logging.debug("Looking for block descriptors in:")
    for path in block_paths:
        logging.debug("    %s", os.path.normpath(path))
    blocks = read_block_descriptions(signatures_conf, *block_paths)

    builder_conf = ImageBuilderConfig(config, blocks, device_conf)

    write_edges(builder_conf, edge_file)
    write_verilog(
        builder_conf,
        image_core_path,
        source=args.get('source'),
        source_hash=args.get('source_hash'))
    write_build_env()

    if "generate_only" in args and args["generate_only"]:
        logging.info("Skip build (generate only option given)")
        return 0

    return build(fpga_path, device, image_core_path, edge_file, **args)
