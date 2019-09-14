//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "x300_adc_ctrl.hpp"
#include "x300_dac_ctrl.hpp"
#include "x300_dboard_iface.hpp"
#include "x300_device_args.hpp"
#include "x300_mb_controller.hpp"
#include "x300_radio_mbc_iface.hpp"
#include "x300_regs.hpp"
#include <uhd/rfnoc/registry.hpp>
#include <uhd/types/direction.hpp>
#include <uhd/types/wb_iface.hpp>
#include <uhd/usrp/dboard_eeprom.hpp>
#include <uhd/usrp/dboard_manager.hpp>
#include <uhd/utils/gain_group.hpp>
#include <uhd/utils/log.hpp>
#include <uhd/utils/math.hpp>
#include <uhd/utils/soft_register.hpp>
#include <uhdlib/rfnoc/radio_control_impl.hpp>
#include <uhdlib/rfnoc/reg_iface_adapter.hpp>
#include <uhdlib/usrp/common/apply_corrections.hpp>
#include <uhdlib/usrp/cores/gpio_atr_3000.hpp>
#include <uhdlib/usrp/cores/rx_frontend_core_3000.hpp>
#include <uhdlib/usrp/cores/spi_core_3000.hpp>
#include <uhdlib/usrp/cores/tx_frontend_core_200.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/make_shared.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

using namespace uhd;
using namespace uhd::usrp;
using namespace uhd::rfnoc;

namespace {

std::vector<uint8_t> str_to_bytes(std::string str)
{
    return std::vector<uint8_t>(str.cbegin(), str.cend());
}

std::string bytes_to_str(std::vector<uint8_t> str_b)
{
    return std::string(str_b.cbegin(), str_b.cend());
}

gain_fcns_t make_gain_fcns_from_subtree(property_tree::sptr subtree)
{
    gain_fcns_t gain_fcns;
    gain_fcns.get_range = [subtree]() {
        return subtree->access<meta_range_t>("range").get();
    };
    gain_fcns.get_value = [subtree]() { return subtree->access<double>("value").get(); };
    gain_fcns.set_value = [subtree](const double gain) {
        subtree->access<double>("value").set(gain);
    };
    return gain_fcns;
}

template <typename map_type>
size_t _get_chan_from_map(std::unordered_map<size_t, map_type> map, const std::string& fe)
{
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (it->second.db_fe_name == fe) {
            return it->first;
        }
    }
    throw uhd::lookup_error(
        str(boost::format("Invalid daughterboard frontend name: %s") % fe));
}

constexpr double DEFAULT_RATE = 200e6;

} // namespace

namespace x300_regs {

static constexpr uint32_t PERIPH_BASE       = 0x80000;
static constexpr uint32_t PERIPH_REG_OFFSET = 8;

// db_control registers
static constexpr uint32_t SR_MISC_OUTS = PERIPH_BASE + 160 * PERIPH_REG_OFFSET;
static constexpr uint32_t SR_SPI       = PERIPH_BASE + 168 * PERIPH_REG_OFFSET;
static constexpr uint32_t SR_LEDS      = PERIPH_BASE + 176 * PERIPH_REG_OFFSET;
static constexpr uint32_t SR_FP_GPIO   = PERIPH_BASE + 184 * PERIPH_REG_OFFSET;
static constexpr uint32_t SR_DB_GPIO   = PERIPH_BASE + 192 * PERIPH_REG_OFFSET;

static constexpr uint32_t RB_MISC_IO = PERIPH_BASE + 16 * PERIPH_REG_OFFSET;
static constexpr uint32_t RB_SPI     = PERIPH_BASE + 17 * PERIPH_REG_OFFSET;
static constexpr uint32_t RB_LEDS    = PERIPH_BASE + 18 * PERIPH_REG_OFFSET;
static constexpr uint32_t RB_DB_GPIO = PERIPH_BASE + 19 * PERIPH_REG_OFFSET;
static constexpr uint32_t RB_FP_GPIO = PERIPH_BASE + 20 * PERIPH_REG_OFFSET;


//! Delta between frontend offsets for channel 0 and 1
constexpr uint32_t SR_FE_CHAN_OFFSET = 16 * PERIPH_REG_OFFSET;
constexpr uint32_t SR_TX_FE_BASE     = PERIPH_BASE + 208 * PERIPH_REG_OFFSET;
constexpr uint32_t SR_RX_FE_BASE     = PERIPH_BASE + 224 * PERIPH_REG_OFFSET;

} // namespace x300_regs

