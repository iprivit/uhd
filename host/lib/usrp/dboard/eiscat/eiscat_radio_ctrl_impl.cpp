//
// Copyright 2017 Ettus Research (National Instruments Corp.)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "eiscat_radio_ctrl_impl.hpp"

#include <uhd/utils/log.hpp>
#include <uhd/utils/math.hpp>
#include <uhd/rfnoc/node_ctrl_base.hpp>
#include <uhd/types/ranges.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/make_shared.hpp>
#include <boost/date_time/posix_time/posix_time_io.hpp>
#include <boost/format.hpp>

using namespace uhd;
using namespace uhd::usrp;
using namespace uhd::rfnoc;

namespace {
    const size_t SR_ANTENNA_GAIN_BASE     = 204;
    const size_t SR_ANTENNA_SELECT_BASE   = 192; // Note: On other dboards, 192 is DB_GPIO address space

    const double EISCAT_TICK_RATE         = 208e6; // Hz
    const double EISCAT_RADIO_RATE        = 104e6; // Hz
    const double EISCAT_CENTER_FREQ       = 104e6; // Hz
    const double EISCAT_DEFAULT_NULL_GAIN = 0.0; // dB. This is not the digital antenna gain, this a fake stub value.
    const double EISCAT_DEFAULT_BANDWIDTH = 52e6; // Hz
    const char*  EISCAT_DEFAULT_ANTENNA   = "BF";
    const size_t EISCAT_NUM_ANTENNAS      = 16;
    const size_t EISCAT_NUM_BEAMS         = 10;
    const size_t EISCAT_NUM_PORTS         = 5;
    const size_t EISCAT_GAIN_RANGE        = 18; // Bits, *signed*.
    const int32_t EISCAT_MAX_GAIN         = (1<<(EISCAT_GAIN_RANGE-1))-1;
    const int32_t EISCAT_MIN_GAIN         = -(1<<(EISCAT_GAIN_RANGE-1));
    const double EISCAT_DEFAULT_NORM_GAIN = 1.0; // Normalized. This is the actual digital gain value.
    const size_t EISCAT_BITS_PER_TAP      = 18;
    const eiscat_radio_ctrl_impl::fir_tap_t EISCAT_MAX_TAP_VALUE  = (1<<(EISCAT_BITS_PER_TAP-1))-1;
    const eiscat_radio_ctrl_impl::fir_tap_t EISCAT_MIN_TAP_VALUE  = -(1<<(EISCAT_BITS_PER_TAP-1));
    const size_t EISCAT_NUM_FIR_TAPS      = 10;
    const size_t EISCAT_NUM_FIR_SETS      = 1024; // BRAM must be at least EISCAT_NUM_FIR_TAPS * EISCAT_NUM_FIR_SETS
    const size_t EISCAT_FIR_INDEX_IMPULSE = 1002;
    const size_t EISCAT_FIR_INDEX_ZEROS   = 1003; // FIXME
};


