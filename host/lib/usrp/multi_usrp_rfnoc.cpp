//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/exception.hpp>
#include <uhd/rfnoc/ddc_block_control.hpp>
#include <uhd/rfnoc/duc_block_control.hpp>
#include <uhd/rfnoc/filter_node.hpp>
#include <uhd/rfnoc/radio_control.hpp>
#include <uhd/rfnoc_graph.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhdlib/rfnoc/rfnoc_device.hpp>
#include <uhdlib/usrp/gpio_defs.hpp>
#include <unordered_set>
#include <boost/make_shared.hpp>
#include <boost/pointer_cast.hpp>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace uhd;
using namespace uhd::usrp;
using namespace uhd::rfnoc;

namespace {
constexpr char DEFAULT_CPU_FORMAT[] = "fc32";
constexpr char DEFAULT_OTW_FORMAT[] = "sc16";
constexpr double RX_SIGN            = +1.0;
constexpr double TX_SIGN            = -1.0;

/*! Make sure the stream args are valid and can be used by get_tx_stream()
 * and get_rx_stream().
 *
 */
stream_args_t sanitize_stream_args(const stream_args_t args_)
{
    stream_args_t args = args_;
    if (args.cpu_format.empty()) {
        UHD_LOG_DEBUG("MULTI_USRP",
            "get_xx_stream(): cpu_format not specified, defaulting to "
                << DEFAULT_CPU_FORMAT);
        args.cpu_format = DEFAULT_CPU_FORMAT;
    }
    if (args.otw_format.empty()) {
        UHD_LOG_DEBUG("MULTI_USRP",
            "get_xx_stream(): otw_format not specified, defaulting to "
                << DEFAULT_OTW_FORMAT);
        args.otw_format = DEFAULT_OTW_FORMAT;
    }
    if (args.channels.empty()) {
        UHD_LOG_DEBUG(
            "MULTI_USRP", "get_xx_stream(): channels not specified, defaulting to [0]");
        args.channels = {0};
    }
    return args;
}

std::string bytes_to_str(std::vector<uint8_t> str_b)
{
    return std::string(str_b.cbegin(), str_b.cend());
}

} // namespace

class multi_usrp_rfnoc : public multi_usrp
{
public:
    struct rx_chan_t
    {
        radio_control::sptr radio;
        ddc_block_control::sptr ddc; // can be nullptr
        size_t block_chan;
    };

    struct tx_chan_t
    {
        radio_control::sptr radio;
        duc_block_control::sptr duc; // can be nullptr
        size_t block_chan;
    };

    /**************************************************************************
     * Structors
     *************************************************************************/
    multi_usrp_rfnoc(rfnoc_graph::sptr graph, const device_addr_t& addr)
        : _args(addr), _graph(graph), _tree(_graph->get_tree())
    {
        // Discover all of the radios on our devices and create a mapping between radio
        // chains and channel numbers
        auto radio_blk_ids = _graph->find_blocks("Radio");
        // find_blocks doesn't sort, so we need to
        std::sort(radio_blk_ids.begin(),
            radio_blk_ids.end(),
            [](uhd::rfnoc::block_id_t i, uhd::rfnoc::block_id_t j) -> bool {
                if (i.get_device_no() != j.get_device_no()) {
                    return i.get_device_no() < j.get_device_no();
                } else {
                    return i.get_block_count() < j.get_block_count();
                }
            });

        // If we don't find any radios, we don't have a multi_usrp object
        if (radio_blk_ids.empty()) {
            throw uhd::runtime_error(
                "[multi_usrp] No radios found in connected devices.");
        }
        // Next, we assign block controllers to RX channels
        // Note that we don't want to connect blocks now; we will wait until we create and
        // connect a streamer. This gives us a little more time to figure out the desired
        // values of our properties (such as master clock)
        size_t musrp_rx_channel = 0;
        size_t musrp_tx_channel = 0;
        for (auto radio_id : radio_blk_ids) {
            auto radio_blk = _graph->get_block<uhd::rfnoc::radio_control>(radio_id);
            // We assume that the DDC connected to this radio block has the same mboard,
            // instance, and port number
            auto ddc_id =
                block_id_t(radio_id.get_device_no(), "DDC", radio_id.get_block_count());
            uhd::rfnoc::ddc_block_control::sptr ddc_blk;
            try {
                ddc_blk = _graph->get_block<uhd::rfnoc::ddc_block_control>(ddc_id);
            } catch (const uhd::exception&) {
                UHD_LOGGER_TRACE("MULTI_USRP")
                    << boost::format("No DDC found: %s") % ddc_id.to_string();
            }
            for (size_t block_chan = 0; block_chan < radio_blk->get_num_output_ports();
                 ++block_chan) {
                // Figure out if this channel has a DDC available
                auto this_chan_ddc =
                    ddc_blk
                            && _graph->is_connectable(
                                   radio_id, block_chan, ddc_id, block_chan)
                        ? ddc_blk
                        : nullptr;
                _rx_chans.emplace(
                    musrp_rx_channel, rx_chan_t({radio_blk, this_chan_ddc, block_chan}));
                if (!this_chan_ddc) {
                    UHD_LOGGER_DEBUG("MULTI_USRP")
                        << boost::format(
                               "Radio %s unable to connect to DDC %s on channel %d")
                               % radio_id.to_string() % ddc_id.to_string() % block_chan;
                } else {
                    UHD_LOG_DEBUG("MULTI_USRP",
                        "RX Channel " << musrp_rx_channel << " has "
                                      << radio_id.to_string() << " and DDC "
                                      << ddc_id.to_string());
                }
                ++musrp_rx_channel; // Increment after logging so we print the correct
                                    // value
            }
            // We assume that the DUC connected to this radio block has the same mboard,
            // instance, and port number
            auto duc_id =
                block_id_t(radio_id.get_device_no(), "DUC", radio_id.get_block_count());
            uhd::rfnoc::duc_block_control::sptr duc_blk;
            try {
                duc_blk = _graph->get_block<uhd::rfnoc::duc_block_control>(duc_id);
            } catch (const uhd::exception&) {
                UHD_LOGGER_TRACE("MULTI_USRP")
                    << boost::format("No DUC found: %s") % duc_id.to_string();
            }
            for (size_t block_chan = 0; block_chan < radio_blk->get_num_input_ports();
                 ++block_chan) {
                auto this_chan_duc =
                    duc_blk
                            && _graph->is_connectable(
                                   duc_id, block_chan, radio_id, block_chan)
                        ? duc_blk
                        : nullptr;
                _tx_chans.emplace(
                    musrp_tx_channel, tx_chan_t({radio_blk, this_chan_duc, block_chan}));
                if (!this_chan_duc) {
                    UHD_LOGGER_DEBUG("MULTI_USRP")
                        << boost::format(
                               "Radio %s unable to connect to DUC %s on channel %d")
                               % radio_id.to_string() % duc_id.to_string() % block_chan;
                } else {
                    UHD_LOG_DEBUG("MULTI_USRP",
                        "TX Channel " << musrp_tx_channel << " has "
                                      << radio_id.to_string() << " and DUC "
                                      << duc_id.to_string());
                }
                ++musrp_tx_channel; // Increment after logging so we print the correct
                                    // value
            }
        }
        _graph->commit();
    }

    ~multi_usrp_rfnoc()
    {
        // nop
    }

    // Direct device access makes no sense with RFNoC
    device::sptr get_device(void)
    {
        return nullptr;
    }

    rx_streamer::sptr get_rx_stream(const stream_args_t& args_)
    {
        std::lock_guard<std::recursive_mutex> l(_graph_mutex);
        stream_args_t args = sanitize_stream_args(args_);
        // Note that we don't release the graph, which means that property
        // propagation is possible. This is necessary so we don't disrupt
        // existing streamers. We use the _graph_mutex to try and avoid any
        // property propagation where possible.
        double rate = 1.0;
        // This will create an unconnected streamer
        auto rx_streamer = _graph->create_rx_streamer(args.channels.size(), args);
        for (size_t strm_port = 0; strm_port < args.channels.size(); ++strm_port) {
            auto rx_channel = args.channels.at(strm_port);
            auto rx_chain   = _get_rx_chan(rx_channel);
            if (rx_chain.ddc) {
                _graph->connect(rx_chain.radio->get_block_id(),
                    rx_chain.block_chan,
                    rx_chain.ddc->get_block_id(),
                    rx_chain.block_chan);
            }
            _graph->connect((rx_chain.ddc) ? rx_chain.ddc->get_block_id()
                                           : rx_chain.radio->get_block_id(),
                rx_chain.block_chan,
                rx_streamer,
                strm_port);
            const double chan_rate =
                _rx_rates.count(rx_channel) ? _rx_rates.at(rx_channel) : 1.0;
            if (chan_rate > 1.0 && rate != chan_rate) {
                UHD_LOG_DEBUG("MULTI_USRP",
                    "Inconsistent RX rates when creating streamer! Harmonizing "
                    "to "
                        << chan_rate);
                rate = chan_rate;
            }
        }
        // Now everything is connected, commit() again so we can have stream
        // commands go through the graph
        _graph->commit();

        // Before we return the streamer, we may need to reapply the rate. This
        // is necessary whenever the blocks were configured before the streamer
        // was created, because we don't know what state the graph is in after
        // commit() was called in that case..
        if (rate > 1.0) {
            UHD_LOG_TRACE("MULTI_USRP",
                "Now reapplying RX rate " << (rate / 1e6)
                                          << " MHz to all streamer channels");
            for (auto rx_channel : args.channels) {
                auto rx_chain = _get_rx_chan(rx_channel);
                if (rx_chain.ddc) {
                    rx_chain.ddc->set_output_rate(rate, rx_chain.block_chan);
                } else {
                    rx_chain.radio->set_rate(rate);
                }
            }
        }
        return rx_streamer;
    }

