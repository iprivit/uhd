//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/exception.hpp>
#include <uhd/utils/log.hpp>
#include <uhdlib/rfnoc/radio_control_impl.hpp>
#include <uhdlib/utils/compat_check.hpp>
#include <map>
#include <tuple>

using namespace uhd::rfnoc;

namespace {

inline uint32_t get_addr(const uint32_t base_addr, const size_t chan)
{
    return radio_control_impl::regmap::RADIO_BASE_ADDR + base_addr
           + radio_control_impl::regmap::REG_CHAN_OFFSET * chan;
}

const std::string DEFAULT_GAIN_PROFILE("default");

} // namespace

const std::string radio_control::ALL_LOS = "all";
const std::string radio_control::ALL_GAINS = "";
const size_t radio_control::ALL_CHANS      = size_t(~0);

const uint16_t radio_control_impl::MAJOR_COMPAT = 0;
const uint16_t radio_control_impl::MINOR_COMPAT = 0;

const uint32_t radio_control_impl::regmap::REG_COMPAT_NUM;
const uint32_t radio_control_impl::regmap::REG_RADIO_WIDTH;
const uint32_t radio_control_impl::regmap::RADIO_BASE_ADDR;
const uint32_t radio_control_impl::regmap::REG_CHAN_OFFSET;
const uint32_t radio_control_impl::regmap::RADIO_ADDR_W;
const uint32_t radio_control_impl::regmap::REG_LOOPBACK_EN;
const uint32_t radio_control_impl::regmap::REG_RX_STATUS;
const uint32_t radio_control_impl::regmap::REG_RX_CMD;
const uint32_t radio_control_impl::regmap::REG_RX_CMD_NUM_WORDS_LO;
const uint32_t radio_control_impl::regmap::REG_RX_CMD_NUM_WORDS_HI;
const uint32_t radio_control_impl::regmap::REG_RX_CMD_TIME_LO;
const uint32_t radio_control_impl::regmap::REG_RX_CMD_TIME_HI;
const uint32_t radio_control_impl::regmap::REG_RX_MAX_WORDS_PER_PKT;
const uint32_t radio_control_impl::regmap::REG_RX_ERR_PORT;
const uint32_t radio_control_impl::regmap::REG_RX_ERR_REM_PORT;
const uint32_t radio_control_impl::regmap::REG_RX_ERR_REM_EPID;
const uint32_t radio_control_impl::regmap::REG_RX_ERR_ADDR;
const uint32_t radio_control_impl::regmap::REG_TX_IDLE_VALUE;
const uint32_t radio_control_impl::regmap::REG_TX_ERROR_POLICY;
const uint32_t radio_control_impl::regmap::REG_TX_ERR_PORT;
const uint32_t radio_control_impl::regmap::REG_TX_ERR_REM_PORT;
const uint32_t radio_control_impl::regmap::REG_TX_ERR_REM_EPID;
const uint32_t radio_control_impl::regmap::REG_TX_ERR_ADDR;
const uint32_t radio_control_impl::regmap::RX_CMD_STOP;
const uint32_t radio_control_impl::regmap::RX_CMD_FINITE;
const uint32_t radio_control_impl::regmap::RX_CMD_CONTINUOUS;
const uint32_t radio_control_impl::regmap::RX_CMD_TIMED_POS;

const uhd::fs_path radio_control_impl::DB_PATH("dboard");
const uhd::fs_path radio_control_impl::FE_PATH("frontends");

/****************************************************************************
 * Structors
 ***************************************************************************/