UHD_RFNOC_RADIO_BLOCK_CONSTRUCTOR(eiscat_radio_ctrl)
{
    UHD_LOG_TRACE("EISCAT", "eiscat_radio_ctrl_impl::ctor() ");
    _num_ports = get_output_ports().size();
    UHD_LOG_TRACE("EISCAT", "Number of channels: " << _num_ports);
    UHD_LOG_TRACE("EISCAT", "Setting tick rate to " << EISCAT_TICK_RATE/1e6 << " MHz");

    /**** Configure the radio_ctrl itself ***********************************/
    radio_ctrl_impl::set_rate(EISCAT_TICK_RATE);
    for (size_t chan = 0; chan < _num_ports; chan++) {
        radio_ctrl_impl::set_rx_frequency(EISCAT_CENTER_FREQ, chan);
        radio_ctrl_impl::set_rx_gain(EISCAT_DEFAULT_NULL_GAIN, chan);
        radio_ctrl_impl::set_rx_antenna(EISCAT_DEFAULT_ANTENNA, chan);
        radio_ctrl_impl::set_rx_bandwidth(EISCAT_DEFAULT_BANDWIDTH, chan);
    }

    /**** Configure the digital gain controls *******************************/
    for (size_t i = 0; i < EISCAT_NUM_ANTENNAS; i++) {
        _tree->access<double>(get_arg_path("gain", i) / "value")
            .set_coercer([](double gain){ return std::max(-1.0, std::min(1.0, gain)); })
            .add_coerced_subscriber([this, i](double gain){ this->set_antenna_gain(i, gain); })
            .set(EISCAT_DEFAULT_NORM_GAIN)
        ;
    }

    /**** Set up legacy compatible properties ******************************/
    // For use with multi_usrp APIs etc.
    // For legacy prop tree init:
    fs_path fe_path = fs_path("dboards") / "A" / "rx_frontends";

    auto antenna_options = std::vector<std::string>{"BF"};
    for (size_t i = 0; i < EISCAT_NUM_ANTENNAS; i++) {
        antenna_options.push_back(str(boost::format("Rx%d") % i));
    }

    // The EISCAT dboards have 16 frontends total, but they map to 5 channels
    // each through a matrix of FIR filters and summations. UHD will get much
    // less confused if we create 5 fake frontends, because that's also the
    // number of channels. Since we have no control over the frontends,
    // nothing is lost here.
    for (size_t fe_idx = 0; fe_idx < _num_ports; fe_idx++) {
        _tree->create<std::string>(fe_path / fe_idx / "name")
            .set(str(boost::format("EISCAT Beam Contributions %d") % fe_idx))
        ;
        _tree->create<std::string>(fe_path / fe_idx / "connection")
            .set("I")
        ;
        _tree->create<std::string>(fe_path / fe_idx / "antenna" / "value")
            .add_coerced_subscriber(boost::bind(&eiscat_radio_ctrl_impl::set_rx_antenna, this, _1, 0))
            .set_publisher(boost::bind(&radio_ctrl_impl::get_rx_antenna, this, 0))
        ;
        _tree->create<std::vector<std::string>>(fe_path / fe_idx / "antenna" / "options")
            .set(antenna_options)
        ;
        _tree->create<double>(fe_path / fe_idx / "freq" / "value")
            .set(EISCAT_CENTER_FREQ)
            //.set_coercer(boost::bind(&eiscat_radio_ctrl_impl::set_rx_frequency, this, _1, 0))
            ////.set_publisher(boost::bind(&radio_ctrl_impl::get_rx_frequency, this, 0))
        ;
        _tree->create<meta_range_t>(fe_path / fe_idx / "freq" / "range")
            .set(meta_range_t(EISCAT_CENTER_FREQ, EISCAT_CENTER_FREQ))
        ;
        _tree->create<double>(fe_path / fe_idx / "gains" / "null" / "value")
            .set(EISCAT_DEFAULT_NULL_GAIN)
            //.set_coercer(boost::bind(&eiscat_radio_ctrl_impl::set_rx_gain, this, _1, 0))
            //.set_publisher(boost::bind(&radio_ctrl_impl::get_rx_gain, this, 0))
        ;
        _tree->create<meta_range_t>(fe_path / fe_idx / "gains" / "null" / "range")
            .set(meta_range_t(EISCAT_DEFAULT_NULL_GAIN, EISCAT_DEFAULT_NULL_GAIN))
        ;
        _tree->create<double>(fe_path / fe_idx / "bandwidth" / "value")
            .set(EISCAT_DEFAULT_BANDWIDTH)
            //.set_coercer(boost::bind(&eiscat_radio_ctrl_impl::set_rx_bandwidth, this, _1, 0))
            //.set_publisher(boost::bind(&radio_ctrl_impl::get_rx_bandwidth, this, 0))
        ;
        _tree->create<meta_range_t>(fe_path / fe_idx / "bandwidth" / "range")
            .set(meta_range_t(EISCAT_DEFAULT_BANDWIDTH, EISCAT_DEFAULT_BANDWIDTH))
        ;
        _tree->create<bool>(fe_path / fe_idx / "use_lo_offset")
            .set(false)
        ;
    }

    // We can actually stream data to an EISCAT board, so it needs some tx
    // frontends too:
    fe_path = fs_path("dboards") / "A" / "tx_frontends";
    for (size_t fe_idx = 0; fe_idx < _num_ports; fe_idx++) {
        _tree->create<std::string>(fe_path / fe_idx / "name")
            .set(str(boost::format("EISCAT Uplink %d") % fe_idx))
        ;
    }

    // There is only ever one EISCAT radio per dboard, so this should be unset
    // when we reach this line:
    UHD_ASSERT_THROW(not _tree->exists("tick_rate"));
    _tree->create<double>("tick_rate")
        //.set_coercer(boost::bind(&eiscat_radio_ctrl_impl::set_rate, this, _1))
        .set(EISCAT_TICK_RATE)
    ;

    if (not _tree->exists(fs_path("clock_source/value"))) {
        _tree->create<std::string>(fs_path("clock_source/value")).set("external");
    }

    UHD_VAR((_tree->exists(fs_path("time/cmd"))));
}