    tx_streamer::sptr get_tx_stream(const stream_args_t& args_)
    {
        std::lock_guard<std::recursive_mutex> l(_graph_mutex);
        stream_args_t args = sanitize_stream_args(args_);
        // Note that we don't release the graph, which means that property
        // propagation is possible. This is necessary so we don't disrupt
        // existing streamers. We use the _graph_mutex to try and avoid any
        // property propagation where possible.
        double rate = 1.0;
        // This will create an unconnected streamer
        auto tx_streamer = _graph->create_tx_streamer(args.channels.size(), args);
        for (size_t strm_port = 0; strm_port < args.channels.size(); ++strm_port) {
            auto tx_channel = args.channels.at(strm_port);
            auto tx_chain   = _get_tx_chan(tx_channel);
            if (tx_chain.duc) {
                _graph->connect(tx_chain.duc->get_block_id(),
                    tx_chain.block_chan,
                    tx_chain.radio->get_block_id(),
                    tx_chain.block_chan);
            }
            _graph->connect(tx_streamer,
                strm_port,
                (tx_chain.duc) ? tx_chain.duc->get_block_id()
                               : tx_chain.radio->get_block_id(),
                tx_chain.block_chan);
            const double chan_rate =
                _tx_rates.count(tx_channel) ? _tx_rates.at(tx_channel) : 1.0;
            if (chan_rate > 1.0 && rate != chan_rate) {
                UHD_LOG_DEBUG("MULTI_USRP",
                    "Inconsistent TX rates when creating streamer! Harmonizing "
                    "to "
                        << chan_rate);
                rate = chan_rate;
            }
        }
        // Now everything is connected, commit() again so we can have stream
        // commands go through the graph
        _graph->commit();

        // Before we return the streamer, we may need to reapply the rate. This
        // is necessary whenever the blocks were configured before the streamer
        // was created, because we don't know what state the graph is in after
        // commit() was called in that case, or we could have configured blocks
        // to run at different rates (see the warning above).
        if (rate > 1.0) {
            UHD_LOG_TRACE("MULTI_USRP",
                "Now reapplying TX rate " << (rate / 1e6)
                                          << " MHz to all streamer channels");
            for (auto tx_channel : args.channels) {
                auto tx_chain = _get_tx_chan(tx_channel);
                if (tx_chain.duc) {
                    tx_chain.duc->set_input_rate(rate, tx_chain.block_chan);
                } else {
                    tx_chain.radio->set_rate(rate);
                }
            }
        }
        return tx_streamer;
    }


    /***********************************************************************
     * Helper methods
     **********************************************************************/
    /*! The CORDIC can be used to shift the baseband below / past the tunable
     * limits of the actual RF front-end. The baseband filter, located on the
     * daughterboard, however, limits the useful instantaneous bandwidth. We
     * allow the user to tune to the edge of the filter, where the roll-off
     * begins.  This prevents the user from tuning past the point where less
     * than half of the spectrum would be useful.
     */
    static meta_range_t make_overall_tune_range(
        const meta_range_t& fe_range, const meta_range_t& dsp_range, const double bw)
    {
        meta_range_t range;
        for (const range_t& sub_range : fe_range) {
            range.push_back(
                range_t(sub_range.start() + std::max(dsp_range.start(), -bw / 2),
                    sub_range.stop() + std::min(dsp_range.stop(), bw / 2),
                    dsp_range.step()));
        }
        return range;
    }

    dict<std::string, std::string> get_usrp_rx_info(size_t chan)
    {
        auto& rx_chain      = _get_rx_chan(chan);
        const size_t mb_idx = rx_chain.radio->get_block_id().get_device_no();
        auto mbc            = get_mbc(mb_idx);
        auto mb_eeprom      = mbc->get_eeprom();

        dict<std::string, std::string> usrp_info;
        usrp_info["mboard_id"]      = mbc->get_mboard_name();
        usrp_info["mboard_name"]    = mb_eeprom.get("name", "n/a");
        usrp_info["mboard_serial"]  = mb_eeprom.get("serial", "n/a");
        usrp_info["rx_subdev_name"] = get_rx_subdev_name(chan);
        usrp_info["rx_subdev_spec"] = get_rx_subdev_spec(mb_idx).to_string();
        usrp_info["rx_antenna"]     = get_rx_antenna(chan);

        const auto db_eeprom = rx_chain.radio->get_db_eeprom();
        usrp_info["rx_serial"] =
            db_eeprom.count("rx_serial") ? bytes_to_str(db_eeprom.at("rx_serial")) : "";
        usrp_info["rx_id"] =
            db_eeprom.count("rx_id") ? bytes_to_str(db_eeprom.at("rx_id")) : "";

        return usrp_info;
    }

    dict<std::string, std::string> get_usrp_tx_info(size_t chan)
    {
        auto& tx_chain      = _get_tx_chan(chan);
        const size_t mb_idx = tx_chain.radio->get_block_id().get_device_no();
        auto mbc            = get_mbc(mb_idx);
        auto mb_eeprom      = mbc->get_eeprom();

        dict<std::string, std::string> usrp_info;
        usrp_info["mboard_id"]      = mbc->get_mboard_name();
        usrp_info["mboard_name"]    = mb_eeprom.get("name", "n/a");
        usrp_info["mboard_serial"]  = mb_eeprom.get("serial", "n/a");
        usrp_info["tx_subdev_name"] = get_tx_subdev_name(chan);
        usrp_info["tx_subdev_spec"] = get_tx_subdev_spec(mb_idx).to_string();
        usrp_info["tx_antenna"]     = get_tx_antenna(chan);

        const auto db_eeprom = tx_chain.radio->get_db_eeprom();
        usrp_info["tx_serial"] =
            db_eeprom.count("tx_serial") ? bytes_to_str(db_eeprom.at("tx_serial")) : "";
        usrp_info["tx_id"] =
            db_eeprom.count("tx_id") ? bytes_to_str(db_eeprom.at("tx_id")) : "";

        return usrp_info;
    }

    /*! Tune the appropriate radio chain to the requested frequency.
     *  The general algorithm is the same for RX and TX, so we can pass in lambdas to do
     * the setting/getting for us.
     */
    tune_result_t tune_xx_subdev_and_dsp(const double xx_sign,
        freq_range_t tune_range,
        freq_range_t rf_freq_range,
        freq_range_t dsp_freq_range,
        std::function<void(double)> set_rf_freq,
        std::function<double()> get_rf_freq,
        std::function<void(double)> set_dsp_freq,
        std::function<double()> get_dsp_freq,
        const tune_request_t& tune_request)
    {
        double clipped_requested_freq = tune_range.clip(tune_request.target_freq);
        UHD_LOGGER_TRACE("MULTI_USRP")
            << boost::format("Frequency Range %.3fMHz->%.3fMHz")
                   % (tune_range.start() / 1e6) % (tune_range.stop() / 1e6);
        UHD_LOGGER_TRACE("MULTI_USRP")
            << "Clipped RX frequency requested: "
                   + std::to_string(clipped_requested_freq / 1e6) + "MHz";

        //------------------------------------------------------------------
        //-- set the RF frequency depending upon the policy
        //------------------------------------------------------------------
        double target_rf_freq = 0.0;
        switch (tune_request.rf_freq_policy) {
            case tune_request_t::POLICY_AUTO:
                target_rf_freq = clipped_requested_freq;
                break;

            case tune_request_t::POLICY_MANUAL:
                target_rf_freq = rf_freq_range.clip(tune_request.rf_freq);
                break;

            case tune_request_t::POLICY_NONE:
                break; // does not set
        }
        UHD_LOGGER_TRACE("MULTI_USRP")
            << "Target RF Freq: " + std::to_string(target_rf_freq / 1e6) + "MHz";

        //------------------------------------------------------------------
        //-- Tune the RF frontend
        //------------------------------------------------------------------
        if (tune_request.rf_freq_policy != tune_request_t::POLICY_NONE) {
            set_rf_freq(target_rf_freq);
        }
        const double actual_rf_freq = get_rf_freq();

        //------------------------------------------------------------------
        //-- Set the DSP frequency depending upon the DSP frequency policy.
        //------------------------------------------------------------------
        double target_dsp_freq = 0.0;
        switch (tune_request.dsp_freq_policy) {
            case tune_request_t::POLICY_AUTO:
                /* If we are using the AUTO tuning policy, then we prevent the
                 * CORDIC from spinning us outside of the range of the baseband
                 * filter, regardless of what the user requested. This could happen
                 * if the user requested a center frequency so far outside of the
                 * tunable range of the FE that the CORDIC would spin outside the
                 * filtered baseband. */
                target_dsp_freq = actual_rf_freq - clipped_requested_freq;

                // invert the sign on the dsp freq for transmit (spinning up vs down)
                target_dsp_freq *= xx_sign;

                break;

            case tune_request_t::POLICY_MANUAL:
                /* If the user has specified a manual tune policy, we will allow
                 * tuning outside of the baseband filter, but will still clip the
                 * target DSP frequency to within the bounds of the CORDIC to
                 * prevent undefined behavior (likely an overflow). */
                target_dsp_freq = dsp_freq_range.clip(tune_request.dsp_freq);
                break;

            case tune_request_t::POLICY_NONE:
                break; // does not set
        }
        UHD_LOGGER_TRACE("MULTI_USRP")
            << "Target DSP Freq: " + std::to_string(target_dsp_freq / 1e6) + "MHz";

        //------------------------------------------------------------------
        //-- Tune the DSP
        //------------------------------------------------------------------
        if (tune_request.dsp_freq_policy != tune_request_t::POLICY_NONE) {
            set_dsp_freq(target_dsp_freq);
        }
        const double actual_dsp_freq = get_dsp_freq();

        //------------------------------------------------------------------
        //-- Load and return the tune result
        //------------------------------------------------------------------
        tune_result_t tune_result;
        tune_result.clipped_rf_freq = clipped_requested_freq;
        tune_result.target_rf_freq  = target_rf_freq;
        tune_result.actual_rf_freq  = actual_rf_freq;
        tune_result.target_dsp_freq = target_dsp_freq;
        tune_result.actual_dsp_freq = actual_dsp_freq;
        return tune_result;
    }