radio_control_impl::radio_control_impl(make_args_ptr make_args)
    : radio_control(std::move(make_args))
    , _fpga_compat(regs().peek32(regmap::REG_COMPAT_NUM))
    , _radio_width(regs().peek32(regmap::REG_RADIO_WIDTH))
    , _samp_width(_radio_width >> 16)
    , _spc(_radio_width & 0xFFFF)
    , _last_stream_cmd(
          get_num_output_ports(), uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS)
{
    uhd::assert_fpga_compat(MAJOR_COMPAT,
        MINOR_COMPAT,
        _fpga_compat,
        get_unique_id(),
        get_unique_id(),
        false /* Let it slide if minors mismatch */
    );
    RFNOC_LOG_TRACE(
        "Loading radio with SPC=" << _spc << ", num_inputs=" << get_num_input_ports()
                                  << ", num_outputs=" << get_num_output_ports());
    set_prop_forwarding_policy(forwarding_policy_t::DROP);
    set_action_forwarding_policy(forwarding_policy_t::DROP);
    register_action_handler(ACTION_KEY_STREAM_CMD,
        [this](const res_source_info& src, action_info::sptr action) {
            stream_cmd_action_info::sptr stream_cmd_action =
                std::dynamic_pointer_cast<stream_cmd_action_info>(action);
            if (!stream_cmd_action) {
                RFNOC_LOG_WARNING("Received invalid stream command action!");
                return;
            }
            RFNOC_LOG_TRACE(
                "Received stream command: " << stream_cmd_action->stream_cmd.stream_mode
                                            << " to " << src.to_string());
            if (src.type != res_source_info::OUTPUT_EDGE) {
                RFNOC_LOG_WARNING("Received stream command, but not to output port! Ignoring.");
                return;
            }
            const size_t port = src.instance;
            if (port > get_num_output_ports()) {
                RFNOC_LOG_WARNING("Received stream command to invalid output port!");
                return;
            }
            issue_stream_cmd(stream_cmd_action->stream_cmd, port);
        });
    // Register spp properties and resolvers
    _spp_prop.reserve(get_num_output_ports());
    _samp_rate_in.reserve(get_num_input_ports());
    _samp_rate_out.reserve(get_num_output_ports());
    _type_in.reserve(get_num_input_ports());
    _type_out.reserve(get_num_output_ports());
    for (size_t chan = 0; chan < get_num_output_ports(); ++chan) {
        _spp_prop.push_back(property_t<int>(
            PROP_KEY_SPP, DEFAULT_SPP, {res_source_info::USER, chan}));
        _samp_rate_in.push_back(property_t<double>(
            PROP_KEY_SAMP_RATE, get_tick_rate(), {res_source_info::INPUT_EDGE, chan}));
        _samp_rate_out.push_back(property_t<double>(
            PROP_KEY_SAMP_RATE, get_tick_rate(), {res_source_info::OUTPUT_EDGE, chan}));
        _type_in.push_back(property_t<io_type_t>(
            PROP_KEY_TYPE, IO_TYPE_SC16, {res_source_info::INPUT_EDGE, chan}));
        _type_out.push_back(property_t<io_type_t>(
            PROP_KEY_TYPE, IO_TYPE_SC16, {res_source_info::OUTPUT_EDGE, chan}));

        register_property(&_spp_prop.back(), [this, chan, &spp = _spp_prop.back()]() {
            const uint32_t words_per_pkt = spp.get();
            RFNOC_LOG_TRACE(
                "Setting words_per_pkt to " << words_per_pkt << " on chan " << chan);
            regs().poke32(
                get_addr(regmap::REG_RX_MAX_WORDS_PER_PKT, chan), words_per_pkt);
        });
        register_property(&_samp_rate_in.back());
        register_property(&_samp_rate_out.back());
        register_property(&_type_in.back());
        register_property(&_type_out.back());
        add_property_resolver({&_spp_prop.back()},
            {&_spp_prop.back()},
            [this, chan, &spp = _spp_prop.back()]() {
                RFNOC_LOG_TRACE("Calling resolver for spp@" << chan);
                if (spp.get() % _spc) {
                    spp = spp.get() - (spp.get() % _spc);
                    RFNOC_LOG_WARNING(
                        "spp must be a multiple of the block bus width! Coercing to "
                        << spp.get());
                }
                if (spp.get() <= 0) {
                    spp = DEFAULT_SPP;
                    RFNOC_LOG_WARNING(
                        "spp must be greater than zero! Coercing to " << spp.get());
                }
            });
        add_property_resolver({&_samp_rate_in.back(), &_samp_rate_out.back()},
            {&_samp_rate_in.back(), &_samp_rate_out.back()},
            [this, chan,
                &samp_rate_in  = _samp_rate_in.at(chan),
                &samp_rate_out = _samp_rate_out.at(chan)]() {
                RFNOC_LOG_TRACE("Calling resolver for samp_rate@" << chan);
                samp_rate_in = set_rate(samp_rate_in.get());
                samp_rate_out = samp_rate_in.get();
            });
        // Resolvers for type: These are constants
        add_property_resolver({&_type_in.back()},
            {&_type_in.back()},
            [& type_in = _type_in.back()]() { type_in.set(IO_TYPE_SC16); });
        add_property_resolver({&_type_out.back()},
            {&_type_out.back()},
            [& type_out = _type_out.back()]() { type_out.set(IO_TYPE_SC16); });
    }
    // Enable async messages coming from the radio
    const uint32_t xbar_port = 1; // FIXME: Find a better way to figure this out
    RFNOC_LOG_TRACE("Sending async messages to EPID "
                    << regs().get_src_epid() << ", remote port " << regs().get_port_num()
                    << ", xbar port " << xbar_port);
    for (size_t tx_chan = 0; tx_chan < get_num_output_ports(); tx_chan++) {
        // Set the EPID and port of our regs() object (all async messages go to
        // the same location)
        regs().poke32(
            get_addr(regmap::REG_TX_ERR_REM_EPID, tx_chan), regs().get_src_epid());
        regs().poke32(
            get_addr(regmap::REG_TX_ERR_REM_PORT, tx_chan), regs().get_port_num());
        // Set the crossbar port for the async packet routing
        regs().poke32(get_addr(regmap::REG_TX_ERR_PORT, tx_chan), xbar_port);
        // Set the async message address
        regs().poke32(get_addr(regmap::REG_TX_ERR_ADDR, tx_chan),
            regmap::SWREG_TX_ERR + regmap::SWREG_CHAN_OFFSET * tx_chan);
    }
    for (size_t rx_chan = 0; rx_chan < get_num_input_ports(); rx_chan++) {
        // Set the EPID and port of our regs() object (all async messages go to
        // the same location)
        regs().poke32(
            get_addr(regmap::REG_RX_ERR_REM_EPID, rx_chan), regs().get_src_epid());
        regs().poke32(
            get_addr(regmap::REG_RX_ERR_REM_PORT, rx_chan), regs().get_port_num());
        // Set the crossbar port for the async packet routing
        regs().poke32(get_addr(regmap::REG_RX_ERR_PORT, rx_chan), xbar_port);
        // Set the async message address
        regs().poke32(get_addr(regmap::REG_RX_ERR_ADDR, rx_chan),
            regmap::SWREG_RX_ERR + regmap::SWREG_CHAN_OFFSET * rx_chan);
    }
    // Now register a function to receive the async messages
    regs().register_async_msg_handler([this](uint32_t addr,
                                          const std::vector<uint32_t>& data,
                                          boost::optional<uint64_t> timestamp) {
        this->async_message_handler(addr, data, timestamp);
    });
} /* ctor */