eiscat_radio_ctrl_impl::~eiscat_radio_ctrl_impl()
{
    UHD_LOG_TRACE("EISCAT", "eiscat_radio_ctrl_impl::dtor() ");
}


/****************************************************************************
 * Public API calls
 ***************************************************************************/
void eiscat_radio_ctrl_impl::set_tx_antenna(const std::string &, const size_t)
{
    throw uhd::runtime_error("Cannot set Tx antenna on EISCAT daughterboard");
}

void eiscat_radio_ctrl_impl::set_rx_antenna(const std::string &ant, const size_t port)
{
    UHD_ASSERT_THROW(port < EISCAT_NUM_PORTS);
    if (ant == "BF") {
        UHD_LOG_TRACE("EISCAT", "Setting antenna to 'BF' (which is a no-op)");
        return;
    }
    if (ant.size() < 3 or ant.size() > 4 or (ant.substr(0, 2) != "Rx" and ant.substr(0, 2) != "BF")) {
        throw uhd::value_error(str(
            boost::format("EISCAT: Invalid antenna selection: %s")
            % ant
        ));
    }

    bool use_fir_matrix = (ant.substr(0, 2) == "BF");

    const size_t antenna_idx = [&ant](){
        try {
            return boost::lexical_cast<size_t>(ant.substr(2));
        } catch (const boost::bad_lexical_cast&) {
            throw uhd::value_error(str(
                boost::format("EISCAT: Invalid antenna selection: %s")
                % ant
            ));
        }
    }();

    if (use_fir_matrix) {
        UHD_LOG_TRACE("EISCAT", str(
            boost::format("Setting port %d to only receive on antenna %d via FIR matrix")
            % port % antenna_idx
        ));
        // TODO: When we have a way to select neighbour contributions, we will need
        // to calculate the beam_index as a function of the port *and* if we're the
        // left or right USRP
        const size_t beam_index = port;
        uhd::time_spec_t send_now(0.0);

        for (size_t i = 0; i < EISCAT_NUM_ANTENNAS; i++) {
            if (i == antenna_idx) {
                select_filter(
                    beam_index,
                    i,
                    EISCAT_FIR_INDEX_IMPULSE,
                    send_now
                );
            } else {
                select_filter(
                    beam_index,
                    i,
                    EISCAT_FIR_INDEX_ZEROS,
                    send_now
                );
            }
        }
    } else {
        set_arg<int>("choose_beams", 6);
        UHD_LOG_TRACE("EISCAT", str(
            boost::format("Setting port %d to only receive on antenna %d directly")
            % port % antenna_idx
        ));
        sr_write(SR_ANTENNA_SELECT_BASE + port, antenna_idx);
    }
}