    /*******************************************************************
     * Mboard methods
     ******************************************************************/
    void set_master_clock_rate(double rate, size_t mboard = ALL_MBOARDS)
    {
        for (auto& chain : _rx_chans) {
            auto radio = chain.second.radio;
            if (radio->get_block_id().get_device_no() == mboard
                || mboard == ALL_MBOARDS) {
                radio->set_rate(rate);
            }
        }
        for (auto& chain : _tx_chans) {
            auto radio = chain.second.radio;
            if (radio->get_block_id().get_device_no() == mboard
                || mboard == ALL_MBOARDS) {
                radio->set_rate(rate);
            }
        }
    }

    double get_master_clock_rate(size_t mboard)
    {
        // We pick the first radio we can find on this mboard, and hope that all
        // radios have the same range.
        for (auto& chain : _rx_chans) {
            auto radio = chain.second.radio;
            if (radio->get_block_id().get_device_no() == mboard) {
                return radio->get_rate();
            }
        }
        for (auto& chain : _tx_chans) {
            auto radio = chain.second.radio;
            if (radio->get_block_id().get_device_no() == mboard) {
                return radio->get_rate();
            }
        }
        throw uhd::key_error("Invalid mboard index!");
    }

    meta_range_t get_master_clock_rate_range(const size_t mboard = 0)
    {
        // We pick the first radio we can find on this mboard, and hope that all
        // radios have the same range.
        for (auto& chain : _rx_chans) {
            auto radio = chain.second.radio;
            if (radio->get_block_id().get_device_no() == mboard) {
                return radio->get_rate_range();
            }
        }
        for (auto& chain : _tx_chans) {
            auto radio = chain.second.radio;
            if (radio->get_block_id().get_device_no() == mboard) {
                return radio->get_rate_range();
            }
        }
        throw uhd::key_error("Invalid mboard index!");
    }

    std::string get_pp_string(void)
    {
        std::string buff = str(boost::format("%s USRP:\n"
                                             "  Device: %s\n")
                               % ((get_num_mboards() > 1) ? "Multi" : "Single")
                               % (_tree->access<std::string>("/name").get()));
        for (size_t m = 0; m < get_num_mboards(); m++) {
            buff += str(
                boost::format("  Mboard %d: %s\n") % m % get_mbc(m)->get_mboard_name());
        }


        //----------- rx side of life ----------------------------------
        for (size_t rx_chan = 0; rx_chan < get_rx_num_channels(); rx_chan++) {
            buff += str(boost::format("  RX Channel: %u\n"
                                      "    RX DSP: %s\n"
                                      "    RX Dboard: %s\n"
                                      "    RX Subdev: %s\n")
                        % rx_chan
                        % (_rx_chans.at(rx_chan).ddc ? std::to_string(rx_chan) : "n/a")
                        % _rx_chans.at(rx_chan).radio->get_slot_name()
                        % get_rx_subdev_name(rx_chan));
        }

        //----------- tx side of life ----------------------------------
        for (size_t tx_chan = 0; tx_chan < get_tx_num_channels(); tx_chan++) {
            buff += str(boost::format("  TX Channel: %u\n"
                                      "    TX DSP: %s\n"
                                      "    TX Dboard: %s\n"
                                      "    TX Subdev: %s\n")
                        % tx_chan
                        % (_tx_chans.at(tx_chan).duc ? std::to_string(tx_chan) : "n/a")
                        % _tx_chans.at(tx_chan).radio->get_slot_name()
                        % get_tx_subdev_name(tx_chan));
        }

        return buff;
    }

    std::string get_mboard_name(size_t mboard = 0)
    {
        return get_mbc(mboard)->get_mboard_name();
    }

    time_spec_t get_time_now(size_t mboard = 0)
    {
        return get_mbc(mboard)->get_timekeeper(0)->get_time_now();
    }

    time_spec_t get_time_last_pps(size_t mboard = 0)
    {
        return get_mbc(mboard)->get_timekeeper(0)->get_time_last_pps();
    }

    void set_time_now(const time_spec_t& time_spec, size_t mboard = ALL_MBOARDS)
    {
        if (mboard == ALL_MBOARDS) {
            for (size_t i = 0; i < get_num_mboards(); ++i) {
                set_time_now(time_spec, i);
            }
            return;
        }
        get_mbc(mboard)->get_timekeeper(0)->set_time_now(time_spec);
    }

    void set_time_next_pps(const time_spec_t& time_spec, size_t mboard = ALL_MBOARDS)
    {
        if (mboard == ALL_MBOARDS) {
            for (size_t i = 0; i < get_num_mboards(); ++i) {
                set_time_next_pps(time_spec, i);
            }
            return;
        }
        get_mbc(mboard)->get_timekeeper(0)->set_time_next_pps(time_spec);
    }

    void set_time_unknown_pps(const time_spec_t& time_spec)
    {
        UHD_LOGGER_INFO("MULTI_USRP") << "    1) catch time transition at pps edge";
        auto end_time                   = std::chrono::steady_clock::now() + 1100ms;
        time_spec_t time_start_last_pps = get_time_last_pps();
        while (time_start_last_pps == get_time_last_pps()) {
            if (std::chrono::steady_clock::now() > end_time) {
                throw uhd::runtime_error("Board 0 may not be getting a PPS signal!\n"
                                         "No PPS detected within the time interval.\n"
                                         "See the application notes for your device.\n");
            }
            std::this_thread::sleep_for(1ms);
        }

        UHD_LOGGER_INFO("MULTI_USRP") << "    2) set times next pps (synchronously)";
        set_time_next_pps(time_spec, ALL_MBOARDS);
        std::this_thread::sleep_for(1s);

        // verify that the time registers are read to be within a few RTT
        for (size_t m = 1; m < get_num_mboards(); m++) {
            time_spec_t time_0 = this->get_time_now(0);
            time_spec_t time_i = this->get_time_now(m);
            // 10 ms: greater than RTT but not too big
            if (time_i < time_0 or (time_i - time_0) > time_spec_t(0.01)) {
                UHD_LOGGER_WARNING("MULTI_USRP")
                    << boost::format(
                           "Detected time deviation between board %d and board 0.\n"
                           "Board 0 time is %f seconds.\n"
                           "Board %d time is %f seconds.\n")
                           % m % time_0.get_real_secs() % m % time_i.get_real_secs();
            }
        }
    }

    bool get_time_synchronized(void)
    {
        for (size_t m = 1; m < get_num_mboards(); m++) {
            time_spec_t time_0 = this->get_time_now(0);
            time_spec_t time_i = this->get_time_now(m);
            if (time_i < time_0 or (time_i - time_0) > time_spec_t(0.01)) {
                return false;
            }
        }
        return true;
    }

    void set_command_time(const uhd::time_spec_t& time_spec, size_t mboard = ALL_MBOARDS)
    {
        if (mboard == ALL_MBOARDS) {
            for (size_t i = 0; i < get_num_mboards(); ++i) {
                set_command_time(time_spec, i);
            }
            return;
        }

        // Set command time on all the blocks that are connected
        for (auto& chain : _rx_chans) {
            chain.second.radio->set_command_time(time_spec, chain.second.block_chan);
            if (chain.second.ddc) {
                chain.second.ddc->set_command_time(time_spec, chain.second.block_chan);
            }
        }
        for (auto& chain : _tx_chans) {
            chain.second.radio->set_command_time(time_spec, chain.second.block_chan);
            if (chain.second.duc) {
                chain.second.duc->set_command_time(time_spec, chain.second.block_chan);
            }
        }
    }

    void clear_command_time(size_t mboard = ALL_MBOARDS)
    {
        if (mboard == ALL_MBOARDS) {
            for (size_t i = 0; i < get_num_mboards(); ++i) {
                clear_command_time(i);
            }
            return;
        }

        // Set command time on all the blocks that are connected
        for (auto& chain : _rx_chans) {
            chain.second.radio->clear_command_time(chain.second.block_chan);
            if (chain.second.ddc) {
                chain.second.ddc->clear_command_time(chain.second.block_chan);
            }
        }
        for (auto& chain : _tx_chans) {
            chain.second.radio->clear_command_time(chain.second.block_chan);
            if (chain.second.duc) {
                chain.second.duc->clear_command_time(chain.second.block_chan);
            }
        }
    }

    void issue_stream_cmd(const stream_cmd_t& stream_cmd, size_t chan = ALL_CHANS)
    {
        if (chan != ALL_CHANS) {
            auto& rx_chain = _get_rx_chan(chan);
            if (rx_chain.ddc) {
                rx_chain.ddc->issue_stream_cmd(stream_cmd, rx_chain.block_chan);
            } else {
                rx_chain.radio->issue_stream_cmd(stream_cmd, rx_chain.block_chan);
            }
            return;
        }
        for (size_t c = 0; c < get_rx_num_channels(); c++) {
            issue_stream_cmd(stream_cmd, c);
        }
    }

    void set_time_source(const std::string& source, const size_t mboard = ALL_MBOARDS)
    {
        if (mboard == ALL_MBOARDS) {
            for (size_t i = 0; i < get_num_mboards(); ++i) {
                set_time_source(source, i);
            }
            return;
        }
        get_mbc(mboard)->set_time_source(source);
    }

    std::string get_time_source(const size_t mboard)
    {
        return get_mbc(mboard)->get_time_source();
    }

    std::vector<std::string> get_time_sources(const size_t mboard)
    {
        return get_mbc(mboard)->get_time_sources();
    }

    void set_clock_source(const std::string& source, const size_t mboard = ALL_MBOARDS)
    {
        if (mboard == ALL_MBOARDS) {
            for (size_t i = 0; i < get_num_mboards(); ++i) {
                set_clock_source(source, i);
            }
            return;
        }
        get_mbc(mboard)->set_clock_source(source);
    }

    std::string get_clock_source(const size_t mboard)
    {
        return get_mbc(mboard)->get_clock_source();
    }

    std::vector<std::string> get_clock_sources(const size_t mboard)
    {
        return get_mbc(mboard)->get_clock_sources();
    }

    void set_sync_source(const std::string& clock_source,
        const std::string& time_source,
        const size_t mboard = ALL_MBOARDS)
    {
        if (mboard == ALL_MBOARDS) {
            for (size_t i = 0; i < get_num_mboards(); ++i) {
                set_sync_source(clock_source, time_source, i);
            }
            return;
        }
        get_mbc(mboard)->set_sync_source(clock_source, time_source);
    }