/******************************************************************************
 * Rate-Related API Calls
 *****************************************************************************/
double radio_control_impl::set_rate(const double rate)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    _rate = rate;
    return rate;
    // FIXME:
    ////_tick_rate = rate;
    ////_time64->set_tick_rate(_tick_rate);
    ////_time64->self_test();
    //// set_command_tick_rate(rate);
}

double radio_control_impl::get_rate() const
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _rate;
}

uhd::meta_range_t radio_control_impl::get_rate_range() const
{
    RFNOC_LOG_TRACE("Using default implementation of get_rx_rate_range()");
    uhd::meta_range_t result;
    result.push_back(get_rate());
    return result;
}

/****************************************************************************
 * RF API
 ***************************************************************************/
void radio_control_impl::set_tx_antenna(const std::string& ant, const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    _tx_antenna[chan] = ant;
}

void radio_control_impl::set_rx_antenna(const std::string& ant, const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    _rx_antenna[chan] = ant;
}

double radio_control_impl::set_tx_frequency(const double freq, const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _tx_freq[chan] = freq;
}

void radio_control_impl::set_tx_tune_args(const uhd::device_addr_t&, const size_t)
{
    RFNOC_LOG_TRACE("tune_args not supported by this radio.");
}

double radio_control_impl::set_rx_frequency(const double freq, const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _rx_freq[chan] = freq;
}