class x300_radio_control_impl : public radio_control_impl,
                                public uhd::usrp::x300::x300_radio_mbc_iface
{
public:
    RFNOC_RADIO_CONSTRUCTOR(x300_radio_control)
    , _radio_type(get_block_id().get_block_count() == 0 ? PRIMARY : SECONDARY)
    {
        RFNOC_LOG_TRACE("Initializing x300_radio_control, slot "
                        << x300_radio_control_impl::get_slot_name());
        UHD_ASSERT_THROW(get_mb_controller());
        _x300_mb_control =
            std::dynamic_pointer_cast<x300_mb_controller>(get_mb_controller());
        UHD_ASSERT_THROW(_x300_mb_control);
        _x300_mb_control->register_radio(this);
        // MCR is locked for this session
        _master_clock_rate = _x300_mb_control->get_clock_ctrl()->get_master_clock_rate();
        UHD_ASSERT_THROW(get_tick_rate() == _master_clock_rate);
        radio_control_impl::set_rate(_master_clock_rate);

        ////////////////////////////////////////////////////////////////
        // Setup peripherals
        ////////////////////////////////////////////////////////////////
        // The X300 only requires a single timed_wb_iface, even for TwinRX
        _wb_iface = RFNOC_MAKE_WB_IFACE(0, 0);

        RFNOC_LOG_TRACE("Creating SPI interface...");
        _spi = spi_core_3000::make(
            [this](const uint32_t addr, const uint32_t data) {
                regs().poke32(addr, data, get_command_time(0));
            },
            [this](
                const uint32_t addr) { return regs().peek32(addr, get_command_time(0)); },
            x300_regs::SR_SPI,
            8,
            x300_regs::RB_SPI);
        // DAC/ADC
        RFNOC_LOG_TRACE("Running init_codec...");
        // Note: ADC calibration and DAC sync happen in x300_mb_controller
        _init_codecs();
        _x300_mb_control->register_reset_codec_cb([this]() { this->reset_codec(); });
        // FP-GPIO
        if (_radio_type == PRIMARY) {
            RFNOC_LOG_TRACE("Creating FP-GPIO interface...");
            _fp_gpio = gpio_atr::gpio_atr_3000::make(_wb_iface,
                x300_regs::SR_FP_GPIO,
                x300_regs::RB_FP_GPIO,
                x300_regs::PERIPH_REG_OFFSET);
            // Create the GPIO banks and attributes, and populate them with some default
            // values
            // TODO: Do we need this section? Since the _fp_gpio handles state now, we
            // don't need to stash values here. We only need this if we want to set
            // anything to a default value.
            for (const gpio_atr::gpio_attr_map_t::value_type attr :
                gpio_atr::gpio_attr_map) {
                // TODO: Default values?
                if (attr.first == usrp::gpio_atr::GPIO_SRC) {
                    // Don't set the SRC
                    // TODO: Remove from the map??
                    continue;
                }
                set_gpio_attr("FP0", usrp::gpio_atr::gpio_attr_map.at(attr.first), 0);
            }
        }
        // DB Initialization
        _init_db(); // This does not init the dboards themselves!

        // LEDs are technically valid for both RX and TX, but let's put them
        // here
        _leds = gpio_atr::gpio_atr_3000::make_write_only(
            _wb_iface, x300_regs::SR_LEDS, x300_regs::PERIPH_REG_OFFSET);
        _leds->set_atr_mode(
            usrp::gpio_atr::MODE_ATR, usrp::gpio_atr::gpio_atr_3000::MASK_SET_ALL);
        // We always want to initialize at least one frontend core for both TX and RX
        // RX periphs
        for (size_t i = 0; i < std::max<size_t>(get_num_output_ports(), 1); i++) {
            _rx_fe_map[i].core = rx_frontend_core_3000::make(_wb_iface,
                x300_regs::SR_RX_FE_BASE + i * x300_regs::SR_FE_CHAN_OFFSET,
                x300_regs::PERIPH_REG_OFFSET);
            _rx_fe_map[i].core->set_adc_rate(
                _x300_mb_control->get_clock_ctrl()->get_master_clock_rate());
            _rx_fe_map[i].core->set_dc_offset(
                rx_frontend_core_3000::DEFAULT_DC_OFFSET_VALUE);
            _rx_fe_map[i].core->set_dc_offset_auto(
                rx_frontend_core_3000::DEFAULT_DC_OFFSET_ENABLE);
            _rx_fe_map[i].core->populate_subtree(
                get_tree()->subtree(FE_PATH / "rx_fe_corrections" / i));
        }
        // TX Periphs
        for (size_t i = 0; i < std::max<size_t>(get_num_input_ports(), 1); i++) {
            _tx_fe_map[i].core = tx_frontend_core_200::make(_wb_iface,
                x300_regs::SR_TX_FE_BASE + i * x300_regs::SR_FE_CHAN_OFFSET,
                x300_regs::PERIPH_REG_OFFSET);
            _tx_fe_map[i].core->set_dc_offset(
                tx_frontend_core_200::DEFAULT_DC_OFFSET_VALUE);
            _tx_fe_map[i].core->set_iq_balance(
                tx_frontend_core_200::DEFAULT_IQ_BALANCE_VALUE);
            _tx_fe_map[i].core->populate_subtree(
                get_tree()->subtree(FE_PATH / "tx_fe_corrections" / i));
        }

        // Dboards
        _init_dboards();

        // Properties
        for (auto& samp_rate_prop : _samp_rate_in) {
            samp_rate_prop.set(get_rate());
        }
        for (auto& samp_rate_prop : _samp_rate_out) {
            samp_rate_prop.set(get_rate());
        }
    } /* ctor */

    ~x300_radio_control_impl()
    {
        // nop
    }

    /**************************************************************************
     * Radio API calls
     *************************************************************************/
    double set_rate(double rate)
    {
        // On X3x0, tick rate can't actually be changed at runtime
        const double actual_rate = get_rate();
        if (not uhd::math::frequencies_are_equal(rate, actual_rate)) {
            RFNOC_LOG_WARNING("Requesting invalid sampling rate from device: "
                              << (rate / 1e6) << " MHz. Actual rate is: "
                              << (actual_rate / 1e6) << " MHz.");
        }
        return actual_rate;
    }

    void set_tx_antenna(const std::string& ant, const size_t chan)
    {
        get_tree()
            ->access<std::string>(get_db_path("tx", chan) / "antenna" / "value")
            .set(ant);
    }

    std::string get_tx_antenna(const size_t chan) const
    {
        return get_tree()
            ->access<std::string>(get_db_path("tx", chan) / "antenna" / "value")
            .get();
    }

    std::vector<std::string> get_tx_antennas(size_t chan) const
    {
        return get_tree()
            ->access<std::vector<std::string>>(
                get_db_path("tx", chan) / "antenna" / "options")
            .get();
    }

    void set_rx_antenna(const std::string& ant, const size_t chan)
    {
        get_tree()
            ->access<std::string>(get_db_path("rx", chan) / "antenna" / "value")
            .set(ant);
    }

    std::string get_rx_antenna(const size_t chan) const
    {
        return get_tree()
            ->access<std::string>(get_db_path("rx", chan) / "antenna" / "value")
            .get();
    }

    std::vector<std::string> get_rx_antennas(size_t chan) const
    {
        return get_tree()
            ->access<std::vector<std::string>>(
                get_db_path("rx", chan) / "antenna" / "options")
            .get();
    }

    double set_tx_frequency(const double freq, const size_t chan)
    {
        return get_tree()
            ->access<double>(get_db_path("tx", chan) / "freq" / "value")
            .set(freq)
            .get();
    }

    void set_tx_tune_args(const uhd::device_addr_t& tune_args, const size_t chan)
    {
        if (get_tree()->exists(get_db_path("tx", chan) / "tune_args")) {
            get_tree()
                ->access<uhd::device_addr_t>(get_db_path("tx", chan) / "tune_args")
                .set(tune_args);
        }
    }

    double get_tx_frequency(const size_t chan)
    {
        return get_tree()
            ->access<double>(get_db_path("tx", chan) / "freq" / "value")
            .get();
    }

    double set_rx_frequency(const double freq, const size_t chan)
    {
        RFNOC_LOG_TRACE(
            "set_rx_frequency(freq=" << (freq / 1e6) << " MHz, chan=" << chan << ")");
        return get_tree()
            ->access<double>(get_db_path("rx", chan) / "freq" / "value")
            .set(freq)
            .get();
    }

    void set_rx_tune_args(const uhd::device_addr_t& tune_args, const size_t chan)
    {
        if (get_tree()->exists(get_db_path("rx", chan) / "tune_args")) {
            get_tree()
                ->access<uhd::device_addr_t>(get_db_path("rx", chan) / "tune_args")
                .set(tune_args);
        }
    }

    double get_rx_frequency(const size_t chan)
    {
        return get_tree()
            ->access<double>(get_db_path("rx", chan) / "freq" / "value")
            .get();
    }

    uhd::freq_range_t get_tx_frequency_range(const size_t chan) const
    {
        return get_tree()
            ->access<uhd::freq_range_t>(get_db_path("tx", chan) / "freq" / "range")
            .get();
    }

    uhd::freq_range_t get_rx_frequency_range(const size_t chan) const
    {
        return get_tree()
            ->access<uhd::meta_range_t>(get_db_path("rx", chan) / "freq" / "range")
            .get();
    }

    /*** Bandwidth-Related APIs************************************************/
    double set_rx_bandwidth(const double bandwidth, const size_t chan)
    {
        return get_tree()
            ->access<double>(get_db_path("rx", chan) / "bandwidth" / "value")
            .set(bandwidth)
            .get();
    }

    double get_rx_bandwidth(const size_t chan)
    {
        return get_tree()
            ->access<double>(get_db_path("rx", chan) / "bandwidth" / "value")
            .get();
    }

    uhd::meta_range_t get_rx_bandwidth_range(size_t chan) const
    {
        return get_tree()
            ->access<uhd::meta_range_t>(get_db_path("rx", chan) / "bandwidth" / "range")
            .get();
    }

    double set_tx_bandwidth(const double bandwidth, const size_t chan)
    {
        return get_tree()
            ->access<double>(get_db_path("tx", chan) / "bandwidth" / "value")
            .set(bandwidth)
            .get();
    }

    double get_tx_bandwidth(const size_t chan)
    {
        return get_tree()
            ->access<double>(get_db_path("tx", chan) / "bandwidth" / "value")
            .get();
    }

    uhd::meta_range_t get_tx_bandwidth_range(size_t chan) const
    {
        return get_tree()
            ->access<uhd::meta_range_t>(get_db_path("tx", chan) / "bandwidth" / "range")
            .get();
    }

    /*** Gain-Related APIs ***************************************************/
    double set_tx_gain(const double gain, const size_t chan)
    {
        return set_tx_gain(gain, ALL_GAINS, chan);
    }

    double set_tx_gain(const double gain, const std::string& name, const size_t chan)
    {
        if (_tx_gain_groups.count(chan)) {
            auto& gg = _tx_gain_groups.at(chan);
            gg->set_value(gain, name);
            return radio_control_impl::set_tx_gain(gg->get_value(name), chan);
        }
        return radio_control_impl::set_tx_gain(0.0, chan);
    }

    double set_rx_gain(const double gain, const size_t chan)
    {
        return set_rx_gain(gain, ALL_GAINS, chan);
    }

    double set_rx_gain(const double gain, const std::string& name, const size_t chan)
    {
        auto& gg = _rx_gain_groups.at(chan);
        gg->set_value(gain, name);
        return radio_control_impl::set_rx_gain(gg->get_value(name), chan);
    }

    double get_rx_gain(const size_t chan)
    {
        return get_rx_gain(ALL_GAINS, chan);
    }

    double get_rx_gain(const std::string& name, const size_t chan)
    {
        return _rx_gain_groups.at(chan)->get_value(name);
    }

    double get_tx_gain(const size_t chan)
    {
        return get_tx_gain(ALL_GAINS, chan);
    }

    double get_tx_gain(const std::string& name, const size_t chan)
    {
        return _tx_gain_groups.at(chan)->get_value(name);
    }

    std::vector<std::string> get_tx_gain_names(const size_t chan) const
    {
        return _tx_gain_groups.at(chan)->get_names();
    }

    std::vector<std::string> get_rx_gain_names(const size_t chan) const
    {
        return _rx_gain_groups.at(chan)->get_names();
    }

    uhd::gain_range_t get_tx_gain_range(const size_t chan) const
    {
        return get_tx_gain_range(ALL_GAINS, chan);
    }

    uhd::gain_range_t get_tx_gain_range(const std::string& name, const size_t chan) const
    {
        if (!_tx_gain_groups.count(chan)) {
            throw uhd::index_error(
                "Trying to access invalid TX gain group: " + std::to_string(chan));
        }
        return _tx_gain_groups.at(chan)->get_range(name);
    }

    uhd::gain_range_t get_rx_gain_range(const size_t chan) const
    {
        return get_rx_gain_range(ALL_GAINS, chan);
    }

    uhd::gain_range_t get_rx_gain_range(const std::string& name, const size_t chan) const
    {
        if (!_rx_gain_groups.count(chan)) {
            throw uhd::index_error(
                "Trying to access invalid RX gain group: " + std::to_string(chan));
        }
        return _rx_gain_groups.at(chan)->get_range(name);
    }

    std::vector<std::string> get_tx_gain_profile_names(const size_t chan) const
    {
        return get_tree()
            ->access<std::vector<std::string>>(
                get_db_path("tx", chan) / "gains/all/profile/options")
            .get();
    }

    std::vector<std::string> get_rx_gain_profile_names(const size_t chan) const
    {
        return get_tree()
            ->access<std::vector<std::string>>(
                get_db_path("rx", chan) / "gains/all/profile/options")
            .get();
    }


    void set_tx_gain_profile(const std::string& profile, const size_t chan)
    {
        get_tree()
            ->access<std::string>(get_db_path("tx", chan) / "gains/all/profile/value")
            .set(profile);
    }

    void set_rx_gain_profile(const std::string& profile, const size_t chan)
    {
        get_tree()
            ->access<std::string>(get_db_path("rx", chan) / "gains/all/profile/value")
            .set(profile);
    }


    std::string get_tx_gain_profile(const size_t chan) const
    {
        return get_tree()
            ->access<std::string>(get_db_path("tx", chan) / "gains/all/profile/value")
            .get();
    }

    std::string get_rx_gain_profile(const size_t chan) const
    {
        return get_tree()
            ->access<std::string>(get_db_path("rx", chan) / "gains/all/profile/value")
            .get();
    }

    /**************************************************************************
     * LO controls
     *************************************************************************/
    std::vector<std::string> get_rx_lo_names(const size_t chan) const
    {
        fs_path rx_fe_fe_root = get_db_path("rx", chan);
        std::vector<std::string> lo_names;
        if (get_tree()->exists(rx_fe_fe_root / "los")) {
            for (const std::string& name : get_tree()->list(rx_fe_fe_root / "los")) {
                lo_names.push_back(name);
            }
        }
        return lo_names;
    }

    std::vector<std::string> get_rx_lo_sources(
        const std::string& name, const size_t chan) const
    {
        fs_path rx_fe_fe_root = get_db_path("rx", chan);

        if (get_tree()->exists(rx_fe_fe_root / "los")) {
            if (name == ALL_LOS) {
                if (get_tree()->exists(rx_fe_fe_root / "los" / ALL_LOS)) {
                    // Special value ALL_LOS support atomically sets the source for all
                    // LOs
                    return get_tree()
                        ->access<std::vector<std::string>>(
                            rx_fe_fe_root / "los" / ALL_LOS / "source" / "options")
                        .get();
                } else {
                    return std::vector<std::string>();
                }
            } else {
                if (get_tree()->exists(rx_fe_fe_root / "los")) {
                    return get_tree()
                        ->access<std::vector<std::string>>(
                            rx_fe_fe_root / "los" / name / "source" / "options")
                        .get();
                } else {
                    throw uhd::runtime_error("Could not find LO stage " + name);
                }
            }
        } else {
            // If the daughterboard doesn't expose it's LO(s) then it can only be internal
            return std::vector<std::string>(1, "internal");
        }
    }

    void set_rx_lo_source(
        const std::string& src, const std::string& name, const size_t chan)
    {
        fs_path rx_fe_fe_root = get_db_path("rx", chan);

        if (get_tree()->exists(rx_fe_fe_root / "los")) {
            if (name == ALL_LOS) {
                if (get_tree()->exists(rx_fe_fe_root / "los" / ALL_LOS)) {
                    // Special value ALL_LOS support atomically sets the source for all
                    // LOs
                    get_tree()
                        ->access<std::string>(
                            rx_fe_fe_root / "los" / ALL_LOS / "source" / "value")
                        .set(src);
                } else {
                    for (const std::string& n : get_tree()->list(rx_fe_fe_root / "los")) {
                        this->set_rx_lo_source(src, n, chan);
                    }
                }
            } else {
                if (get_tree()->exists(rx_fe_fe_root / "los")) {
                    get_tree()
                        ->access<std::string>(
                            rx_fe_fe_root / "los" / name / "source" / "value")
                        .set(src);
                } else {
                    throw uhd::runtime_error("Could not find LO stage " + name);
                }
            }
        } else {
            throw uhd::runtime_error(
                "This device does not support manual configuration of LOs");
        }
    }

    const std::string get_rx_lo_source(const std::string& name, const size_t chan)
    {
        fs_path rx_fe_fe_root = get_db_path("rx", chan);

        if (get_tree()->exists(rx_fe_fe_root / "los")) {
            if (name == ALL_LOS) {
                // Special value ALL_LOS support atomically sets the source for all LOs
                return get_tree()
                    ->access<std::string>(
                        rx_fe_fe_root / "los" / ALL_LOS / "source" / "value")
                    .get();
            } else {
                if (get_tree()->exists(rx_fe_fe_root / "los")) {
                    return get_tree()
                        ->access<std::string>(
                            rx_fe_fe_root / "los" / name / "source" / "value")
                        .get();
                } else {
                    throw uhd::runtime_error("Could not find LO stage " + name);
                }
            }
        } else {
            // If the daughterboard doesn't expose it's LO(s) then it can only be internal
            return "internal";
        }
    }

    void set_rx_lo_export_enabled(
        bool enabled, const std::string& name, const size_t chan)
    {
        fs_path rx_fe_fe_root = get_db_path("rx", chan);

        if (get_tree()->exists(rx_fe_fe_root / "los")) {
            if (name == ALL_LOS) {
                if (get_tree()->exists(rx_fe_fe_root / "los" / ALL_LOS)) {
                    // Special value ALL_LOS support atomically sets the source for all
                    // LOs
                    get_tree()
                        ->access<bool>(rx_fe_fe_root / "los" / ALL_LOS / "export")
                        .set(enabled);
                } else {
                    for (const std::string& n : get_tree()->list(rx_fe_fe_root / "los")) {
                        this->set_rx_lo_export_enabled(enabled, n, chan);
                    }
                }
            } else {
                if (get_tree()->exists(rx_fe_fe_root / "los")) {
                    get_tree()
                        ->access<bool>(rx_fe_fe_root / "los" / name / "export")
                        .set(enabled);
                } else {
                    throw uhd::runtime_error("Could not find LO stage " + name);
                }
            }
        } else {
            throw uhd::runtime_error(
                "This device does not support manual configuration of LOs");
        }
    }

    bool get_rx_lo_export_enabled(const std::string& name, const size_t chan) const
    {
        fs_path rx_fe_fe_root = get_db_path("rx", chan);

        if (get_tree()->exists(rx_fe_fe_root / "los")) {
            if (name == ALL_LOS) {
                // Special value ALL_LOS support atomically sets the source for all LOs
                return get_tree()
                    ->access<bool>(rx_fe_fe_root / "los" / ALL_LOS / "export")
                    .get();
            } else {
                if (get_tree()->exists(rx_fe_fe_root / "los")) {
                    return get_tree()
                        ->access<bool>(rx_fe_fe_root / "los" / name / "export")
                        .get();
                } else {
                    throw uhd::runtime_error("Could not find LO stage " + name);
                }
            }
        } else {
            // If the daughterboard doesn't expose it's LO(s), assume it cannot export
            return false;
        }
    }

    double set_rx_lo_freq(double freq, const std::string& name, const size_t chan)
    {
        fs_path rx_fe_fe_root = get_db_path("rx", chan);

        if (get_tree()->exists(rx_fe_fe_root / "los")) {
            if (name == ALL_LOS) {
                throw uhd::runtime_error(
                    "LO frequency must be set for each stage individually");
            } else {
                if (get_tree()->exists(rx_fe_fe_root / "los")) {
                    get_tree()
                        ->access<double>(rx_fe_fe_root / "los" / name / "freq" / "value")
                        .set(freq);
                    return get_tree()
                        ->access<double>(rx_fe_fe_root / "los" / name / "freq" / "value")
                        .get();
                } else {
                    throw uhd::runtime_error("Could not find LO stage " + name);
                }
            }
        } else {
            throw uhd::runtime_error(
                "This device does not support manual configuration of LOs");
        }
    }

    double get_rx_lo_freq(const std::string& name, const size_t chan)
    {
        fs_path rx_fe_fe_root = get_db_path("rx", chan);

        if (get_tree()->exists(rx_fe_fe_root / "los")) {
            if (name == ALL_LOS) {
                throw uhd::runtime_error(
                    "LO frequency must be retrieved for each stage individually");
            } else {
                if (get_tree()->exists(rx_fe_fe_root / "los")) {
                    return get_tree()
                        ->access<double>(rx_fe_fe_root / "los" / name / "freq" / "value")
                        .get();
                } else {
                    throw uhd::runtime_error("Could not find LO stage " + name);
                }
            }
        } else {
            // Return actual RF frequency if the daughterboard doesn't expose its LO(s)
            return get_tree()->access<double>(rx_fe_fe_root / "freq" / " value").get();
        }
    }

    freq_range_t get_rx_lo_freq_range(const std::string& name, const size_t chan) const
    {
        fs_path rx_fe_fe_root = get_db_path("rx", chan);

        if (get_tree()->exists(rx_fe_fe_root / "los")) {
            if (name == ALL_LOS) {
                throw uhd::runtime_error(
                    "LO frequency range must be retrieved for each stage individually");
            } else {
                if (get_tree()->exists(rx_fe_fe_root / "los")) {
                    return get_tree()
                        ->access<freq_range_t>(
                            rx_fe_fe_root / "los" / name / "freq" / "range")
                        .get();
                } else {
                    throw uhd::runtime_error("Could not find LO stage " + name);
                }
            }
        } else {
            // Return the actual RF range if the daughterboard doesn't expose its LO(s)
            return get_tree()
                ->access<meta_range_t>(rx_fe_fe_root / "freq" / "range")
                .get();
        }
    }

    /*** Calibration API *****************************************************/
    void set_tx_dc_offset(const std::complex<double>& offset, size_t chan)
    {
        const fs_path dc_offset_path = get_fe_path("tx", chan) / "dc_offset" / "value";
        if (get_tree()->exists(dc_offset_path)) {
            get_tree()->access<std::complex<double>>(dc_offset_path).set(offset);
        } else {
            RFNOC_LOG_WARNING("Setting TX DC offset is not possible on this device.");
        }
    }

    meta_range_t get_tx_dc_offset_range(size_t chan) const
    {
        const fs_path range_path = get_fe_path("tx", chan) / "dc_offset" / "range";
        if (get_tree()->exists(range_path)) {
            return get_tree()->access<uhd::meta_range_t>(range_path).get();
        } else {
            RFNOC_LOG_WARNING(
                "This device does not support querying the TX DC offset range.");
            return meta_range_t(0, 0);
        }
    }

    void set_tx_iq_balance(const std::complex<double>& correction, size_t chan)
    {
        const fs_path iq_balance_path = get_fe_path("tx", chan) / "iq_balance" / "value";
        if (get_tree()->exists(iq_balance_path)) {
            get_tree()->access<std::complex<double>>(iq_balance_path).set(correction);
        } else {
            RFNOC_LOG_WARNING("Setting TX IQ Balance is not possible on this device.");
        }
    }

    void set_rx_dc_offset(const bool enb, size_t chan)
    {
        const fs_path dc_offset_path = get_fe_path("rx", chan) / "dc_offset" / "enable";
        if (get_tree()->exists(dc_offset_path)) {
            get_tree()->access<bool>(dc_offset_path).set(enb);
        } else {
            RFNOC_LOG_WARNING(
                "Setting DC offset compensation is not possible on this device.");
        }
    }

    void set_rx_dc_offset(const std::complex<double>& offset, size_t chan)
    {
        const fs_path dc_offset_path = get_fe_path("rx", chan) / "dc_offset" / "value";
        if (get_tree()->exists(dc_offset_path)) {
            get_tree()->access<std::complex<double>>(dc_offset_path).set(offset);
        } else {
            RFNOC_LOG_WARNING("Setting RX DC offset is not possible on this device.");
        }
    }

    meta_range_t get_rx_dc_offset_range(size_t chan) const
    {
        const fs_path range_path = get_fe_path("rx", chan) / "dc_offset" / "range";
        if (get_tree()->exists(range_path)) {
            return get_tree()->access<uhd::meta_range_t>(range_path).get();
        } else {
            RFNOC_LOG_WARNING(
                "This device does not support querying the rx DC offset range.");
            return meta_range_t(0, 0);
        }
    }

    void set_rx_iq_balance(const bool enb, size_t chan)
    {
        const fs_path iq_balance_path = get_fe_path("rx", chan) / "iq_balance" / "enable";
        if (get_tree()->exists(iq_balance_path)) {
            get_tree()->access<bool>(iq_balance_path).set(enb);
        } else {
            RFNOC_LOG_WARNING("Setting RX IQ Balance is not possible on this device.");
        }
    }

    void set_rx_iq_balance(const std::complex<double>& correction, size_t chan)
    {
        const fs_path iq_balance_path = get_fe_path("rx", chan) / "iq_balance" / "value";
        if (get_tree()->exists(iq_balance_path)) {
            get_tree()->access<std::complex<double>>(iq_balance_path).set(correction);
        } else {
            RFNOC_LOG_WARNING("Setting RX IQ Balance is not possible on this device.");
        }
    }

    /*** GPIO API ************************************************************/
    std::vector<std::string> get_gpio_banks() const
    {
        std::vector<std::string> banks{"RX", "TX"};
        if (_fp_gpio) {
            banks.push_back("FP0");
        }
        return banks;
    }

    void set_gpio_attr(
        const std::string& bank, const std::string& attr, const uint32_t value)
    {
        if (bank == "FP0" and _fp_gpio) {
            _fp_gpio->set_gpio_attr(usrp::gpio_atr::gpio_attr_rev_map.at(attr), value);
            return;
        }
        if (bank.size() > 2 and bank[1] == 'X') {
            const std::string name          = bank.substr(2);
            const dboard_iface::unit_t unit = (bank[0] == 'R') ? dboard_iface::UNIT_RX
                                                               : dboard_iface::UNIT_TX;
            dboard_iface::sptr iface =
                get_tree()->access<dboard_iface::sptr>(DB_PATH / name / "iface").get();
            const uint16_t mask = 0xFFFF;
            if (attr == "CTRL")
                iface->set_pin_ctrl(unit, uint16_t(value), uint16_t(mask));
            if (attr == "DDR")
                iface->set_gpio_ddr(unit, uint16_t(value), uint16_t(mask));
            if (attr == "OUT")
                iface->set_gpio_out(unit, uint16_t(value), uint16_t(mask));
            if (attr == "ATR_0X")
                iface->set_atr_reg(
                    unit, gpio_atr::ATR_REG_IDLE, uint16_t(value), uint16_t(mask));
            if (attr == "ATR_RX")
                iface->set_atr_reg(
                    unit, gpio_atr::ATR_REG_RX_ONLY, uint16_t(value), uint16_t(mask));
            if (attr == "ATR_TX")
                iface->set_atr_reg(
                    unit, gpio_atr::ATR_REG_TX_ONLY, uint16_t(value), uint16_t(mask));
            if (attr == "ATR_XX")
                iface->set_atr_reg(
                    unit, gpio_atr::ATR_REG_FULL_DUPLEX, uint16_t(value), uint16_t(mask));
        }
    }

    uint32_t get_gpio_attr(const std::string& bank, const std::string& attr)
    {
        if (bank == "FP0" and _fp_gpio) {
            return _fp_gpio->get_attr_reg(usrp::gpio_atr::gpio_attr_rev_map.at(attr));
        }
        if (bank.size() > 2 and bank[1] == 'X') {
            const std::string name          = bank.substr(2);
            const dboard_iface::unit_t unit = (bank[0] == 'R') ? dboard_iface::UNIT_RX
                                                               : dboard_iface::UNIT_TX;
            auto iface =
                get_tree()->access<dboard_iface::sptr>(DB_PATH / name / "iface").get();
            if (attr == "CTRL")
                return iface->get_pin_ctrl(unit);
            if (attr == "DDR")
                return iface->get_gpio_ddr(unit);
            if (attr == "OUT")
                return iface->get_gpio_out(unit);
            if (attr == "ATR_0X")
                return iface->get_atr_reg(unit, gpio_atr::ATR_REG_IDLE);
            if (attr == "ATR_RX")
                return iface->get_atr_reg(unit, gpio_atr::ATR_REG_RX_ONLY);
            if (attr == "ATR_TX")
                return iface->get_atr_reg(unit, gpio_atr::ATR_REG_TX_ONLY);
            if (attr == "ATR_XX")
                return iface->get_atr_reg(unit, gpio_atr::ATR_REG_FULL_DUPLEX);
            if (attr == "READBACK")
                return iface->read_gpio(unit);
        }
        return 0;
    }

    /**************************************************************************
     * Sensor API
     *************************************************************************/
    std::vector<std::string> get_rx_sensor_names(size_t chan) const
    {
        const fs_path sensor_path = get_db_path("rx", chan) / "sensors";
        if (get_tree()->exists(sensor_path)) {
            return get_tree()->list(sensor_path);
        }
        return {};
    }

    uhd::sensor_value_t get_rx_sensor(const std::string& name, size_t chan)
    {
        return get_tree()
            ->access<uhd::sensor_value_t>(get_db_path("rx", chan) / "sensors" / name)
            .get();
    }

    std::vector<std::string> get_tx_sensor_names(size_t chan) const
    {
        const fs_path sensor_path = get_db_path("tx", chan) / "sensors";
        if (get_tree()->exists(sensor_path)) {
            return get_tree()->list(sensor_path);
        }
        return {};
    }

    uhd::sensor_value_t get_tx_sensor(const std::string& name, size_t chan)
    {
        return get_tree()
            ->access<uhd::sensor_value_t>(get_db_path("tx", chan) / "sensors" / name)
            .get();
    }

    /**************************************************************************
     * EEPROM API
     *************************************************************************/
    void set_db_eeprom(const uhd::eeprom_map_t& db_eeprom)
    {
        const std::string key_prefix = db_eeprom.count("rx_id") ? "rx_" : "tx_";
        const std::string id_key     = key_prefix + "id";
        const std::string serial_key = key_prefix + "serial";
        const std::string rev_key    = key_prefix + "rev";
        if (!(db_eeprom.count(id_key) && db_eeprom.count(serial_key)
                && db_eeprom.count(rev_key))) {
            RFNOC_LOG_ERROR("set_db_eeprom() requires id, serial, and rev keys!");
            throw uhd::key_error(
                "[X300] set_db_eeprom() requires id, serial, and rev keys!");
        }

        dboard_eeprom_t eeprom;
        eeprom.id.from_string(bytes_to_str(db_eeprom.at(id_key)));
        eeprom.serial   = bytes_to_str(db_eeprom.at(serial_key));
        eeprom.revision = bytes_to_str(db_eeprom.at(rev_key));
        if (get_tree()->exists(DB_PATH / (key_prefix + "eeprom"))) {
            get_tree()
                ->access<dboard_eeprom_t>(DB_PATH / (key_prefix + "eeprom"))
                .set(eeprom);
        } else {
            RFNOC_LOG_WARNING("Cannot set EEPROM, tree path does not exist.");
        }
    }


    uhd::eeprom_map_t get_db_eeprom()
    {
        uhd::eeprom_map_t result;
        if (get_tree()->exists(DB_PATH / "rx_eeprom")) {
            const auto rx_eeprom =
                get_tree()->access<dboard_eeprom_t>(DB_PATH / "rx_eeprom").get();
            result["rx_id"]     = str_to_bytes(rx_eeprom.id.to_pp_string());
            result["rx_serial"] = str_to_bytes(rx_eeprom.serial);
            result["rx_rev"]    = str_to_bytes(rx_eeprom.revision);
        }
        if (get_tree()->exists(DB_PATH / "tx_eeprom")) {
            const auto rx_eeprom =
                get_tree()->access<dboard_eeprom_t>(DB_PATH / "rx_eeprom").get();
            result["tx_id"]     = str_to_bytes(rx_eeprom.id.to_pp_string());
            result["tx_serial"] = str_to_bytes(rx_eeprom.serial);
            result["tx_rev"]    = str_to_bytes(rx_eeprom.revision);
        }
        return result;
    }

    /**************************************************************************
     * Radio Identification API Calls
     *************************************************************************/
    std::string get_slot_name() const
    {
        return _radio_type == PRIMARY ? "A" : "B";
    }

    size_t get_chan_from_dboard_fe(
        const std::string& fe, const direction_t direction) const
    {
        switch (direction) {
            case uhd::TX_DIRECTION:
                return _get_chan_from_map(_tx_fe_map, fe);
            case uhd::RX_DIRECTION:
                return _get_chan_from_map(_rx_fe_map, fe);
            default:
                UHD_THROW_INVALID_CODE_PATH();
        }
    }

    std::string get_dboard_fe_from_chan(
        const size_t chan, const uhd::direction_t direction) const
    {
        switch (direction) {
            case uhd::TX_DIRECTION:
                return _tx_fe_map.at(chan).db_fe_name;
            case uhd::RX_DIRECTION:
                return _rx_fe_map.at(chan).db_fe_name;
            default:
                UHD_THROW_INVALID_CODE_PATH();
        }
    }

    std::string get_fe_name(const size_t chan, const uhd::direction_t direction) const
    {
        fs_path name_path =
            get_db_path(direction == uhd::RX_DIRECTION ? "rx" : "tx", chan) / "name";
        if (!get_tree()->exists(name_path)) {
            return get_dboard_fe_from_chan(chan, direction);
        }

        return get_tree()->access<std::string>(name_path).get();
    }


    virtual void set_command_time(uhd::time_spec_t time, const size_t chan)
    {
        node_t::set_command_time(time, chan);
        // This is for TwinRX only:
        fs_path cmd_time_path = get_db_path("rx", chan) / "time" / "cmd";
        if (get_tree()->exists(cmd_time_path)) {
            get_tree()->access<time_spec_t>(cmd_time_path).set(time);
        }
    }

    /**************************************************************************
     * MB Interface API Calls
     *************************************************************************/
    uint32_t get_adc_rx_word()
    {
        return regs().peek32(regmap::RADIO_BASE_ADDR + regmap::REG_RX_DATA);
    }

    void set_adc_test_word(const std::string& patterna, const std::string& patternb)
    {
        _adc->set_test_word(patterna, patternb);
    }

    void set_adc_checker_enabled(const bool enb)
    {
        _regs->misc_outs_reg.write(
            radio_regmap_t::misc_outs_reg_t::ADC_CHECKER_ENABLED, enb ? 1 : 0);
    }

    bool get_adc_checker_locked(const bool I)
    {
        return bool(_regs->misc_ins_reg.read(
            I ? radio_regmap_t::misc_ins_reg_t::ADC_CHECKER1_I_LOCKED
              : radio_regmap_t::misc_ins_reg_t::ADC_CHECKER1_Q_LOCKED));
    }

    uint32_t get_adc_checker_error_code(const bool I)
    {
        return _regs->misc_ins_reg.get(
            I ? radio_regmap_t::misc_ins_reg_t::ADC_CHECKER1_I_ERROR
              : radio_regmap_t::misc_ins_reg_t::ADC_CHECKER1_Q_ERROR);
    }

    // Documented in x300_radio_mbc_iface.hpp
    void self_test_adc(const uint32_t ramp_time_ms)
    {
        RFNOC_LOG_DEBUG("Running ADC self-cal...");
        // Bypass all front-end corrections
        for (size_t i = 0; i < get_num_output_ports(); i++) {
            _rx_fe_map[i].core->bypass_all(true);
        }

        // Test basic patterns
        _adc->set_test_word("ones", "ones");
        _check_adc(0xfffcfffc);
        _adc->set_test_word("zeros", "zeros");
        _check_adc(0x00000000);
        _adc->set_test_word("ones", "zeros");
        _check_adc(0xfffc0000);
        _adc->set_test_word("zeros", "ones");
        _check_adc(0x0000fffc);
        for (size_t k = 0; k < 14; k++) {
            _adc->set_test_word("zeros", "custom", 1 << k);
            _check_adc(1 << (k + 2));
        }
        for (size_t k = 0; k < 14; k++) {
            _adc->set_test_word("custom", "zeros", 1 << k);
            _check_adc(1 << (k + 18));
        }

        // Turn on ramp pattern test
        _adc->set_test_word("ramp", "ramp");
        _regs->misc_outs_reg.write(
            radio_regmap_t::misc_outs_reg_t::ADC_CHECKER_ENABLED, 0);
        // Sleep added for SPI transactions to finish and ramp to start before checker is
        // enabled.
        std::this_thread::sleep_for(std::chrono::microseconds(1000));
        _regs->misc_outs_reg.write(
            radio_regmap_t::misc_outs_reg_t::ADC_CHECKER_ENABLED, 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(ramp_time_ms));
        _regs->misc_ins_reg.refresh();

        std::string i_status, q_status;
        if (_regs->misc_ins_reg.get(
                radio_regmap_t::misc_ins_reg_t::ADC_CHECKER1_I_LOCKED))
            if (_regs->misc_ins_reg.get(
                    radio_regmap_t::misc_ins_reg_t::ADC_CHECKER1_I_ERROR))
                i_status = "Bit Errors!";
            else
                i_status = "Good";
        else
            i_status = "Not Locked!";

        if (_regs->misc_ins_reg.get(
                radio_regmap_t::misc_ins_reg_t::ADC_CHECKER1_Q_LOCKED))
            if (_regs->misc_ins_reg.get(
                    radio_regmap_t::misc_ins_reg_t::ADC_CHECKER1_Q_ERROR))
                q_status = "Bit Errors!";
            else
                q_status = "Good";
        else
            q_status = "Not Locked!";

        // Return to normal mode
        _adc->set_test_word("normal", "normal");

        if ((i_status != "Good") or (q_status != "Good")) {
            throw uhd::runtime_error(
                (boost::format("ADC self-test failed for %s. Ramp checker status: "
                               "{ADC_A=%s, ADC_B=%s}")
                    % get_unique_id() % i_status % q_status)
                    .str());
        }

        // Restore front-end corrections
        for (size_t i = 0; i < get_num_output_ports(); i++) {
            _rx_fe_map[i].core->bypass_all(false);
        }
    }

    void sync_dac()
    {
        _dac->sync();
    }

    void set_dac_sync(const bool enb, const uhd::time_spec_t& time)
    {
        if (time != uhd::time_spec_t(0.0)) {
            set_command_time(time, 0);
        }
        _regs->misc_outs_reg.write(
            radio_regmap_t::misc_outs_reg_t::DAC_SYNC, enb ? 1 : 0);
        if (!enb && time != uhd::time_spec_t(0.0)) {
            set_command_time(uhd::time_spec_t(0.0), 0);
        }
    }

    void dac_verify_sync()
    {
        _dac->verify_sync();
    }

private:
    /**************************************************************************
     * ADC Control
     *************************************************************************/
    //! Create the ADC/DAC objects, reset them, run ADC cal
    void _init_codecs()
    {
        _regs = std::make_unique<radio_regmap_t>(get_block_id().get_block_count());
        _regs->initialize(*_wb_iface, true);
        // Only Radio0 has the ADC/DAC reset bits
        if (_radio_type == PRIMARY) {
            RFNOC_LOG_TRACE("Resetting DAC and ADCs...");
            _regs->misc_outs_reg.set(radio_regmap_t::misc_outs_reg_t::ADC_RESET, 1);
            _regs->misc_outs_reg.set(radio_regmap_t::misc_outs_reg_t::DAC_RESET_N, 0);
            _regs->misc_outs_reg.flush();
            _regs->misc_outs_reg.set(radio_regmap_t::misc_outs_reg_t::ADC_RESET, 0);
            _regs->misc_outs_reg.set(radio_regmap_t::misc_outs_reg_t::DAC_RESET_N, 1);
            _regs->misc_outs_reg.flush();
        }
        _regs->misc_outs_reg.write(radio_regmap_t::misc_outs_reg_t::DAC_ENABLED, 1);

        RFNOC_LOG_TRACE("Creating ADC interface...");
        _adc = x300_adc_ctrl::make(_spi, DB_ADC_SEN);
        RFNOC_LOG_TRACE("Creating DAC interface...");
        _dac = x300_dac_ctrl::make(_spi, DB_DAC_SEN, _master_clock_rate);
        _self_cal_adc_capture_delay();

        ////////////////////////////////////////////////////////////////
        // create legacy codec control objects
        ////////////////////////////////////////////////////////////////
        // DAC has no gains
        get_tree()->create<int>("tx_codec/gains");
        get_tree()->create<std::string>("tx_codec/name").set("ad9146");
        get_tree()->create<std::string>("rx_codec/name").set("ads62p48");
        get_tree()
            ->create<meta_range_t>("rx_codec/gains/digital/range")
            .set(meta_range_t(0, 6.0, 0.5));
        get_tree()
            ->create<double>("rx_codec/gains/digital/value")
            .add_coerced_subscriber([this](const double gain) { _adc->set_gain(gain); })
            .set(0);
    }

    //! Calibrate delays on the ADC. This needs to happen before every session.
    void _self_cal_adc_capture_delay()
    {
        RFNOC_LOG_TRACE("Running ADC capture delay self-cal...");
        constexpr uint32_t NUM_DELAY_STEPS = 32; // The IDELAYE2 element has 32 steps
        // Retry self-cal if it fails in warmup situations
        constexpr uint32_t NUM_RETRIES   = 2;
        constexpr int32_t MIN_WINDOW_LEN = 4;

        int32_t win_start = -1, win_stop = -1;
        uint32_t iter = 0;
        while (iter++ < NUM_RETRIES) {
            for (uint32_t dly_tap = 0; dly_tap < NUM_DELAY_STEPS; dly_tap++) {
                // Apply delay
                _regs->misc_outs_reg.write(
                    radio_regmap_t::misc_outs_reg_t::ADC_DATA_DLY_VAL, dly_tap);
                _regs->misc_outs_reg.write(
                    radio_regmap_t::misc_outs_reg_t::ADC_DATA_DLY_STB, 1);
                _regs->misc_outs_reg.write(
                    radio_regmap_t::misc_outs_reg_t::ADC_DATA_DLY_STB, 0);

                uint32_t err_code = 0;

                // -- Test I Channel --
                // Put ADC in ramp test mode. Tie the other channel to all ones.
                _adc->set_test_word("ramp", "ones");
                // Turn on the pattern checker in the FPGA. It will lock when it sees a
                // zero and count deviations from the expected value
                _regs->misc_outs_reg.write(
                    radio_regmap_t::misc_outs_reg_t::ADC_CHECKER_ENABLED, 0);
                _regs->misc_outs_reg.write(
                    radio_regmap_t::misc_outs_reg_t::ADC_CHECKER_ENABLED, 1);
                // 5ms @ 200MHz = 1 million samples
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (_regs->misc_ins_reg.read(
                        radio_regmap_t::misc_ins_reg_t::ADC_CHECKER0_I_LOCKED)) {
                    err_code += _regs->misc_ins_reg.get(
                        radio_regmap_t::misc_ins_reg_t::ADC_CHECKER0_I_ERROR);
                } else {
                    err_code += 100; // Increment error code by 100 to indicate no lock
                }

                // -- Test Q Channel --
                // Put ADC in ramp test mode. Tie the other channel to all ones.
                _adc->set_test_word("ones", "ramp");
                // Turn on the pattern checker in the FPGA. It will lock when it sees a
                // zero and count deviations from the expected value
                _regs->misc_outs_reg.write(
                    radio_regmap_t::misc_outs_reg_t::ADC_CHECKER_ENABLED, 0);
                _regs->misc_outs_reg.write(
                    radio_regmap_t::misc_outs_reg_t::ADC_CHECKER_ENABLED, 1);
                // 5ms @ 200MHz = 1 million samples
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (_regs->misc_ins_reg.read(
                        radio_regmap_t::misc_ins_reg_t::ADC_CHECKER0_Q_LOCKED)) {
                    err_code += _regs->misc_ins_reg.get(
                        radio_regmap_t::misc_ins_reg_t::ADC_CHECKER0_Q_ERROR);
                } else {
                    err_code += 100; // Increment error code by 100 to indicate no lock
                }

                if (err_code == 0) {
                    if (win_start == -1) { // This is the first window
                        win_start = dly_tap;
                        win_stop  = dly_tap;
                    } else { // We are extending the window
                        win_stop = dly_tap;
                    }
                } else {
                    if (win_start != -1) { // A valid window turned invalid
                        if (win_stop - win_start >= MIN_WINDOW_LEN) {
                            break; // Valid window found
                        } else {
                            win_start = -1; // Reset window
                        }
                    }
                }
                // UHD_LOGGER_INFO("X300 RADIO") << (boost::format("CapTap=%d, Error=%d")
                // % dly_tap % err_code);
            }

            // Retry the self-cal if it fails
            if ((win_start == -1 || (win_stop - win_start) < MIN_WINDOW_LEN)
                && iter < NUM_RETRIES /*not last iteration*/) {
                win_start = -1;
                win_stop  = -1;
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            } else {
                break;
            }
        }
        _adc->set_test_word("normal", "normal");
        _regs->misc_outs_reg.write(
            radio_regmap_t::misc_outs_reg_t::ADC_CHECKER_ENABLED, 0);

        if (win_start == -1) {
            throw uhd::runtime_error("self_cal_adc_capture_delay: Self calibration "
                                     "failed. Convergence error.");
        }

        if (win_stop - win_start < MIN_WINDOW_LEN) {
            throw uhd::runtime_error(
                "self_cal_adc_capture_delay: Self calibration failed. "
                "Valid window too narrow.");
        }

        uint32_t ideal_tap = (win_stop + win_start) / 2;
        _regs->misc_outs_reg.write(
            radio_regmap_t::misc_outs_reg_t::ADC_DATA_DLY_VAL, ideal_tap);
        _regs->misc_outs_reg.write(radio_regmap_t::misc_outs_reg_t::ADC_DATA_DLY_STB, 1);
        _regs->misc_outs_reg.write(radio_regmap_t::misc_outs_reg_t::ADC_DATA_DLY_STB, 0);

        double tap_delay = (1.0e12 / 200e6) / (2 * 32); // in ps
        RFNOC_LOG_DEBUG(
            boost::format("ADC capture delay self-cal done (Tap=%d, Window=%d, "
                          "TapDelay=%.3fps, Iter=%d)")
            % ideal_tap % (win_stop - win_start) % tap_delay % iter);
    }

    //! Verify that the output of the ADC matches an expected \p val
    void _check_adc(const uint32_t val)
    {
        // Wait for previous control transaction to flush
        get_adc_rx_word();
        // Wait for ADC test pattern to propagate
        std::this_thread::sleep_for(std::chrono::microseconds(5));
        // Read value of RX readback register and verify, adapt for I inversion
        // in FPGA
        const uint32_t adc_rb = get_adc_rx_word() ^ 0xfffc0000;
        if (val != adc_rb) {
            RFNOC_LOG_ERROR(boost::format("ADC self-test failed! (Exp=0x%x, Got=0x%x)")
                            % val % adc_rb);
            throw uhd::runtime_error("ADC self-test failed!");
        }
    }

    void reset_codec()
    {
        RFNOC_LOG_TRACE("Start reset_codec");
        if (_radio_type == PRIMARY) { // ADC/DAC reset lines only exist in Radio0
            _regs->misc_outs_reg.set(radio_regmap_t::misc_outs_reg_t::ADC_RESET, 1);
            _regs->misc_outs_reg.set(radio_regmap_t::misc_outs_reg_t::DAC_RESET_N, 0);
            _regs->misc_outs_reg.flush();
            _regs->misc_outs_reg.set(radio_regmap_t::misc_outs_reg_t::ADC_RESET, 0);
            _regs->misc_outs_reg.set(radio_regmap_t::misc_outs_reg_t::DAC_RESET_N, 1);
            _regs->misc_outs_reg.flush();
        }
        _regs->misc_outs_reg.write(radio_regmap_t::misc_outs_reg_t::DAC_ENABLED, 1);
        UHD_ASSERT_THROW(bool(_adc));
        UHD_ASSERT_THROW(bool(_dac));
        _adc->reset();
        _dac->reset();
        RFNOC_LOG_TRACE("Done reset_codec");
    }

    /**************************************************************************
     * DBoard
     *************************************************************************/
    fs_path get_db_path(const std::string& dir, const size_t chan) const
    {
        UHD_ASSERT_THROW(dir == "rx" || dir == "tx");
        if (dir == "rx" && chan >= get_num_output_ports()) {
            throw uhd::key_error("Invalid RX channel: " + std::to_string(chan));
        }
        if (dir == "tx" && chan >= get_num_input_ports()) {
            throw uhd::key_error("Invalid TX channel: " + std::to_string(chan));
        }
        return DB_PATH / (dir + "_frontends")
               / ((dir == "rx") ? _rx_fe_map.at(chan).db_fe_name
                                : _tx_fe_map.at(chan).db_fe_name);
    }

    fs_path get_fe_path(const std::string& dir, const size_t chan) const
    {
        UHD_ASSERT_THROW(dir == "rx" || dir == "tx");
        if (dir == "rx" && chan >= get_num_output_ports()) {
            throw uhd::key_error("Invalid RX channel: " + std::to_string(chan));
        }
        if (dir == "tx" && chan >= get_num_input_ports()) {
            throw uhd::key_error("Invalid TX channel: " + std::to_string(chan));
        }
        return FE_PATH / (dir + "_fe_corrections")
               / ((dir == "rx") ? _rx_fe_map.at(chan).db_fe_name
                                : _tx_fe_map.at(chan).db_fe_name);
    }

    void _init_db()
    {
        constexpr size_t BASE_ADDR       = 0x50;
        constexpr size_t RX_EEPROM_ADDR  = 0x5;
        constexpr size_t TX_EEPROM_ADDR  = 0x4;
        constexpr size_t GDB_EEPROM_ADDR = 0x1;
        static const std::vector<size_t> EEPROM_ADDRS{
            RX_EEPROM_ADDR, TX_EEPROM_ADDR, GDB_EEPROM_ADDR};
        static const std::vector<std::string> EEPROM_PATHS{
            "rx_eeprom", "tx_eeprom", "gdb_eeprom"};
        const size_t DB_OFFSET = (_radio_type == PRIMARY) ? 0x0 : 0x2;
        auto zpu_i2c           = _x300_mb_control->get_zpu_i2c();
        auto clock             = _x300_mb_control->get_clock_ctrl();
        for (size_t i = 0; i < EEPROM_ADDRS.size(); i++) {
            const size_t addr = EEPROM_ADDRS[i] + DB_OFFSET;
            // Load EEPROM
            _db_eeproms[addr].load(*zpu_i2c, BASE_ADDR | addr);
            // Add to tree
            get_tree()
                ->create<dboard_eeprom_t>(DB_PATH / EEPROM_PATHS[i])
                .set(_db_eeproms[addr])
                .add_coerced_subscriber([this, zpu_i2c, BASE_ADDR, addr](
                                            const uhd::usrp::dboard_eeprom_t& db_eeprom) {
                    _set_db_eeprom(zpu_i2c, BASE_ADDR | addr, db_eeprom);
                });
        }

        // create a new dboard interface
        x300_dboard_iface_config_t db_config;
        db_config.gpio           = gpio_atr::db_gpio_atr_3000::make(_wb_iface,
            x300_regs::SR_DB_GPIO,
            x300_regs::RB_DB_GPIO,
            x300_regs::PERIPH_REG_OFFSET);
        db_config.spi            = _spi;
        db_config.rx_spi_slaveno = DB_RX_SEN;
        db_config.tx_spi_slaveno = DB_TX_SEN;
        db_config.i2c            = zpu_i2c;
        db_config.clock          = clock;
        db_config.which_rx_clk   = (_radio_type == PRIMARY) ? X300_CLOCK_WHICH_DB0_RX
                                                          : X300_CLOCK_WHICH_DB1_RX;
        db_config.which_tx_clk = (_radio_type == PRIMARY) ? X300_CLOCK_WHICH_DB0_TX
                                                          : X300_CLOCK_WHICH_DB1_TX;
        db_config.dboard_slot   = (_radio_type == PRIMARY) ? 0 : 1;
        db_config.cmd_time_ctrl = _wb_iface;

        // create a new dboard manager
        RFNOC_LOG_TRACE("Creating DB interface...");
        _db_iface = boost::make_shared<x300_dboard_iface>(db_config);
        RFNOC_LOG_TRACE("Creating DB manager...");
        _db_manager = dboard_manager::make(_db_eeproms[RX_EEPROM_ADDR + DB_OFFSET],
            _db_eeproms[TX_EEPROM_ADDR + DB_OFFSET],
            _db_eeproms[GDB_EEPROM_ADDR + DB_OFFSET],
            _db_iface,
            get_tree()->subtree(DB_PATH),
            true // defer daughterboard initialization
        );
        RFNOC_LOG_TRACE("DB Manager Initialization complete.");

        // The X3x0 radio block defaults to two ports, but most daughterboards
        // only have one frontend. So we now reduce the number of actual ports
        // based on what is connected.
        // Note: The Basic and LF boards pretend they have four frontends,
        // which a hack from the past. However, they actually only have one
        // frontend, and we select the AB/BA/A/B setting through the antenna.
        // The easiest way to identify those boards is because they're the only
        // ones with four frontends.
        // For all other cases, we reduce the number of frontends to one.
        const size_t num_tx_frontends = _db_manager->get_tx_frontends().size();
        const size_t num_rx_frontends = _db_manager->get_rx_frontends().size();
        if (num_tx_frontends == 4) {
            RFNOC_LOG_TRACE("Found four frontends, inferring BasicTX or LFTX.");
            set_num_input_ports(1);
        } else if (num_tx_frontends == 2 || num_tx_frontends == 1) {
            set_num_input_ports(num_tx_frontends);
        } else {
            throw uhd::runtime_error("Unexpected number of TX frontends!");
        }
        if (num_rx_frontends == 4) {
            RFNOC_LOG_TRACE("Found four frontends, inferring BasicRX or LFRX.");
            set_num_output_ports(1);
        } else if (num_rx_frontends == 2 || num_rx_frontends == 1) {
            set_num_output_ports(num_rx_frontends);
        } else {
            throw uhd::runtime_error("Unexpected number of RX frontends!");
        }
        // This is specific to TwinRX. Due to driver legacy, we think we have a
        // Tx frontend even though we don't. We thus hard-code that knowledge
        // here.
        if (num_rx_frontends == 2
            && boost::starts_with(
                   get_tree()->access<std::string>(DB_PATH / "rx_frontends/0/name").get(),
                   "TwinRX")) {
            set_num_input_ports(0);
        }
        RFNOC_LOG_TRACE("Num Active Frontends: RX: " << get_num_output_ports()
                                                     << " TX: " << get_num_input_ports());
    }

    void _init_dboards()
    {
        size_t rx_chan = 0;
        size_t tx_chan = 0;
        for (const std::string& fe : _db_manager->get_rx_frontends()) {
            if (rx_chan >= get_num_output_ports()) {
                break;
            }
            _rx_fe_map[rx_chan].db_fe_name = fe;
            _db_iface->add_rx_fe(fe, _rx_fe_map[rx_chan].core);
            const fs_path fe_path(DB_PATH / "rx_frontends" / fe);
            const std::string conn =
                get_tree()->access<std::string>(fe_path / "connection").get();
            const double if_freq =
                (get_tree()->exists(fe_path / "if_freq/value"))
                    ? get_tree()->access<double>(fe_path / "if_freq/value").get()
                    : 0.0;
            _rx_fe_map[rx_chan].core->set_fe_connection(
                usrp::fe_connection_t(conn, if_freq));
            rx_chan++;
        }
        for (const std::string& fe : _db_manager->get_tx_frontends()) {
            if (tx_chan >= get_num_input_ports()) {
                break;
            }
            _tx_fe_map[tx_chan].db_fe_name = fe;
            const fs_path fe_path(DB_PATH / "tx_frontends" / fe);
            const std::string conn =
                get_tree()->access<std::string>(fe_path / "connection").get();
            _tx_fe_map[tx_chan].core->set_mux(conn);
            tx_chan++;
        }
        UHD_ASSERT_THROW(rx_chan or tx_chan);
        const double actual_rate = rx_chan ? _rx_fe_map.at(0).core->get_output_rate()
                                           : get_rate();
        RFNOC_LOG_DEBUG("Actual sample rate: " << (actual_rate / 1e6) << " Msps.");
        radio_control_impl::set_rate(actual_rate);

        // Initialize the daughterboards now that frontend cores and connections exist
        _db_manager->initialize_dboards();

        // now that dboard is created -- register into rx antenna event
        if (not _rx_fe_map.empty()) {
            for (size_t i = 0; i < get_num_output_ports(); i++) {
                if (get_tree()->exists(get_db_path("rx", i) / "antenna" / "value")) {
                    // We need a desired subscriber for antenna/value because the experts
                    // don't coerce that property.
                    get_tree()
                        ->access<std::string>(get_db_path("rx", i) / "antenna" / "value")
                        .add_desired_subscriber([this, i](const std::string& led) {
                            _update_atr_leds(led, i);
                        })
                        .update();
                } else {
                    _update_atr_leds("", i); // init anyway, even if never called
                }
            }
        }

        // bind frontend corrections to the dboard freq props
        if (not _tx_fe_map.empty()) {
            for (size_t i = 0; i < get_num_input_ports(); i++) {
                if (get_tree()->exists(get_db_path("tx", i) / "freq" / "value")) {
                    get_tree()
                        ->access<double>(get_db_path("tx", i) / "freq" / "value")
                        .add_coerced_subscriber([this, i](const double freq) {
                            set_tx_fe_corrections(freq, i);
                        });
                }
            }
        }
        if (not _rx_fe_map.empty()) {
            for (size_t i = 0; i < get_num_output_ports(); i++) {
                if (get_tree()->exists(get_db_path("rx", i) / "freq" / "value")) {
                    get_tree()
                        ->access<double>(get_db_path("rx", i) / "freq" / "value")
                        .add_coerced_subscriber([this, i](const double freq) {
                            set_rx_fe_corrections(freq, i);
                        });
                }
            }
        }

        ////////////////////////////////////////////////////////////////
        // Set gain groups
        // Note: The actual gain control comes from the daughterboard drivers, thus,
        // we need to call into the prop tree at the appropriate location in order
        // to modify the gains.
        ////////////////////////////////////////////////////////////////
        // TX
        for (size_t chan = 0; chan < get_num_input_ports(); chan++) {
            fs_path rf_gains_path(get_db_path("tx", chan) / "gains");
            if (!get_tree()->exists(rf_gains_path)) {
                _tx_gain_groups[chan] = gain_group::make_zero();
                continue;
            }

            std::vector<std::string> gain_stages = get_tree()->list(rf_gains_path);
            if (gain_stages.empty()) {
                _tx_gain_groups[chan] = gain_group::make_zero();
                continue;
            }

            // DAC does not have a gain path
            auto gg = gain_group::make();
            for (const auto& name : gain_stages) {
                gg->register_fcns(name,
                    make_gain_fcns_from_subtree(
                        get_tree()->subtree(rf_gains_path / name)),
                    1 /* high prio */);
            }
            _tx_gain_groups[chan] = gg;
        }
        // RX
        for (size_t chan = 0; chan < get_num_output_ports(); chan++) {
            fs_path rf_gains_path(get_db_path("rx", chan) / "gains");
            fs_path adc_gains_path("rx_codec/gains");

            auto gg = gain_group::make();
            // ADC also has a gain path
            for (const auto& name : get_tree()->list(adc_gains_path)) {
                gg->register_fcns("ADC-" + name,
                    make_gain_fcns_from_subtree(
                        get_tree()->subtree(adc_gains_path / name)),
                    0 /* low prio */);
            }
            if (get_tree()->exists(rf_gains_path)) {
                for (const auto& name : get_tree()->list(rf_gains_path)) {
                    gg->register_fcns(name,
                        make_gain_fcns_from_subtree(
                            get_tree()->subtree(rf_gains_path / name)),
                        1 /* high prio */);
                }
            }
            _rx_gain_groups[chan] = gg;
        }
    } /* _init_dboards */

    void _set_db_eeprom(i2c_iface::sptr i2c,
        const size_t addr,
        const uhd::usrp::dboard_eeprom_t& db_eeprom)
    {
        db_eeprom.store(*i2c, addr);
        _db_eeproms[addr] = db_eeprom;
    }

    void _update_atr_leds(const std::string& rx_ant, const size_t /*chan*/)
    {
        // The "RX1" port is used by TwinRX and the "TX/RX" port is used by all
        // other full-duplex dboards. We need to handle both here.
        const bool is_txrx = (rx_ant == "TX/RX" or rx_ant == "RX1");
        const int TXRX_RX  = (1 << 0);
        const int TXRX_TX  = (1 << 1);
        const int RX2_RX   = (1 << 2);
        _leds->set_atr_reg(gpio_atr::ATR_REG_IDLE, 0);
        _leds->set_atr_reg(gpio_atr::ATR_REG_RX_ONLY, is_txrx ? TXRX_RX : RX2_RX);
        _leds->set_atr_reg(gpio_atr::ATR_REG_TX_ONLY, TXRX_TX);
        _leds->set_atr_reg(gpio_atr::ATR_REG_FULL_DUPLEX, RX2_RX | TXRX_TX);
    }

    void set_rx_fe_corrections(const double lo_freq, const size_t chan)
    {
        if (not _ignore_cal_file) {
            apply_rx_fe_corrections(get_tree(),
                get_tree()->access<dboard_eeprom_t>(DB_PATH / "rx_eeprom").get().serial,
                get_fe_path("rx", chan),
                lo_freq);
        }
    }

    void set_tx_fe_corrections(const double lo_freq, const size_t chan)
    {
        if (not _ignore_cal_file) {
            apply_tx_fe_corrections(get_tree(),
                get_tree()->access<dboard_eeprom_t>(DB_PATH / "tx_eeprom").get().serial,
                get_fe_path("tx", chan),
                lo_freq);
        }
    }

    /**************************************************************************
     * noc_block_base API
     *************************************************************************/
    //! Safely shut down all peripherals
    //
    // Reminder: After this is called, no peeks and pokes are allowed!
    void deinit()
    {
        RFNOC_LOG_TRACE("deinit()");
        // Reset daughterboard
        _db_manager.reset();
        _db_iface.reset();
        // Reset codecs
        if (_radio_type == PRIMARY) {
            _regs->misc_outs_reg.set(radio_regmap_t::misc_outs_reg_t::ADC_RESET, 1);
            _regs->misc_outs_reg.set(radio_regmap_t::misc_outs_reg_t::DAC_RESET_N, 0);
        }
        _regs->misc_outs_reg.write(radio_regmap_t::misc_outs_reg_t::DAC_ENABLED, 0);
        _regs->misc_outs_reg.flush();
        _adc.reset();
        _dac.reset();
        // Destroy all other periph controls
        _spi.reset();
        _fp_gpio.reset();
        _leds.reset();
        _rx_fe_map.clear();
        _tx_fe_map.clear();
    }


    /**************************************************************************
     * Attributes
     *************************************************************************/
    //! Register space for the ADC and DAC
    class radio_regmap_t : public uhd::soft_regmap_t
    {
    public:
        class misc_outs_reg_t : public uhd::soft_reg32_wo_t
        {
        public:
            UHD_DEFINE_SOFT_REG_FIELD(DAC_ENABLED, /*width*/ 1, /*shift*/ 0); //[0]
            UHD_DEFINE_SOFT_REG_FIELD(DAC_RESET_N, /*width*/ 1, /*shift*/ 1); //[1]
            UHD_DEFINE_SOFT_REG_FIELD(ADC_RESET, /*width*/ 1, /*shift*/ 2); //[2]
            UHD_DEFINE_SOFT_REG_FIELD(ADC_DATA_DLY_STB, /*width*/ 1, /*shift*/ 3); //[3]
            UHD_DEFINE_SOFT_REG_FIELD(ADC_DATA_DLY_VAL, /*width*/ 5, /*shift*/ 4); //[8:4]
            UHD_DEFINE_SOFT_REG_FIELD(
                ADC_CHECKER_ENABLED, /*width*/ 1, /*shift*/ 9); //[9]
            UHD_DEFINE_SOFT_REG_FIELD(DAC_SYNC, /*width*/ 1, /*shift*/ 10); //[10]

            misc_outs_reg_t() : uhd::soft_reg32_wo_t(x300_regs::SR_MISC_OUTS)
            {
                // Initial values
                set(DAC_ENABLED, 0);
                set(DAC_RESET_N, 0);
                set(ADC_RESET, 0);
                set(ADC_DATA_DLY_STB, 0);
                set(ADC_DATA_DLY_VAL, 16);
                set(ADC_CHECKER_ENABLED, 0);
                set(DAC_SYNC, 0);
            }
        } misc_outs_reg;

        class misc_ins_reg_t : public uhd::soft_reg64_ro_t
        {
        public:
            UHD_DEFINE_SOFT_REG_FIELD(
                ADC_CHECKER0_Q_LOCKED, /*width*/ 1, /*shift*/ 32); //[0]
            UHD_DEFINE_SOFT_REG_FIELD(
                ADC_CHECKER0_I_LOCKED, /*width*/ 1, /*shift*/ 33); //[1]
            UHD_DEFINE_SOFT_REG_FIELD(
                ADC_CHECKER1_Q_LOCKED, /*width*/ 1, /*shift*/ 34); //[2]
            UHD_DEFINE_SOFT_REG_FIELD(
                ADC_CHECKER1_I_LOCKED, /*width*/ 1, /*shift*/ 35); //[3]
            UHD_DEFINE_SOFT_REG_FIELD(
                ADC_CHECKER0_Q_ERROR, /*width*/ 1, /*shift*/ 36); //[4]
            UHD_DEFINE_SOFT_REG_FIELD(
                ADC_CHECKER0_I_ERROR, /*width*/ 1, /*shift*/ 37); //[5]
            UHD_DEFINE_SOFT_REG_FIELD(
                ADC_CHECKER1_Q_ERROR, /*width*/ 1, /*shift*/ 38); //[6]
            UHD_DEFINE_SOFT_REG_FIELD(
                ADC_CHECKER1_I_ERROR, /*width*/ 1, /*shift*/ 39); //[7]

            misc_ins_reg_t() : uhd::soft_reg64_ro_t(x300_regs::RB_MISC_IO) {}
        } misc_ins_reg;

        radio_regmap_t(int radio_num)
            : soft_regmap_t("radio" + std::to_string(radio_num) + "_regmap")
        {
            add_to_map(misc_outs_reg, "misc_outs_reg", PRIVATE);
            add_to_map(misc_ins_reg, "misc_ins_reg", PRIVATE);
        }
    }; /* class radio_regmap_t */
    //! wb_iface Instance for _regs
    uhd::timed_wb_iface::sptr _wb_iface;
    //! Instantiation of regs object for ADC and DAC (MISC_OUT register)
    std::unique_ptr<radio_regmap_t> _regs;
    //! Reference to the MB controller, typecast
    std::shared_ptr<x300_mb_controller> _x300_mb_control;

    //! Reference to the DBoard SPI core (also controls ADC/DAC)
    spi_core_3000::sptr _spi;
    //! Reference to the ADC controller
    x300_adc_ctrl::sptr _adc;
    //! Reference to the DAC controller
    x300_dac_ctrl::sptr _dac;
    //! Front-panel GPIO
    usrp::gpio_atr::gpio_atr_3000::sptr _fp_gpio;
    //! LEDs
    usrp::gpio_atr::gpio_atr_3000::sptr _leds;

    struct rx_fe_perif
    {
        std::string name;
        std::string db_fe_name;
        rx_frontend_core_3000::sptr core;
    };
    struct tx_fe_perif
    {
        std::string name;
        std::string db_fe_name;
        tx_frontend_core_200::sptr core;
    };

    std::unordered_map<size_t, rx_fe_perif> _rx_fe_map;
    std::unordered_map<size_t, tx_fe_perif> _tx_fe_map;

    //! Cache of EEPROM info (one per channel)
    std::unordered_map<size_t, usrp::dboard_eeprom_t> _db_eeproms;
    //! Reference to DB manager
    usrp::dboard_manager::sptr _db_manager;
    //! Reference to DB iface
    boost::shared_ptr<x300_dboard_iface> _db_iface;

    enum radio_connection_t { PRIMARY, SECONDARY };
    radio_connection_t _radio_type;

    bool _ignore_cal_file = false;

    std::unordered_map<size_t, uhd::gain_group::sptr> _tx_gain_groups;
    std::unordered_map<size_t, uhd::gain_group::sptr> _rx_gain_groups;

    double _master_clock_rate = DEFAULT_RATE;
};

UHD_RFNOC_BLOCK_REGISTER_FOR_DEVICE_DIRECT(
    x300_radio_control, RADIO_BLOCK, X300, "Radio", true, "radio_clk", "radio_clk")