    void set_sync_source(
        const device_addr_t& sync_source, const size_t mboard = ALL_MBOARDS)
    {
        if (mboard == ALL_MBOARDS) {
            for (size_t i = 0; i < get_num_mboards(); ++i) {
                set_sync_source(sync_source, i);
            }
            return;
        }
        get_mbc(mboard)->set_sync_source(sync_source);
    }

    device_addr_t get_sync_source(const size_t mboard)
    {
        return get_mbc(mboard)->get_sync_source();
    }

    std::vector<device_addr_t> get_sync_sources(const size_t mboard)
    {
        return get_mbc(mboard)->get_sync_sources();
    }

    void set_clock_source_out(const bool enb, const size_t mboard = ALL_MBOARDS)
    {
        get_mbc(mboard)->set_clock_source_out(enb);
    }

    void set_time_source_out(const bool enb, const size_t mboard = ALL_MBOARDS)
    {
        get_mbc(mboard)->set_time_source_out(enb);
    }

    size_t get_num_mboards(void)
    {
        return _graph->get_num_mboards();
    }

    sensor_value_t get_mboard_sensor(const std::string& name, size_t mboard = 0)
    {
        return get_mbc(mboard)->get_sensor(name);
    }

    std::vector<std::string> get_mboard_sensor_names(size_t mboard = 0)
    {
        return get_mbc(mboard)->get_sensor_names();
    }

    // This only works on the USRP2 and B100, both of which are not rfnoc_device
    void set_user_register(const uint8_t, const uint32_t, size_t)
    {
        throw uhd::not_implemented_error(
            "set_user_register(): Not implemented on this device!");
    }

    // This only works on the B200, which is not an rfnoc_device
    uhd::wb_iface::sptr get_user_settings_iface(const size_t)
    {
        return nullptr;
    }

    /*******************************************************************
     * RX methods
     ******************************************************************/
    rx_chan_t _generate_rx_radio_chan(block_id_t radio_id, size_t block_chan)
    {
        auto radio_blk = _graph->get_block<uhd::rfnoc::radio_control>(radio_id);
        // We assume that the DDC connected to this radio block has the same mboard,
        // instance, and port number
        auto ddc_id =
            block_id_t(radio_id.get_device_no(), "DDC", radio_id.get_block_count());
        uhd::rfnoc::ddc_block_control::sptr ddc_blk;
        try {
            ddc_blk = _graph->get_block<uhd::rfnoc::ddc_block_control>(ddc_id);
        } catch (const uhd::lookup_error&) {
            UHD_LOGGER_TRACE("MULTI_USRP") << "No DDC found: " << ddc_id.to_string();
        }
        // Figure out if this channel has a DDC available
        auto this_chan_ddc =
            ddc_blk && _graph->is_connectable(radio_id, block_chan, ddc_id, block_chan)
                ? ddc_blk
                : nullptr;
        return {radio_blk, this_chan_ddc, block_chan};
    }

    std::vector<rx_chan_t> _generate_mboard_rx_chans(
        const uhd::usrp::subdev_spec_t& spec, size_t mboard)
    {
        // Discover all of the radios on our devices and create a mapping between radio
        // chains and channel numbers
        auto radio_blk_ids = _graph->find_blocks(std::to_string(mboard) + "/Radio");
        // find_blocks doesn't sort, so we need to
        std::sort(radio_blk_ids.begin(),
            radio_blk_ids.end(),
            [](uhd::rfnoc::block_id_t i, uhd::rfnoc::block_id_t j) -> bool {
                if (i.get_device_no() != j.get_device_no()) {
                    return i.get_device_no() < j.get_device_no();
                } else {
                    return i.get_block_count() < j.get_block_count();
                }
            });

        // If we don't find any radios, we don't have a multi_usrp object
        if (radio_blk_ids.empty()) {
            throw uhd::runtime_error(
                "[multi_usrp] No radios found in the requested mboard: "
                + std::to_string(mboard));
        }

        // Iterate through the subdev pairs, and try to find a radio that matches
        std::vector<rx_chan_t> new_chans;
        for (auto chan_subdev_pair : spec) {
            bool subdev_found = false;
            for (auto radio_id : radio_blk_ids) {
                auto radio_blk  = _graph->get_block<uhd::rfnoc::radio_control>(radio_id);
                size_t block_chan;
                try {
                    block_chan = radio_blk->get_chan_from_dboard_fe(
                        chan_subdev_pair.sd_name, RX_DIRECTION);
                } catch (const uhd::lookup_error&) {
                    // This is OK, since we're probing all radios, this
                    // particular radio may not have the requested frontend name
                    // so it's not one that we want in this list.
                    continue;
                }
                subdev_spec_pair_t radio_subdev(radio_blk->get_slot_name(),
                    radio_blk->get_dboard_fe_from_chan(block_chan, uhd::RX_DIRECTION));
                if (chan_subdev_pair == radio_subdev) {
                    new_chans.push_back(_generate_rx_radio_chan(radio_id, block_chan));
                    subdev_found = true;
                }
            }
            if (!subdev_found) {
                std::string err_msg("Could not find radio on mboard "
                                    + std::to_string(mboard) + " that matches subdev "
                                    + chan_subdev_pair.db_name + ":"
                                    + chan_subdev_pair.sd_name);
                UHD_LOG_ERROR("MULTI_USRP", err_msg);
                throw uhd::lookup_error(err_msg);
            }
        }
        UHD_LOG_TRACE("MULTI_USRP",
            std::string("Using RX subdev " + spec.to_string() + ", found ")
                + std::to_string(new_chans.size()) + " channels for mboard "
                + std::to_string(mboard));
        return new_chans;
    }

    void set_rx_subdev_spec(
        const uhd::usrp::subdev_spec_t& spec, size_t mboard = ALL_MBOARDS)
    {
        // First, generate a vector of the RX channels that we need to register
        auto new_rx_chans = [this, spec, mboard]() {
            /* When setting the subdev spec in multiple mboard scenarios, there are two
             * cases we need to handle:
             * 1. Setting all mboard to the same subdev spec. This is the easy case.
             * 2. Setting a single mboard's subdev spec. In this case, we need to update
             * the requested mboard's subdev spec, and keep the old subdev spec for the
             * other mboards.
             */
            std::vector<rx_chan_t> new_rx_chans;
            for (size_t current_mboard = 0; current_mboard < get_num_mboards();
                 ++current_mboard) {
                auto current_spec = [this, spec, mboard, current_mboard]() {
                    if (mboard == ALL_MBOARDS || mboard == current_mboard) {
                        // Update all mboards to the same subdev spec OR
                        // only update this mboard to the new subdev spec
                        return spec;
                    } else {
                        // Keep the old subdev spec for this mboard
                        return get_rx_subdev_spec(current_mboard);
                    }
                }();
                auto new_mboard_chans =
                    _generate_mboard_rx_chans(current_spec, current_mboard);
                new_rx_chans.insert(
                    new_rx_chans.end(), new_mboard_chans.begin(), new_mboard_chans.end());
            }
            return new_rx_chans;
        }();

        // Now register them
        _rx_chans.clear();
        for (size_t rx_chan = 0; rx_chan < new_rx_chans.size(); ++rx_chan) {
            _rx_chans.emplace(rx_chan, new_rx_chans.at(rx_chan));
        }
    }

    uhd::usrp::subdev_spec_t get_rx_subdev_spec(size_t mboard)
    {
        uhd::usrp::subdev_spec_t result;
        for (size_t rx_chan = 0; rx_chan < get_rx_num_channels(); rx_chan++) {
            auto& rx_chain = _rx_chans.at(rx_chan);
            if (rx_chain.radio->get_block_id().get_device_no() == mboard) {
                result.push_back(
                    uhd::usrp::subdev_spec_pair_t(rx_chain.radio->get_slot_name(),
                        rx_chain.radio->get_dboard_fe_from_chan(
                            rx_chain.block_chan, uhd::RX_DIRECTION)));
            }
        }

        return result;
    }

    size_t get_rx_num_channels(void)
    {
        return _rx_chans.size();
    }

    std::string get_rx_subdev_name(size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_fe_name(rx_chain.block_chan, uhd::RX_DIRECTION);
    }

    void set_rx_rate(double rate, size_t chan = ALL_CHANS)
    {
        std::lock_guard<std::recursive_mutex> l(_graph_mutex);
        if (chan == ALL_CHANS) {
            for (size_t chan = 0; chan < _rx_chans.size(); ++chan) {
                set_rx_rate(rate, chan);
            }
            return;
        }
        const double actual_rate = [&]() {
            auto rx_chain = _get_rx_chan(chan);
            if (rx_chain.ddc) {
                return rx_chain.ddc->set_output_rate(rate, rx_chain.block_chan);
            } else {
                return rx_chain.radio->set_rate(rate);
            }
        }();
        if (actual_rate != rate) {
            UHD_LOGGER_WARNING("MULTI_USRP")
                << boost::format(
                       "Could not set RX rate to %.3f MHz. Actual rate is %.3f MHz")
                       % (rate / 1.0e6) % (actual_rate / 1.0e6);
        }
        _rx_rates[chan] = actual_rate;
    }

    double get_rx_rate(size_t chan)
    {
        std::lock_guard<std::recursive_mutex> l(_graph_mutex);
        auto& rx_chain = _get_rx_chan(chan);
        if (rx_chain.ddc) {
            return rx_chain.ddc->get_output_rate(rx_chain.block_chan);
        }
        return rx_chain.radio->get_rate();
    }

    meta_range_t get_rx_rates(size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return (rx_chain.ddc)
                   ? make_overall_tune_range(
                         rx_chain.radio->get_rx_frequency_range(rx_chain.block_chan),
                         rx_chain.ddc->get_frequency_range(rx_chain.block_chan),
                         rx_chain.radio->get_rx_bandwidth(rx_chain.block_chan))
                   : rx_chain.radio->get_rx_frequency_range(rx_chain.block_chan);
    }