double eiscat_radio_ctrl_impl::get_tx_frequency(const size_t /* chan */)
{
    UHD_LOG_WARNING("EISCAT", "Ignoring attempt to read Tx frequency");
    return 0.0;
}

double eiscat_radio_ctrl_impl::set_tx_frequency(const double /* freq */, const size_t /* chan */)
{
    throw uhd::runtime_error("Cannot set Tx frequency on EISCAT daughterboard");
}

double eiscat_radio_ctrl_impl::set_rx_frequency(const double freq, const size_t chan)
{
    if (freq != get_rx_frequency(chan)) {
        UHD_LOG_WARNING("EISCAT", "Ignoring attempt to set Rx frequency");
    }
    return get_rx_frequency(chan);
}

double eiscat_radio_ctrl_impl::set_rx_bandwidth(const double bandwidth, const size_t chan)
{
    if (bandwidth != get_rx_bandwidth(chan)) {
        UHD_LOG_WARNING("EISCAT", "Ignoring attempt to set Rx bandwidth");
    }
    return get_rx_bandwidth(chan);
}


double eiscat_radio_ctrl_impl::set_tx_gain(const double /* gain */, const size_t /* chan */)
{
    throw uhd::runtime_error("Cannot set Tx gain on EISCAT daughterboard");
}

double eiscat_radio_ctrl_impl::set_rx_gain(const double gain, const size_t chan)
{
    // TODO: Add ability to set digital gain or make it explicit this function is not supported.
    if (gain != get_rx_gain(chan)) {
        UHD_LOG_WARNING("EISCAT", "Ignoring attempt to set Rx gain.");
    }
    return get_rx_gain(chan);
}

double eiscat_radio_ctrl_impl::set_rate(double rate)
{
    if (rate != get_rate()) {
        UHD_LOG_WARNING("EISCAT", "Attempting to set sampling rate to invalid value " << rate);
    }
    return get_rate();
}

size_t eiscat_radio_ctrl_impl::get_chan_from_dboard_fe(
        const std::string &fe,
        const uhd::direction_t /* dir */
) {
    return boost::lexical_cast<size_t>(fe);
}

std::string eiscat_radio_ctrl_impl::get_dboard_fe_from_chan(
        const size_t chan,
        const uhd::direction_t /* dir */
) {
    return std::to_string(chan);
}

double eiscat_radio_ctrl_impl::get_output_samp_rate(size_t /* port */)
{
    return EISCAT_RADIO_RATE;
}

bool eiscat_radio_ctrl_impl::check_radio_config()
{
    UHD_RFNOC_BLOCK_TRACE() << "x300_radio_ctrl_impl::check_radio_config() " ;
    const fs_path rx_fe_path = fs_path("dboards/A/rx_frontends");
    uint32_t chan_enables = 0;
    for (const auto &enb: _rx_streamer_active) {
        if (enb.second) {
            chan_enables |= (1<<enb.first);
        }
    }
    UHD_LOG_TRACE("EISCAT", str(boost::format("check_radio_config(): Setting channel enables to 0x%02X") % chan_enables));
    sr_write("SR_RX_STREAM_ENABLE", chan_enables);

    return true;
}

/****************************************************************************
 * Internal methods
 ***************************************************************************/