void radio_control_impl::set_rx_tune_args(const uhd::device_addr_t&, const size_t)
{
    RFNOC_LOG_TRACE("tune_args not supported by this radio.");
}

std::vector<std::string> radio_control_impl::get_tx_gain_names(const size_t) const
{
    return {ALL_GAINS};
}

std::vector<std::string> radio_control_impl::get_rx_gain_names(const size_t) const
{
    return {ALL_GAINS};
}

uhd::gain_range_t radio_control_impl::get_tx_gain_range(const size_t chan) const
{
    RFNOC_LOG_DEBUG("Using default implementation of get_tx_gain_range()");
    uhd::gain_range_t result;
    std::lock_guard<std::mutex> l(_cache_mutex);
    result.push_back(_rx_gain.at(chan));
    return result;
}

uhd::gain_range_t radio_control_impl::get_tx_gain_range(
    const std::string& name, const size_t chan) const
{
    if (name != ALL_GAINS) {
        throw uhd::value_error(
            std::string("get_tx_gain_range(): Unknown gain name `") + name + "'!");
    }
    return get_tx_gain_range(chan);
}

uhd::gain_range_t radio_control_impl::get_rx_gain_range(const size_t chan) const
{
    RFNOC_LOG_DEBUG("Using default implementation of get_rx_gain_range()");
    uhd::gain_range_t result;
    std::lock_guard<std::mutex> l(_cache_mutex);
    result.push_back(_rx_gain.at(chan));
    return result;
}

uhd::gain_range_t radio_control_impl::get_rx_gain_range(const std::string& name, const size_t chan) const
{
    if (name != ALL_GAINS) {
        throw uhd::value_error(
            std::string("get_rx_gain_range(): Unknown gain name `") + name + "'!");
    }
    return get_rx_gain_range(chan);
}

double radio_control_impl::set_tx_gain(const double gain, const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    _tx_gain[chan] = gain;
    return gain;
}

double radio_control_impl::set_tx_gain(const double gain, const std::string& name, const size_t chan)
{
    if (name != ALL_GAINS) {
        throw uhd::key_error(
            std::string("set_tx_gain(): Gain name `") + name + "' is not defined!");
    }
    return set_tx_gain(gain, chan);
}

double radio_control_impl::set_rx_gain(const double gain, const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    _rx_gain[chan] = gain;
    return gain;
}

double radio_control_impl::set_rx_gain(const double gain, const std::string& name, const size_t chan)
{
    if (name != ALL_GAINS) {
        throw uhd::key_error(
            std::string("set_rx_gain(): Gain name `") + name + "' is not defined!");
    }
    return set_rx_gain(gain, chan);
}

void radio_control_impl::set_rx_agc(const bool, const size_t)
{
    throw uhd::not_implemented_error("set_rx_agc() is not supported on this radio!");
}

void radio_control_impl::set_tx_gain_profile(const std::string& profile, const size_t)
{
    if (profile != DEFAULT_GAIN_PROFILE) {
        throw uhd::value_error(
            std::string("set_tx_gain_profile(): Unknown gain profile: `") + profile
            + "'");
    }
}

void radio_control_impl::set_rx_gain_profile(const std::string& profile, const size_t)
{
    if (profile != DEFAULT_GAIN_PROFILE) {
        throw uhd::value_error(
            std::string("set_rx_gain_profile(): Unknown gain profile: `") + profile
            + "'");
    }
}

std::vector<std::string> radio_control_impl::get_tx_gain_profile_names(const size_t) const
{
    return {DEFAULT_GAIN_PROFILE};
}

std::vector<std::string> radio_control_impl::get_rx_gain_profile_names(const size_t) const
{
    return {DEFAULT_GAIN_PROFILE};
}