    tune_result_t set_rx_freq(const tune_request_t& tune_request, size_t chan)
    {
        std::lock_guard<std::recursive_mutex> l(_graph_mutex);
        // TODO: Add external LO warning

        auto rx_chain = _get_rx_chan(chan);

        rx_chain.radio->set_rx_tune_args(tune_request.args, rx_chain.block_chan);
        //------------------------------------------------------------------
        //-- calculate the tunable frequency ranges of the system
        //------------------------------------------------------------------
        freq_range_t tune_range =
            (rx_chain.ddc)
                ? make_overall_tune_range(
                      rx_chain.radio->get_rx_frequency_range(rx_chain.block_chan),
                      rx_chain.ddc->get_frequency_range(rx_chain.block_chan),
                      rx_chain.radio->get_rx_bandwidth(rx_chain.block_chan))
                : rx_chain.radio->get_rx_frequency_range(rx_chain.block_chan);

        freq_range_t rf_range =
            rx_chain.radio->get_rx_frequency_range(rx_chain.block_chan);
        freq_range_t dsp_range =
            (rx_chain.ddc) ? rx_chain.ddc->get_frequency_range(rx_chain.block_chan)
                           : meta_range_t(0, 0);
        // Create lambdas to feed to tune_xx_subdev_and_dsp()
        // Note: If there is no DDC present, register empty lambdas for the DSP functions
        auto set_rf_freq = [rx_chain](double freq) {
            rx_chain.radio->set_rx_frequency(freq, rx_chain.block_chan);
        };
        auto get_rf_freq = [rx_chain](void) {
            return rx_chain.radio->get_rx_frequency(rx_chain.block_chan);
        };
        auto set_dsp_freq = [rx_chain](double freq) {
            (rx_chain.ddc) ? rx_chain.ddc->set_freq(freq, rx_chain.block_chan) : 0;
        };
        auto get_dsp_freq = [rx_chain](void) {
            return (rx_chain.ddc) ? rx_chain.ddc->get_freq(rx_chain.block_chan) : 0.0;
        };
        return tune_xx_subdev_and_dsp(RX_SIGN,
            tune_range,
            rf_range,
            dsp_range,
            set_rf_freq,
            get_rf_freq,
            set_dsp_freq,
            get_dsp_freq,
            tune_request);
    }

    double get_rx_freq(size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);

        // extract actual dsp and IF frequencies
        const double actual_rf_freq =
            rx_chain.radio->get_rx_frequency(rx_chain.block_chan);
        const double actual_dsp_freq =
            (rx_chain.ddc) ? rx_chain.ddc->get_freq(rx_chain.block_chan) : 0.0;