void eiscat_radio_ctrl_impl::write_fir_taps(
    const size_t fir_idx,
    const std::vector<eiscat_radio_ctrl_impl::fir_tap_t> &taps
) {
    if (taps.size() > EISCAT_NUM_FIR_TAPS) {
        throw uhd::value_error(str(
            boost::format("Too many FIR taps for EISCAT filters (%d)")
            % taps.size()
        ));
    }
    for (const auto &tap: taps) {
        if (tap > EISCAT_MAX_TAP_VALUE or tap < EISCAT_MIN_TAP_VALUE) {
            throw uhd::value_error(str(
                boost::format("Filter tap for filter_idx %d exceeds dynamic range (%d bits are allowed)")
                % fir_idx % EISCAT_BITS_PER_TAP
            ));
        }
    }

    UHD_LOG_TRACE("EISCAT", str(
        boost::format("Writing %d filter taps for filter index %d")
        % taps.size() % fir_idx
    ));
    for (size_t i = 0; i < EISCAT_NUM_FIR_TAPS; i++) {
        // Payload:
        // - bottom 14 bits address, fir_idx * 16 + tap_index
        // - top 18 bits are value
        uint32_t reg_value = (fir_idx * 16) + i;;
        if (taps.size() > i) {
            reg_value |= (taps[i] & 0x3FFFF) << 14;
        }
        sr_write("SR_FIR_BRAM_WRITE_TAPS", reg_value);
    }
}

void eiscat_radio_ctrl_impl::select_filter(
        const size_t beam_index,
        const size_t antenna_index,
        const size_t fir_index,
        const uhd::time_spec_t &time_spec
) {
    if (antenna_index >= EISCAT_NUM_ANTENNAS) {
        throw uhd::value_error(str(
            boost::format("Antenna index %d out of range. There are %d antennas in EISCAT.")
            % antenna_index % EISCAT_NUM_ANTENNAS
        ));
    }
    if (beam_index >= EISCAT_NUM_BEAMS) {
        throw uhd::value_error(str(
            boost::format("Beam index %d out of range. There are %d beam channels in EISCAT.")
            % beam_index % EISCAT_NUM_BEAMS
        ));
    }

    UHD_LOGGER_TRACE("EISCAT")
        << "Selecting filter " << fir_index
        << " for beam " << beam_index
        << " and antenna " << antenna_index
    ;
    bool send_now = (time_spec == uhd::time_spec_t(0.0));
    uint32_t reg_value = 0
        | (fir_index * 16)
        | (antenna_index & 0xF) << 14
        | (beam_index & 0xF) << 18
        | send_now << 22
    ;

    if (not send_now) {
        uint64_t cmd_time_ticks = time_spec.to_ticks(EISCAT_TICK_RATE);
        UHD_LOG_TRACE("EISCAT", str(
            boost::format("Filter selection will be applied at time %f (0x%016X == %u)")
            % time_spec.get_full_secs()
            % cmd_time_ticks
        ));
        sr_write("SR_FIR_COMMANDS_CTRL_TIME_LO", cmd_time_ticks & 0xFFFFFFFF);
        sr_write("SR_FIR_COMMANDS_CTRL_TIME_HI", (cmd_time_ticks >> 32) & 0xFFFFFFFF);
    }
    sr_write("SR_FIR_COMMANDS_RELOAD", reg_value);
}

void eiscat_radio_ctrl_impl::set_antenna_gain(
    const size_t antenna_idx,
    const double normalized_gain
) {
    if (normalized_gain < -1.0 or normalized_gain > 1.0) {
        throw uhd::value_error(str(
            boost::format("Invalid gain value for antenna %d: %f")
            % antenna_idx % normalized_gain
        ));
    }

    const auto fixpoint_gain = std::max<int32_t>(
        EISCAT_MIN_GAIN,
        std::min(
            EISCAT_MAX_GAIN,
            int32_t(normalized_gain * EISCAT_MAX_GAIN)
        )
    );

    UHD_LOG_TRACE("EISCAT", str(
        boost::format("Setting digital gain value for antenna %d to %f (%d)")
        % antenna_idx % normalized_gain % fixpoint_gain
    ));
    sr_write(SR_ANTENNA_GAIN_BASE + antenna_idx, fixpoint_gain);
}

/****************************************************************************
 * Registry
 ***************************************************************************/
UHD_RFNOC_BLOCK_REGISTER(eiscat_radio_ctrl, "EISCATRadio");