std::string radio_control_impl::get_tx_gain_profile(const size_t) const
{
    return DEFAULT_GAIN_PROFILE;
}

std::string radio_control_impl::get_rx_gain_profile(const size_t) const
{
    return DEFAULT_GAIN_PROFILE;
}

double radio_control_impl::set_tx_bandwidth(const double bandwidth, const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _tx_bandwidth[chan] = bandwidth;
}

double radio_control_impl::set_rx_bandwidth(const double bandwidth, const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _rx_bandwidth[chan] = bandwidth;
}

std::string radio_control_impl::get_tx_antenna(const size_t chan) const
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _tx_antenna.at(chan);
}

std::string radio_control_impl::get_rx_antenna(const size_t chan) const
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _rx_antenna.at(chan);
}

std::vector<std::string> radio_control_impl::get_tx_antennas(const size_t chan) const
{
    RFNOC_LOG_DEBUG("get_tx_antennas(): Using default implementation.");
    std::lock_guard<std::mutex> l(_cache_mutex);
    return {_tx_antenna.at(chan)};
}

std::vector<std::string> radio_control_impl::get_rx_antennas(const size_t chan) const
{
    RFNOC_LOG_DEBUG("get_rx_antennas(): Using default implementation.");
    std::lock_guard<std::mutex> l(_cache_mutex);
    return {_rx_antenna.at(chan)};
}

double radio_control_impl::get_tx_frequency(const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _tx_freq.at(chan);
}

double radio_control_impl::get_rx_frequency(const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _rx_freq.at(chan);
}

uhd::freq_range_t radio_control_impl::get_tx_frequency_range(const size_t) const
{
    RFNOC_LOG_WARNING(
        "get_tx_frequency_range() not implemented! Returning current rate only.");
    uhd::freq_range_t result;
    result.push_back(get_rate());
    return result;
}

uhd::freq_range_t radio_control_impl::get_rx_frequency_range(const size_t) const
{
    RFNOC_LOG_WARNING(
        "get_rx_frequency_range() not implemented! Returning current rate only.");
    uhd::freq_range_t result;
    result.push_back(get_rate());
    return result;
}

double radio_control_impl::get_tx_gain(const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _tx_gain.at(chan);
}

double radio_control_impl::get_rx_gain(const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _rx_gain.at(chan);
}

double radio_control_impl::get_tx_gain(const std::string& name, const size_t chan)
{
    if (name != ALL_GAINS) {
        throw uhd::value_error(std::string("get_tx_gain(): Unknown gain name `") + name + "'");
    }
    return get_tx_gain(chan);
}

double radio_control_impl::get_rx_gain(const std::string& name, const size_t chan)
{
    if (name != ALL_GAINS) {
        throw uhd::value_error(std::string("get_rx_gain(): Unknown gain name `") + name + "'");
    }
    return get_rx_gain(chan);
}

double radio_control_impl::get_tx_bandwidth(const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _tx_bandwidth.at(chan);
}

double radio_control_impl::get_rx_bandwidth(const size_t chan)
{
    std::lock_guard<std::mutex> l(_cache_mutex);
    return _rx_bandwidth.at(chan);
}

uhd::meta_range_t radio_control_impl::get_tx_bandwidth_range(size_t chan) const
{
    RFNOC_LOG_DEBUG("get_tx_bandwidth_range(): Using default implementation.");
    uhd::meta_range_t result;
    std::lock_guard<std::mutex> l(_cache_mutex);
    result.push_back(_rx_bandwidth.at(chan));
    return result;
}

uhd::meta_range_t radio_control_impl::get_rx_bandwidth_range(size_t chan) const
{
    RFNOC_LOG_DEBUG("get_tx_bandwidth_range(): Using default implementation.");
    uhd::meta_range_t result;
    std::lock_guard<std::mutex> l(_cache_mutex);
    result.push_back(_rx_bandwidth.at(chan));
    return result;
}

/******************************************************************************
 * LO Default API
 *****************************************************************************/
std::vector<std::string> radio_control_impl::get_rx_lo_names(const size_t) const
{
    return {};
}