        // invert the sign on the dsp freq for transmit
        return actual_rf_freq - actual_dsp_freq * RX_SIGN;
    }

    freq_range_t get_rx_freq_range(size_t chan)
    {
        auto fe_freq_range = get_fe_rx_freq_range(chan);

        auto rx_chain = _get_rx_chan(chan);
        uhd::freq_range_t dsp_freq_range =
            (rx_chain.ddc) ? make_overall_tune_range(get_fe_rx_freq_range(chan),
                                 rx_chain.ddc->get_frequency_range(rx_chain.block_chan),
                                 rx_chain.radio->get_rx_bandwidth(rx_chain.block_chan))
                           : get_fe_rx_freq_range(chan);
        return dsp_freq_range;
    }

    freq_range_t get_fe_rx_freq_range(size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_frequency_range(rx_chain.block_chan);
    }

    /**************************************************************************
     * LO controls
     *************************************************************************/
    std::vector<std::string> get_rx_lo_names(size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_lo_names(rx_chain.block_chan);
    }

    void set_rx_lo_source(const std::string& src, const std::string& name, size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        rx_chain.radio->set_rx_lo_source(src, name, rx_chain.block_chan);
    }

    const std::string get_rx_lo_source(const std::string& name, size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_lo_source(name, rx_chain.block_chan);
    }

    std::vector<std::string> get_rx_lo_sources(const std::string& name, size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_lo_sources(name, chan);
    }

    void set_rx_lo_export_enabled(bool enabled, const std::string& name, size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->set_rx_lo_export_enabled(
            enabled, name, rx_chain.block_chan);
    }

    bool get_rx_lo_export_enabled(const std::string& name, size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_lo_export_enabled(name, rx_chain.block_chan);
    }

    double set_rx_lo_freq(double freq, const std::string& name, size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->set_rx_lo_freq(freq, name, rx_chain.block_chan);
    }

    double get_rx_lo_freq(const std::string& name, size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_lo_freq(name, rx_chain.block_chan);
    }

    freq_range_t get_rx_lo_freq_range(const std::string& name, size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_lo_freq_range(name, rx_chain.block_chan);
    }

    /*** TX LO API ***/
    std::vector<std::string> get_tx_lo_names(size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_lo_names(tx_chain.block_chan);
    }

    void set_tx_lo_source(
        const std::string& src, const std::string& name, const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        tx_chain.radio->set_tx_lo_source(src, name, tx_chain.block_chan);
    }

    const std::string get_tx_lo_source(const std::string& name, const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_lo_source(name, tx_chain.block_chan);
    }

    std::vector<std::string> get_tx_lo_sources(const std::string& name, const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_lo_sources(name, tx_chain.block_chan);
    }

    void set_tx_lo_export_enabled(
        const bool enabled, const std::string& name, const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        tx_chain.radio->set_tx_lo_export_enabled(enabled, name, tx_chain.block_chan);
    }

    bool get_tx_lo_export_enabled(const std::string& name, const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_lo_export_enabled(name, tx_chain.block_chan);
    }

    double set_tx_lo_freq(const double freq, const std::string& name, const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->set_tx_lo_freq(freq, name, tx_chain.block_chan);
    }

    double get_tx_lo_freq(const std::string& name, const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_lo_freq(name, tx_chain.block_chan);
    }

    freq_range_t get_tx_lo_freq_range(const std::string& name, const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_lo_freq_range(name, tx_chain.block_chan);
    }

    /**************************************************************************
     * Gain controls
     *************************************************************************/
    void set_rx_gain(double gain, const std::string& name, size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        rx_chain.radio->set_rx_gain(gain, name, rx_chain.block_chan);
    }

    std::vector<std::string> get_rx_gain_profile_names(const size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_gain_profile_names(rx_chain.block_chan);
    }

    void set_rx_gain_profile(const std::string& profile, const size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        rx_chain.radio->set_rx_gain_profile(profile, rx_chain.block_chan);
    }

    std::string get_rx_gain_profile(const size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_gain_profile(rx_chain.block_chan);
    }

    void set_normalized_rx_gain(double gain, size_t chan = 0)
    {
        if (gain > 1.0 || gain < 0.0) {
            throw uhd::runtime_error("Normalized gain out of range, must be in [0, 1].");
        }
        gain_range_t gain_range = get_rx_gain_range(ALL_GAINS, chan);
        double abs_gain =
            (gain * (gain_range.stop() - gain_range.start())) + gain_range.start();
        set_rx_gain(abs_gain, ALL_GAINS, chan);
    }

    void set_rx_agc(bool enable, size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        rx_chain.radio->set_rx_agc(enable, rx_chain.block_chan);
    }

    double get_rx_gain(const std::string& name, size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_gain(name, rx_chain.block_chan);
    }

    double get_normalized_rx_gain(size_t chan)
    {
        gain_range_t gain_range       = get_rx_gain_range(ALL_GAINS, chan);
        const double gain_range_width = gain_range.stop() - gain_range.start();
        // In case we have a device without a range of gains:
        if (gain_range_width == 0.0) {
            return 0;
        }
        const double norm_gain =
            (get_rx_gain(ALL_GAINS, chan) - gain_range.start()) / gain_range_width;
        // Avoid rounding errors:
        return std::max(std::min(norm_gain, 1.0), 0.0);
    }

    gain_range_t get_rx_gain_range(const std::string& name, size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_gain_range(name, rx_chain.block_chan);
    }

    std::vector<std::string> get_rx_gain_names(size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_gain_names(rx_chain.block_chan);
    }

    void set_rx_antenna(const std::string& ant, size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        rx_chain.radio->set_rx_antenna(ant, rx_chain.block_chan);
    }

    std::string get_rx_antenna(size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_antenna(rx_chain.block_chan);
    }

    std::vector<std::string> get_rx_antennas(size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_antennas(rx_chain.block_chan);
    }

    void set_rx_bandwidth(double bandwidth, size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        rx_chain.radio->set_rx_bandwidth(bandwidth, rx_chain.block_chan);
    }

    double get_rx_bandwidth(size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_bandwidth(rx_chain.block_chan);
    }

    meta_range_t get_rx_bandwidth_range(size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_bandwidth_range(rx_chain.block_chan);
    }

    dboard_iface::sptr get_rx_dboard_iface(size_t chan)
    {
        auto& rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_tree()->access<dboard_iface::sptr>("iface").get();
    }

    sensor_value_t get_rx_sensor(const std::string& name, size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_sensor(name, rx_chain.block_chan);
    }

    std::vector<std::string> get_rx_sensor_names(size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_sensor_names(rx_chain.block_chan);
    }

    void set_rx_dc_offset(const bool enb, size_t chan = ALL_CHANS)
    {
        if (chan != ALL_CHANS) {
            auto rx_chain = _get_rx_chan(chan);
            rx_chain.radio->set_rx_dc_offset(enb, rx_chain.block_chan);
            return;
        }
        for (size_t ch = 0; ch < get_rx_num_channels(); ++ch) {
            set_rx_dc_offset(enb, ch);
        }
    }

    void set_rx_dc_offset(const std::complex<double>& offset, size_t chan = ALL_CHANS)
    {
        if (chan != ALL_CHANS) {
            auto rx_chain = _get_rx_chan(chan);
            rx_chain.radio->set_rx_dc_offset(offset, rx_chain.block_chan);
            return;
        }
        for (size_t ch = 0; ch < get_rx_num_channels(); ++ch) {
            set_rx_dc_offset(offset, ch);
        }
    }

    meta_range_t get_rx_dc_offset_range(size_t chan)
    {
        auto rx_chain = _get_rx_chan(chan);
        return rx_chain.radio->get_rx_dc_offset_range(rx_chain.block_chan);
    }

    void set_rx_iq_balance(const bool enb, size_t chan)
    {
        if (chan != ALL_CHANS) {
            auto rx_chain = _get_rx_chan(chan);
            rx_chain.radio->set_rx_iq_balance(enb, rx_chain.block_chan);
            return;
        }
        for (size_t ch = 0; ch < get_rx_num_channels(); ++ch) {
            set_rx_iq_balance(enb, ch);
        }
    }

    void set_rx_iq_balance(
        const std::complex<double>& correction, size_t chan = ALL_CHANS)
    {
        if (chan != ALL_CHANS) {
            auto rx_chain = _get_rx_chan(chan);
            rx_chain.radio->set_rx_iq_balance(correction, rx_chain.block_chan);
            return;
        }
        for (size_t ch = 0; ch < get_rx_num_channels(); ++ch) {
            set_rx_iq_balance(correction, ch);
        }
    }

    /*******************************************************************
     * TX methods
     ******************************************************************/
    tx_chan_t _generate_tx_radio_chan(block_id_t radio_id, size_t block_chan)
    {
        auto radio_blk = _graph->get_block<uhd::rfnoc::radio_control>(radio_id);
        // We assume that the duc connected to this radio block has the same mboard,
        // instance, and port number
        auto duc_id =
            block_id_t(radio_id.get_device_no(), "DUC", radio_id.get_block_count());
        uhd::rfnoc::duc_block_control::sptr duc_blk;
        try {
            duc_blk = _graph->get_block<uhd::rfnoc::duc_block_control>(duc_id);
        } catch (const uhd::exception&) {
            UHD_LOGGER_TRACE("MULTI_USRP_RFNOC")
                << boost::format("No DUC found: %s") % duc_id.to_string();
        }
        // Figure out if this channel has a DUC available
        auto this_chan_duc =
            duc_blk && _graph->is_connectable(duc_id, block_chan, radio_id, block_chan)
                ? duc_blk
                : nullptr;
        return {radio_blk, this_chan_duc, block_chan};
    }

    std::vector<tx_chan_t> _generate_mboard_tx_chans(
        const uhd::usrp::subdev_spec_t& spec, size_t mboard)
    {
        // Discover all of the radios on our devices and create a mapping between radio
        // chains and channel numbers
        auto radio_blk_ids = _graph->find_blocks(std::to_string(mboard) + "/Radio");
        // find_blocks doesn't sort, so we need to
        std::sort(radio_blk_ids.begin(),
            radio_blk_ids.end(),
            [](uhd::rfnoc::block_id_t i, uhd::rfnoc::block_id_t j) -> bool {
                if (i.get_device_no() != j.get_device_no()) {
                    return i.get_device_no() < j.get_device_no();
                } else {
                    return i.get_block_count() < j.get_block_count();
                }
            });

        // If we don't find any radios, we don't have a multi_usrp object
        if (radio_blk_ids.empty()) {
            throw uhd::runtime_error(
                "[multi_usrp] No radios found in the requested mboard: "
                + std::to_string(mboard));
        }

        // Iterate through the subdev pairs, and try to find a radio that matches
        std::vector<tx_chan_t> new_chans;
        for (auto chan_subdev_pair : spec) {
            bool subdev_found = false;
            for (auto radio_id : radio_blk_ids) {
                auto radio_blk  = _graph->get_block<uhd::rfnoc::radio_control>(radio_id);
                size_t block_chan;
                try {
                    block_chan = radio_blk->get_chan_from_dboard_fe(
                        chan_subdev_pair.sd_name, TX_DIRECTION);
                } catch (const uhd::lookup_error&) {
                    // This is OK, since we're probing all radios, this
                    // particular radio may not have the requested frontend name
                    // so it's not one that we want in this list.
                    continue;
                }
                subdev_spec_pair_t radio_subdev(radio_blk->get_slot_name(),
                    radio_blk->get_dboard_fe_from_chan(block_chan, uhd::TX_DIRECTION));
                if (chan_subdev_pair == radio_subdev) {
                    new_chans.push_back(_generate_tx_radio_chan(radio_id, block_chan));
                    subdev_found = true;
                }
            }
            if (!subdev_found) {
                std::string err_msg("Could not find radio on mboard "
                                    + std::to_string(mboard) + " that matches subdev "
                                    + chan_subdev_pair.db_name + ":"
                                    + chan_subdev_pair.sd_name);
                UHD_LOG_ERROR("MULTI_USRP", err_msg);
                throw uhd::lookup_error(err_msg);
            }
        }
        UHD_LOG_TRACE("MULTI_USRP",
            std::string("Using TX subdev " + spec.to_string() + ", found ")
                + std::to_string(new_chans.size()) + " channels for mboard "
                + std::to_string(mboard));
        return new_chans;
    }

    void set_tx_subdev_spec(
        const uhd::usrp::subdev_spec_t& spec, size_t mboard = ALL_MBOARDS)
    {
        /* TODO: Refactor with get_rx_subdev_spec- the algorithms are the same, just the
         * types are different
         */
        // First, generate a vector of the tx channels that we need to register
        auto new_tx_chans = [this, spec, mboard]() {
            /* When setting the subdev spec in multiple mboard scenarios, there are two
             * cases we need to handle:
             * 1. Setting all mboard to the same subdev spec. This is the easy case.
             * 2. Setting a single mboard's subdev spec. In this case, we need to update
             * the requested mboard's subdev spec, and keep the old subdev spec for the
             * other mboards.
             */
            std::vector<tx_chan_t> new_tx_chans;
            for (size_t current_mboard = 0; current_mboard < get_num_mboards();
                 ++current_mboard) {
                auto current_spec = [this, spec, mboard, current_mboard]() {
                    if (mboard == ALL_MBOARDS || mboard == current_mboard) {
                        // Update all mboards to the same subdev spec OR
                        // only update this mboard to the new subdev spec
                        return spec;
                    } else {
                        // Keep the old subdev spec for this mboard
                        return get_tx_subdev_spec(current_mboard);
                    }
                }();
                auto new_mboard_chans =
                    _generate_mboard_tx_chans(current_spec, current_mboard);
                new_tx_chans.insert(
                    new_tx_chans.end(), new_mboard_chans.begin(), new_mboard_chans.end());
            }
            return new_tx_chans;
        }();

        // Now register them
        _tx_chans.clear();
        for (size_t tx_chan = 0; tx_chan < new_tx_chans.size(); ++tx_chan) {
            _tx_chans.emplace(tx_chan, new_tx_chans.at(tx_chan));
        }
    }

    uhd::usrp::subdev_spec_t get_tx_subdev_spec(size_t mboard)
    {
        uhd::usrp::subdev_spec_t result;
        for (size_t tx_chan = 0; tx_chan < get_tx_num_channels(); tx_chan++) {
            auto& tx_chain = _tx_chans.at(tx_chan);
            if (tx_chain.radio->get_block_id().get_device_no() == mboard) {
                result.push_back(
                    uhd::usrp::subdev_spec_pair_t(tx_chain.radio->get_slot_name(),
                        tx_chain.radio->get_dboard_fe_from_chan(
                            tx_chain.block_chan, uhd::TX_DIRECTION)));
            }
        }

        return result;
    }

    size_t get_tx_num_channels(void)
    {
        return _tx_chans.size();
    }

    std::string get_tx_subdev_name(size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_fe_name(tx_chain.block_chan, uhd::TX_DIRECTION);
    }

    void set_tx_rate(double rate, size_t chan = ALL_CHANS)
    {
        std::lock_guard<std::recursive_mutex> l(_graph_mutex);
        if (chan == ALL_CHANS) {
            for (size_t chan = 0; chan < _tx_chans.size(); ++chan) {
                set_tx_rate(rate, chan);
            }
            return;
        }
        const double actual_rate = [&]() {
            auto tx_chain = _get_tx_chan(chan);
            if (tx_chain.duc) {
                return tx_chain.duc->set_input_rate(rate, tx_chain.block_chan);
            } else {
                return tx_chain.radio->set_rate(rate);
            }
        }();
        if (actual_rate != rate) {
            UHD_LOGGER_WARNING("MULTI_USRP")
                << boost::format(
                       "Could not set TX rate to %.3f MHz. Actual rate is %.3f MHz")
                       % (rate / 1.0e6) % (actual_rate / 1.0e6);
        }
        _tx_rates[chan] = actual_rate;
    }

    double get_tx_rate(size_t chan)
    {
        std::lock_guard<std::recursive_mutex> l(_graph_mutex);
        auto& tx_chain = _get_tx_chan(chan);
        if (tx_chain.duc) {
            return tx_chain.duc->get_input_rate(tx_chain.block_chan);
        }
        return tx_chain.radio->get_rate();
    }

    meta_range_t get_tx_rates(size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return (tx_chain.duc)
                   ? make_overall_tune_range(
                         tx_chain.radio->get_tx_frequency_range(tx_chain.block_chan),
                         tx_chain.duc->get_frequency_range(tx_chain.block_chan),
                         tx_chain.radio->get_tx_bandwidth(tx_chain.block_chan))
                   : tx_chain.radio->get_tx_frequency_range(tx_chain.block_chan);
    }

    tune_result_t set_tx_freq(const tune_request_t& tune_request, size_t chan)
    {
        std::lock_guard<std::recursive_mutex> l(_graph_mutex);
        auto tx_chain = _get_tx_chan(chan);

        tx_chain.radio->set_tx_tune_args(tune_request.args, tx_chain.block_chan);
        //------------------------------------------------------------------
        //-- calculate the tunable frequency ranges of the system
        //------------------------------------------------------------------
        freq_range_t tune_range =
            (tx_chain.duc)
                ? make_overall_tune_range(
                      tx_chain.radio->get_tx_frequency_range(tx_chain.block_chan),
                      tx_chain.duc->get_frequency_range(tx_chain.block_chan),
                      tx_chain.radio->get_tx_bandwidth(tx_chain.block_chan))
                : tx_chain.radio->get_tx_frequency_range(tx_chain.block_chan);

        freq_range_t rf_range =
            tx_chain.radio->get_tx_frequency_range(tx_chain.block_chan);
        freq_range_t dsp_range =
            (tx_chain.duc) ? tx_chain.duc->get_frequency_range(tx_chain.block_chan)
                           : meta_range_t(0, 0);
        // Create lambdas to feed to tune_xx_subdev_and_dsp()
        // Note: If there is no DDC present, register empty lambdas for the DSP functions
        auto set_rf_freq = [tx_chain](double freq) {
            tx_chain.radio->set_tx_frequency(freq, tx_chain.block_chan);
        };
        auto get_rf_freq = [tx_chain](void) {
            return tx_chain.radio->get_tx_frequency(tx_chain.block_chan);
        };
        auto set_dsp_freq = [tx_chain](double freq) {
            (tx_chain.duc) ? tx_chain.duc->set_freq(freq, tx_chain.block_chan) : 0;
        };
        auto get_dsp_freq = [tx_chain](void) {
            return (tx_chain.duc) ? tx_chain.duc->get_freq(tx_chain.block_chan) : 0.0;
        };
        return tune_xx_subdev_and_dsp(TX_SIGN,
            tune_range,
            rf_range,
            dsp_range,
            set_rf_freq,
            get_rf_freq,
            set_dsp_freq,
            get_dsp_freq,
            tune_request);
    }

    double get_tx_freq(size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_frequency(tx_chain.block_chan);
    }

    freq_range_t get_tx_freq_range(size_t chan)
    {
        auto tx_chain = _tx_chans.at(chan);
        return (tx_chain.duc)
                   ? make_overall_tune_range(get_fe_rx_freq_range(chan),
                         tx_chain.duc->get_frequency_range(tx_chain.block_chan),
                         tx_chain.radio->get_rx_bandwidth(tx_chain.block_chan))
                   : get_fe_rx_freq_range(chan);
    }

    freq_range_t get_fe_tx_freq_range(size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_frequency_range(tx_chain.block_chan);
    }

    void set_tx_gain(double gain, const std::string& name, size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        tx_chain.radio->set_tx_gain(gain, name, tx_chain.block_chan);
    }

    std::vector<std::string> get_tx_gain_profile_names(const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_gain_profile_names(tx_chain.block_chan);
    }

    void set_tx_gain_profile(const std::string& profile, const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        tx_chain.radio->set_tx_gain_profile(profile, tx_chain.block_chan);
    }

    std::string get_tx_gain_profile(const size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_gain_profile(tx_chain.block_chan);
    }

    void set_normalized_tx_gain(double gain, size_t chan = 0)
    {
        if (gain > 1.0 || gain < 0.0) {
            throw uhd::runtime_error("Normalized gain out of range, must be in [0, 1].");
        }
        gain_range_t gain_range = get_tx_gain_range(ALL_GAINS, chan);
        double abs_gain =
            (gain * (gain_range.stop() - gain_range.start())) + gain_range.start();
        set_tx_gain(abs_gain, ALL_GAINS, chan);
    }

    double get_tx_gain(const std::string& name, size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_gain(name, tx_chain.block_chan);
    }

    double get_normalized_tx_gain(size_t chan)
    {
        gain_range_t gain_range       = get_tx_gain_range(ALL_GAINS, chan);
        const double gain_range_width = gain_range.stop() - gain_range.start();
        // In case we have a device without a range of gains:
        if (gain_range_width == 0.0) {
            return 0;
        }
        const double norm_gain =
            (get_tx_gain(ALL_GAINS, chan) - gain_range.start()) / gain_range_width;
        // Avoid rounding errors:
        return std::max(std::min(norm_gain, 1.0), 0.0);
    }

    gain_range_t get_tx_gain_range(const std::string& name, size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_gain_range(name, tx_chain.block_chan);
    }

    std::vector<std::string> get_tx_gain_names(size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_gain_names(tx_chain.block_chan);
    }

    void set_tx_antenna(const std::string& ant, size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        tx_chain.radio->set_tx_antenna(ant, tx_chain.block_chan);
    }

    std::string get_tx_antenna(size_t chan)
    {
        auto& tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_antenna(tx_chain.block_chan);
    }

    std::vector<std::string> get_tx_antennas(size_t chan)
    {
        auto& tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_antennas(tx_chain.block_chan);
    }

    void set_tx_bandwidth(double bandwidth, size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        tx_chain.radio->set_tx_bandwidth(bandwidth, tx_chain.block_chan);
    }

    double get_tx_bandwidth(size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_bandwidth(tx_chain.block_chan);
    }

    meta_range_t get_tx_bandwidth_range(size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_bandwidth_range(tx_chain.block_chan);
    }

    dboard_iface::sptr get_tx_dboard_iface(size_t chan)
    {
        auto& tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tree()->access<dboard_iface::sptr>("iface").get();
    }

    sensor_value_t get_tx_sensor(const std::string& name, size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_sensor(name, tx_chain.block_chan);
    }

    std::vector<std::string> get_tx_sensor_names(size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_sensor_names(tx_chain.block_chan);
    }

    void set_tx_dc_offset(const std::complex<double>& offset, size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        tx_chain.radio->set_tx_dc_offset(offset, tx_chain.block_chan);
    }

    meta_range_t get_tx_dc_offset_range(size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        return tx_chain.radio->get_tx_dc_offset_range(tx_chain.block_chan);
    }

    void set_tx_iq_balance(const std::complex<double>& correction, size_t chan)
    {
        auto tx_chain = _get_tx_chan(chan);
        tx_chain.radio->set_tx_iq_balance(correction, tx_chain.block_chan);
    }

    /*******************************************************************
     * GPIO methods
     ******************************************************************/
    /* Helper function to get the radio block controller which controls the GPIOs for a
     * given motherboard
     */
    uhd::rfnoc::radio_control::sptr _get_gpio_radio(const size_t mboard)
    {
        // We assume that the first radio block on each board controls the GPIO banks
        return _graph->get_block<uhd::rfnoc::radio_control>(
            block_id_t(mboard, "Radio", 0));
    }

    std::vector<std::string> get_gpio_banks(const size_t mboard)
    {
        return _get_gpio_radio(mboard)->get_gpio_banks();
    }

    void set_gpio_attr(const std::string& bank,
        const std::string& attr,
        const uint32_t value,
        const uint32_t mask = 0xffffffff,
        const size_t mboard = 0)
    {
        const uint32_t current   = get_gpio_attr(bank, attr, mboard);
        const uint32_t new_value = (current & ~mask) | (value & mask);
        return _get_gpio_radio(mboard)->set_gpio_attr(bank, attr, new_value);
    }

    uint32_t get_gpio_attr(
        const std::string& bank, const std::string& attr, const size_t mboard)
    {
        return _get_gpio_radio(mboard)->get_gpio_attr(bank, attr);
    }

    std::vector<std::string> get_gpio_srcs(const std::string& bank, const size_t mboard)
    {
        return get_mbc(mboard)->get_gpio_srcs(bank);
    }

    std::vector<std::string> get_gpio_src(const std::string& bank, const size_t mboard)
    {
        return get_mbc(mboard)->get_gpio_src(bank);
    }

    void set_gpio_src(
        const std::string& bank, const std::vector<std::string>& src, const size_t mboard)
    {
        get_mbc(mboard)->set_gpio_src(bank, src);
    }

    /*******************************************************************
     * Filter API methods
     ******************************************************************/
    std::vector<std::string> get_rx_filter_names(const size_t chan)
    {
        std::vector<std::string> filter_names;
        // Grab the Radio's filters
        auto rx_chan    = _get_rx_chan(chan);
        auto radio_id   = rx_chan.radio->get_block_id();
        auto radio_ctrl = std::dynamic_pointer_cast<detail::filter_node>(rx_chan.radio);
        if (radio_ctrl) {
            auto radio_filters = radio_ctrl->get_rx_filter_names(rx_chan.block_chan);
            // Prepend the radio's block ID to each filter name
            std::transform(radio_filters.begin(),
                radio_filters.end(),
                radio_filters.begin(),
                [radio_id](
                    std::string name) { return radio_id.to_string() + ":" + name; });
            // Add the radio's filter names to the return vector
            filter_names.insert(
                filter_names.end(), radio_filters.begin(), radio_filters.end());
        } else {
            UHD_LOG_DEBUG("MULTI_USRP",
                "Radio block " + radio_id.to_string() + " does not support filters");
        }
        // Grab the DDC's filter
        auto ddc_id   = rx_chan.ddc->get_block_id();
        auto ddc_ctrl = std::dynamic_pointer_cast<detail::filter_node>(rx_chan.ddc);
        if (ddc_ctrl) {
            auto ddc_filters = ddc_ctrl->get_rx_filter_names(rx_chan.block_chan);
            // Prepend the DDC's block ID to each filter name
            std::transform(ddc_filters.begin(),
                ddc_filters.end(),
                ddc_filters.begin(),
                [ddc_id](std::string name) { return ddc_id.to_string() + ":" + name; });
            // Add the radio's filter names to the return vector
            filter_names.insert(
                filter_names.end(), ddc_filters.begin(), ddc_filters.end());
        } else {
            UHD_LOG_DEBUG("MULTI_USRP",
                "DDC block " + ddc_id.to_string() + " does not support filters");
        }
        return filter_names;
    }

    uhd::filter_info_base::sptr get_rx_filter(const std::string& name, const size_t chan)
    {
        try {
            // The block_id_t constructor is pretty smart; let it handle the parsing.
            block_id_t block_id(name);
            auto rx_chan = _get_rx_chan(chan);
            // The filter name is the `name` after the BLOCK_ID and a `:`
            std::string filter_name = name.substr(block_id.to_string().size() + 1);
            // Try to dynamic cast either the radio or the DDC to a filter_node, and call
            // its filter function
            auto block_ctrl = [rx_chan, block_id, chan]() -> noc_block_base::sptr {
                if (block_id == rx_chan.radio->get_block_id()) {
                    return rx_chan.radio;
                } else if (block_id == rx_chan.ddc->get_block_id()) {
                    return rx_chan.ddc;
                } else {
                    throw uhd::runtime_error("Requested block " + block_id.to_string()
                                             + " does not match block ID in channel "
                                             + std::to_string(chan));
                }
            }();
            auto filter_ctrl = std::dynamic_pointer_cast<detail::filter_node>(block_ctrl);
            if (filter_ctrl) {
                return filter_ctrl->get_rx_filter(filter_name, rx_chan.block_chan);
            }
            std::string err_msg =
                block_ctrl->get_block_id().to_string() + " does not support filters";
            UHD_LOG_ERROR("MULTI_USRP", err_msg);
            throw uhd::runtime_error(err_msg);
        } catch (const uhd::value_error&) {
            // Catch the error from the block_id_t constructor and add better logging
            UHD_LOG_ERROR("MULTI_USRP",
                "Invalid filter name; could not determine block controller from name: "
                    + name);
            throw;
        }
    }

    void set_rx_filter(
        const std::string& name, uhd::filter_info_base::sptr filter, const size_t chan)
    {
        try {
            // The block_id_t constructor is pretty smart; let it handle the parsing.
            block_id_t block_id(name);
            auto rx_chan = _get_rx_chan(chan);
            // The filter name is the `name` after the BLOCK_ID and a `:`
            std::string filter_name = name.substr(block_id.to_string().size() + 1);
            // Try to dynamic cast either the radio or the DDC to a filter_node, and call
            // its filter function
            auto block_ctrl = [rx_chan, block_id, chan]() -> noc_block_base::sptr {
                if (block_id == rx_chan.radio->get_block_id()) {
                    return rx_chan.radio;
                } else if (block_id == rx_chan.ddc->get_block_id()) {
                    return rx_chan.ddc;
                } else {
                    throw uhd::runtime_error("Requested block " + block_id.to_string()
                                             + " does not match block ID in channel "
                                             + std::to_string(chan));
                }
            }();
            auto filter_ctrl = std::dynamic_pointer_cast<detail::filter_node>(block_ctrl);
            if (filter_ctrl) {
                return filter_ctrl->set_rx_filter(
                    filter_name, filter, rx_chan.block_chan);
            }
            std::string err_msg =
                block_ctrl->get_block_id().to_string() + " does not support filters";
            UHD_LOG_ERROR("MULTI_USRP", err_msg);
            throw uhd::runtime_error(err_msg);
        } catch (const uhd::value_error&) {
            // Catch the error from the block_id_t constructor and add better logging
            UHD_LOG_ERROR("MULTI_USRP",
                "Invalid filter name; could not determine block controller from name: "
                    + name);
            throw;
        }
    }

    std::vector<std::string> get_tx_filter_names(const size_t chan)
    {
        std::vector<std::string> filter_names;
        // Grab the Radio's filters
        auto tx_chan    = _get_tx_chan(chan);
        auto radio_id   = tx_chan.radio->get_block_id();
        auto radio_ctrl = std::dynamic_pointer_cast<detail::filter_node>(tx_chan.radio);
        if (radio_ctrl) {
            auto radio_filters = radio_ctrl->get_tx_filter_names(tx_chan.block_chan);
            // Prepend the radio's block ID to each filter name
            std::transform(radio_filters.begin(),
                radio_filters.end(),
                radio_filters.begin(),
                [radio_id](
                    std::string name) { return radio_id.to_string() + ":" + name; });
            // Add the radio's filter names to the return vector
            filter_names.insert(
                filter_names.end(), radio_filters.begin(), radio_filters.end());
        } else {
            UHD_LOG_DEBUG("MULTI_USRP",
                "Radio block " + radio_id.to_string() + " does not support filters");
        }
        // Grab the DUC's filter
        auto duc_id   = tx_chan.duc->get_block_id();
        auto duc_ctrl = std::dynamic_pointer_cast<detail::filter_node>(tx_chan.duc);
        if (duc_ctrl) {
            auto duc_filters = duc_ctrl->get_tx_filter_names(tx_chan.block_chan);
            // Prepend the DUC's block ID to each filter name
            std::transform(duc_filters.begin(),
                duc_filters.end(),
                duc_filters.begin(),
                [duc_id](std::string name) { return duc_id.to_string() + ":" + name; });
            // Add the radio's filter names to the return vector
            filter_names.insert(
                filter_names.end(), duc_filters.begin(), duc_filters.end());
        } else {
            UHD_LOG_DEBUG("MULTI_USRP",
                "DUC block " + duc_id.to_string() + " does not support filters");
        }
        return filter_names;
    }

    uhd::filter_info_base::sptr get_tx_filter(const std::string& name, const size_t chan)
    {
        try {
            // The block_id_t constructor is pretty smart; let it handle the parsing.
            block_id_t block_id(name);
            auto tx_chan = _get_tx_chan(chan);
            // The filter name is the `name` after the BLOCK_ID and a `:`
            std::string filter_name = name.substr(block_id.to_string().size() + 1);
            // Try to dynamic cast either the radio or the DUC to a filter_node, and call
            // its filter function
            auto block_ctrl = [tx_chan, block_id, chan]() -> noc_block_base::sptr {
                if (block_id == tx_chan.radio->get_block_id()) {
                    return tx_chan.radio;
                } else if (block_id == tx_chan.duc->get_block_id()) {
                    return tx_chan.duc;
                } else {
                    throw uhd::runtime_error("Requested block " + block_id.to_string()
                                             + " does not match block ID in channel "
                                             + std::to_string(chan));
                }
            }();
            auto filter_ctrl = std::dynamic_pointer_cast<detail::filter_node>(block_ctrl);
            if (filter_ctrl) {
                return filter_ctrl->get_tx_filter(filter_name, tx_chan.block_chan);
            }
            std::string err_msg =
                block_ctrl->get_block_id().to_string() + " does not support filters";
            UHD_LOG_ERROR("MULTI_USRP", err_msg);
            throw uhd::runtime_error(err_msg);
        } catch (const uhd::value_error&) {
            // Catch the error from the block_id_t constructor and add better logging
            UHD_LOG_ERROR("MULTI_USRP",
                "Invalid filter name; could not determine block controller from name: "
                    + name);
            throw;
        }
    }

    void set_tx_filter(
        const std::string& name, uhd::filter_info_base::sptr filter, const size_t chan)
    {
        try {
            // The block_id_t constructor is pretty smart; let it handle the parsing.
            block_id_t block_id(name);
            auto tx_chan = _get_tx_chan(chan);
            // The filter name is the `name` after the BLOCK_ID and a `:`
            std::string filter_name = name.substr(block_id.to_string().size() + 1);
            // Try to dynamic cast either the radio or the DUC to a filter_node, and call
            // its filter function
            auto block_ctrl = [tx_chan, block_id, chan]() -> noc_block_base::sptr {
                if (block_id == tx_chan.radio->get_block_id()) {
                    return tx_chan.radio;
                } else if (block_id == tx_chan.duc->get_block_id()) {
                    return tx_chan.duc;
                } else {
                    throw uhd::runtime_error("Requested block " + block_id.to_string()
                                             + " does not match block ID in channel "
                                             + std::to_string(chan));
                }
            }();
            auto filter_ctrl = std::dynamic_pointer_cast<detail::filter_node>(block_ctrl);
            if (filter_ctrl) {
                return filter_ctrl->set_tx_filter(
                    filter_name, filter, tx_chan.block_chan);
            }
            std::string err_msg =
                block_ctrl->get_block_id().to_string() + " does not support filters";
            UHD_LOG_ERROR("MULTI_USRP", err_msg);
            throw uhd::runtime_error(err_msg);
        } catch (const uhd::value_error&) {
            // Catch the error from the block_id_t constructor and add better logging
            UHD_LOG_ERROR("MULTI_USRP",
                "Invalid filter name; could not determine block controller from name: "
                    + name);
            throw;
        }
    }

private:
    /**************************************************************************
     * Private Helpers
     *************************************************************************/
    mb_controller::sptr get_mbc(const size_t mb_idx)
    {
        if (mb_idx >= get_num_mboards()) {
            throw uhd::key_error(
                std::string("No such mboard: ") + std::to_string(mb_idx));
        }
        return _graph->get_mb_controller(mb_idx);
    }

    rx_chan_t& _get_rx_chan(const size_t chan)
    {
        if (!_rx_chans.count(chan)) {
            throw uhd::key_error(
                std::string("Invalid RX channel: ") + std::to_string(chan));
        }
        return _rx_chans.at(chan);
    }

    tx_chan_t& _get_tx_chan(const size_t chan)
    {
        if (!_tx_chans.count(chan)) {
            throw uhd::key_error(
                std::string("Invalid TX channel: ") + std::to_string(chan));
        }
        return _tx_chans.at(chan);
    }

    /**************************************************************************
     * Private Attributes
     *************************************************************************/
    //! Devices args used to spawn this multi_usrp
    const uhd::device_addr_t _args;
    //! Reference to rfnoc_graph
    rfnoc_graph::sptr _graph;
    //! Reference to the prop tree
    property_tree::sptr _tree;
    //! Mapping between channel number and the RFNoC blocks in that RX chain
    std::unordered_map<size_t, rx_chan_t> _rx_chans;
    //! Mapping between channel number and the RFNoC blocks in that TX chain
    std::unordered_map<size_t, tx_chan_t> _tx_chans;
    //! Cache the requested RX rates
    std::unordered_map<size_t, double> _rx_rates;
    //! Cache the requested TX rates
    std::unordered_map<size_t, double> _tx_rates;

    std::recursive_mutex _graph_mutex;
};

/******************************************************************************
 * Factory
 *****************************************************************************/
namespace uhd { namespace rfnoc { namespace detail {
// Forward declare
rfnoc_graph::sptr make_rfnoc_graph(
    detail::rfnoc_device::sptr dev, const uhd::device_addr_t& device_addr);

multi_usrp::sptr make_rfnoc_device(
    detail::rfnoc_device::sptr rfnoc_device, const uhd::device_addr_t& dev_addr)
{
    auto graph = uhd::rfnoc::detail::make_rfnoc_graph(rfnoc_device, dev_addr);
    return boost::make_shared<multi_usrp_rfnoc>(graph, dev_addr);
}

}}} // namespace uhd::rfnoc::detail