std::vector<std::string> radio_control_impl::get_rx_lo_sources(
    const std::string&, const size_t) const
{
    return {"internal"};
}

uhd::freq_range_t radio_control_impl::get_rx_lo_freq_range(
    const std::string&, const size_t) const
{
    return uhd::freq_range_t();
}

void radio_control_impl::set_rx_lo_source(
    const std::string&, const std::string&, const size_t)
{
    throw uhd::not_implemented_error("set_rx_lo_source is not supported on this radio");
}

const std::string radio_control_impl::get_rx_lo_source(const std::string&, const size_t)
{
    return "internal";
}

void radio_control_impl::set_rx_lo_export_enabled(bool, const std::string&, const size_t)
{
    throw uhd::not_implemented_error(
        "set_rx_lo_export_enabled is not supported on this radio");
}

bool radio_control_impl::get_rx_lo_export_enabled(const std::string&, const size_t) const
{
    return false;
}
double radio_control_impl::set_rx_lo_freq(double, const std::string&, const size_t)
{
    throw uhd::not_implemented_error("set_rx_lo_freq is not supported on this radio");
}

double radio_control_impl::get_rx_lo_freq(const std::string&, const size_t chan)
{
    return get_rx_frequency(chan);
}

std::vector<std::string> radio_control_impl::get_tx_lo_names(const size_t) const
{
    return {};
}

std::vector<std::string> radio_control_impl::get_tx_lo_sources(const std::string&, const size_t)
{
    return {"internal"};
}

uhd::freq_range_t radio_control_impl::get_tx_lo_freq_range(const std::string&, const size_t)
{
    return uhd::freq_range_t();
}

void radio_control_impl::set_tx_lo_source(
    const std::string&, const std::string&, const size_t)
{
    throw uhd::not_implemented_error("set_tx_lo_source is not supported on this radio");
}
const std::string radio_control_impl::get_tx_lo_source(const std::string&, const size_t)
{
    return "internal";
}

void radio_control_impl::set_tx_lo_export_enabled(
    const bool, const std::string&, const size_t)
{
    throw uhd::not_implemented_error(
        "set_tx_lo_export_enabled is not supported on this radio");
}

bool radio_control_impl::get_tx_lo_export_enabled(const std::string&, const size_t)
{
    return false;
}

double radio_control_impl::set_tx_lo_freq(const double, const std::string&, const size_t)
{
    throw uhd::not_implemented_error("set_tx_lo_freq is not supported on this radio");
}

double radio_control_impl::get_tx_lo_freq(const std::string&, const size_t chan)
{
    return get_tx_frequency(chan);
}

/******************************************************************************
 * Calibration-Related API Calls
 *****************************************************************************/
void radio_control_impl::set_tx_dc_offset(const std::complex<double>&, size_t)
{
    throw uhd::not_implemented_error("set_tx_dc_offset() is not supported on this radio");
}

uhd::meta_range_t radio_control_impl::get_tx_dc_offset_range(size_t) const
{
    return uhd::meta_range_t(0, 0);
}

void radio_control_impl::set_tx_iq_balance(const std::complex<double>&, size_t)
{
    throw uhd::not_implemented_error(
        "set_tx_iq_balance() is not supported on this radio");
}

void radio_control_impl::set_rx_dc_offset(const bool enb, size_t)
{
    RFNOC_LOG_DEBUG("set_rx_dc_offset() has no effect on this radio");
    if (enb) {
        throw uhd::not_implemented_error(
            "set_rx_dc_offset() is not supported on this radio");
    }
}

void radio_control_impl::set_rx_dc_offset(const std::complex<double>&, size_t)
{
    throw uhd::not_implemented_error("set_rx_dc_offset() is not supported on this radio");
}

uhd::meta_range_t radio_control_impl::get_rx_dc_offset_range(size_t) const
{
    return uhd::meta_range_t(0, 0);
}

void radio_control_impl::set_rx_iq_balance(const bool enb, size_t)
{
    RFNOC_LOG_DEBUG("set_rx_iq_balance() has no effect on this radio");
    if (enb) {
        throw uhd::not_implemented_error(
            "set_rx_iq_balance() is not supported on this radio");
    }
}

void radio_control_impl::set_rx_iq_balance(const std::complex<double>&, size_t)
{
    throw uhd::not_implemented_error(
        "set_rx_iq_balance() is not supported on this radio");
}

/******************************************************************************
 * GPIO Controls
 *****************************************************************************/
std::vector<std::string> radio_control_impl::get_gpio_banks() const
{
    return {};
}

void radio_control_impl::set_gpio_attr(
    const std::string&, const std::string&, const uint32_t, const uint32_t)
{
    throw uhd::not_implemented_error("set_gpio_attr() not implemented on this radio!");
}

uint32_t radio_control_impl::get_gpio_attr(const std::string&, const std::string&)
{
    throw uhd::not_implemented_error("get_gpio_attr() not implemented on this radio!");
}

/**************************************************************************
 * Sensor API
 *************************************************************************/
std::vector<std::string> radio_control_impl::get_rx_sensor_names(size_t) const
{
    return {};
}

uhd::sensor_value_t radio_control_impl::get_rx_sensor(const std::string& name, size_t)
{
    throw uhd::key_error(std::string("Unknown RX sensor: ") + name);
}

std::vector<std::string> radio_control_impl::get_tx_sensor_names(size_t) const
{
    return {};
}

uhd::sensor_value_t radio_control_impl::get_tx_sensor(const std::string& name, size_t)
{
    throw uhd::key_error(std::string("Unknown TX sensor: ") + name);
}

/**************************************************************************
 * EEPROM API
 *************************************************************************/
void radio_control_impl::set_db_eeprom(const uhd::eeprom_map_t&)
{
    throw uhd::not_implemented_error("set_db_eeprom() not implemented for this radio!");
}

uhd::eeprom_map_t radio_control_impl::get_db_eeprom()
{
    return {};
}

/****************************************************************************
 * Streaming API
 ***************************************************************************/
void radio_control_impl::issue_stream_cmd(
    const uhd::stream_cmd_t& stream_cmd, const size_t chan)
{
    // std::lock_guard<std::mutex> lock(_mutex);
    RFNOC_LOG_TRACE("radio_control_impl::issue_stream_cmd(chan="
                    << chan << ", mode=" << char(stream_cmd.stream_mode) << ")");
    _last_stream_cmd[chan] = stream_cmd;

    // calculate the command word
    const std::unordered_map<stream_cmd_t::stream_mode_t, uint32_t, std::hash<size_t>>
        stream_mode_to_cmd_word{
            {stream_cmd_t::STREAM_MODE_START_CONTINUOUS, regmap::RX_CMD_CONTINUOUS},
            {stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS, regmap::RX_CMD_STOP},
            {stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE, regmap::RX_CMD_FINITE},
            {stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_MORE, regmap::RX_CMD_FINITE}};
    const uint32_t cmd_bits = stream_mode_to_cmd_word.at(stream_cmd.stream_mode);
    const uint32_t cmd_word =
        cmd_bits
        | (uint32_t((stream_cmd.stream_now) ? 0 : 1) << regmap::RX_CMD_TIMED_POS);

    if (cmd_bits == regmap::RX_CMD_FINITE) {
        if (stream_cmd.num_samps == 0) {
            throw uhd::value_error("When requesting a finite number of samples, the "
                                   "number of samples must be greater than zero.");
        }
        // FIXME: The num words might be different from num_samps, check the
        // radio width
        const uint64_t num_words         = stream_cmd.num_samps;
        constexpr uint64_t max_num_words = 0x00FFFFFFFFFFFF; // 48 bits
        if (num_words > max_num_words) {
            RFNOC_LOG_ERROR("Requesting too many samples in a single burst! "
                            "Requested "
                            + std::to_string(stream_cmd.num_samps)
                            + ", maximum "
                              "is "
                            + std::to_string(max_num_words) + "."); // FIXME
            RFNOC_LOG_INFO(
                "Note that a decimation block will increase the number of samples "
                "per burst by the decimation factor. Your application may have "
                "requested fewer samples.");
            throw uhd::value_error("Requested too many samples in a single burst.");
        }
        regs().poke32(
            get_addr(regmap::REG_RX_CMD_NUM_WORDS_HI, chan), uint32_t(num_words >> 32));
        regs().poke32(get_addr(regmap::REG_RX_CMD_NUM_WORDS_LO, chan),
            uint32_t(num_words & 0xFFFFFFFF));
    }
    if (!stream_cmd.stream_now) {
        const uint64_t ticks = stream_cmd.time_spec.to_ticks(get_tick_rate());
        regs().poke32(get_addr(regmap::REG_RX_CMD_TIME_HI, chan), uint32_t(ticks >> 32));
        regs().poke32(get_addr(regmap::REG_RX_CMD_TIME_LO, chan), uint32_t(ticks >> 0));
    }
    regs().poke32(get_addr(regmap::REG_RX_CMD, chan), cmd_word);
}

/******************************************************************************
 * Private methods
 *****************************************************************************/
void radio_control_impl::async_message_handler(
    uint32_t addr, const std::vector<uint32_t>& data, boost::optional<uint64_t> timestamp)
{
    if (data.empty()) {
        RFNOC_LOG_WARNING(
            str(boost::format("Received async message with invalid length %d!")
                % data.size()));
        return;
    }
    if (data.size() > 1) {
        RFNOC_LOG_WARNING(
            str(boost::format("Received async message with extra data, length %d!")
                % data.size()));
    }
    // Reminder: The address is calculated as:
    // BASE + 64 * chan + addr_offset
    // BASE == 0x0000 for RX, 0x1000 for TX
    const uint32_t addr_base = (addr >= regmap::SWREG_RX_ERR) ? regmap::SWREG_RX_ERR
                                                              : regmap::SWREG_TX_ERR;
    const uint32_t chan        = (addr - addr_base) / regmap::SWREG_CHAN_OFFSET;
    const uint32_t addr_offset = addr % regmap::SWREG_CHAN_OFFSET;
    const uint32_t code        = data[0];
    RFNOC_LOG_TRACE(
        str(boost::format("Received async message to addr 0x%08X, data length %d words, "
                          "%s channel %d, addr_offset %d")
            % addr % data.size() % (addr_base == regmap::SWREG_TX_ERR ? "TX" : "RX")
            % chan % addr_offset));
    if (timestamp) {
        RFNOC_LOG_TRACE(
            str(boost::format("Async message timestamp: %ul") % timestamp.get()));
    }
    switch (addr_base + addr_offset) {
        case regmap::SWREG_TX_ERR: {
            switch (code) {
                case err_codes::ERR_TX_UNDERRUN:
                    UHD_LOG_FASTPATH("U");
                    break;
                case err_codes::ERR_TX_LATE_DATA:
                    UHD_LOG_FASTPATH("L");
                    break;
            }
            break;
        }
        case regmap::SWREG_RX_ERR: {
            switch (code) {
                case err_codes::ERR_RX_OVERRUN: {
                    UHD_LOG_FASTPATH("O");
                    auto rx_event_action        = rx_event_action_info::make();
                    rx_event_action->error_code = uhd::rx_metadata_t::ERROR_CODE_OVERFLOW;
                    RFNOC_LOG_TRACE("Posting overrun event action message.");
                    post_action(res_source_info{res_source_info::OUTPUT_EDGE, chan},
                        rx_event_action);
                    break;
                }
                case err_codes::ERR_RX_LATE_CMD:
                    UHD_LOG_FASTPATH("L");
                    break;
            }
            break;
        }
        case regmap::SWREG_TX_ERR + 8:
        case regmap::SWREG_TX_ERR + 12:
        case regmap::SWREG_RX_ERR + 8:
        case regmap::SWREG_RX_ERR + 12:
            RFNOC_LOG_TRACE("Dropping timestamp info for async message.");
            break;
        default:
            RFNOC_LOG_WARNING(str(
                boost::format("Received async message to invalid addr 0x%08X!") % addr));
    }
}
