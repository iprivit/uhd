//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/rfnoc/defaults.hpp>
#include <uhdlib/rfnoc/node_accessor.hpp>
#include <uhdlib/rfnoc/rfnoc_rx_streamer.hpp>
#include <atomic>

using namespace uhd;
using namespace uhd::rfnoc;

const std::string STREAMER_ID = "RxStreamer";
static std::atomic<uint64_t> streamer_inst_ctr;

rfnoc_rx_streamer::rfnoc_rx_streamer(const size_t num_chans,
    const uhd::stream_args_t stream_args)
    : rx_streamer_impl<chdr_rx_data_xport>(num_chans, stream_args)
    , _unique_id(STREAMER_ID + "#" + std::to_string(streamer_inst_ctr++))
{
    // No block to which to forward properties or actions
    set_prop_forwarding_policy(forwarding_policy_t::DROP);
    set_action_forwarding_policy(forwarding_policy_t::DROP);

    // Initialize properties
    _scaling_in.reserve(num_chans);
    _samp_rate_in.reserve(num_chans);
    _tick_rate_in.reserve(num_chans);
    _type_in.reserve(num_chans);

    for (size_t i = 0; i < num_chans; i++) {
        _register_props(i, stream_args.otw_format);
    }
    node_accessor_t node_accessor{};
    node_accessor.init_props(this);
}

std::string rfnoc_rx_streamer::get_unique_id() const
{
    return _unique_id;
}

size_t rfnoc_rx_streamer::get_num_input_ports() const
{
    return get_num_channels();
}

size_t rfnoc_rx_streamer::get_num_output_ports() const
{
    return 0;
}

void rfnoc_rx_streamer::issue_stream_cmd(const stream_cmd_t& stream_cmd)
{
    if (get_num_channels() > 1 and stream_cmd.stream_now
        and stream_cmd.stream_mode != stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS) {
        throw uhd::runtime_error(
            "Invalid recv stream command - stream now on multiple channels in a "
            "single streamer will fail to time align.");
    }

    auto cmd = stream_cmd_action_info::make(stream_cmd.stream_mode);
    cmd->stream_cmd = stream_cmd;

    for (size_t i = 0; i < get_num_channels(); i++) {
        const res_source_info info(res_source_info::INPUT_EDGE, i);
        post_action(info, cmd);
    }
}

const uhd::stream_args_t& rfnoc_rx_streamer::get_stream_args() const
{
    return _stream_args;
}

bool rfnoc_rx_streamer::check_topology(
    const std::vector<size_t>& connected_inputs,
    const std::vector<size_t>& connected_outputs)
{
    // Check that all channels are connected
    if (connected_inputs.size() != get_num_input_ports()) {
        return false;
    }

    // Call base class to check that connections are valid
    return node_t::check_topology(connected_inputs, connected_outputs);
}

void rfnoc_rx_streamer::_register_props(const size_t chan,
    const std::string& otw_format)
{
    // Create actual properties and store them
    _scaling_in.push_back(property_t<double>(
        PROP_KEY_SCALING, {res_source_info::INPUT_EDGE, chan}));
    _samp_rate_in.push_back(
        property_t<double>(PROP_KEY_SAMP_RATE, {res_source_info::INPUT_EDGE, chan}));
    _tick_rate_in.push_back(property_t<double>(
        PROP_KEY_TICK_RATE, {res_source_info::INPUT_EDGE, chan}));
    _type_in.emplace_back(property_t<std::string>(
        PROP_KEY_TYPE, otw_format, {res_source_info::INPUT_EDGE, chan}));

    // Give us some shorthands for the rest of this function
    property_t<double>* scaling_in   = &_scaling_in.back();
    property_t<double>* samp_rate_in = &_samp_rate_in.back();
    property_t<double>* tick_rate_in = &_tick_rate_in.back();
    property_t<std::string>* type_in = &_type_in.back();

    // Register them
    register_property(scaling_in);
    register_property(samp_rate_in);
    register_property(tick_rate_in);
    register_property(type_in);

    // Add resolvers
    add_property_resolver({scaling_in}, {},
        [&scaling_in = *scaling_in, chan, this]() {
            RFNOC_LOG_TRACE("Calling resolver for `scaling_in'@" << chan);
            if (scaling_in.is_valid()) {
                this->set_scale_factor(chan, scaling_in.get() / 32767.0);
            }
        });

    add_property_resolver({samp_rate_in}, {},
        [&samp_rate_in = *samp_rate_in, chan, this]() {
            RFNOC_LOG_TRACE("Calling resolver for `samp_rate_in'@" << chan);
            if (samp_rate_in.is_valid()) {
                this->set_samp_rate(samp_rate_in.get());
            }
        });

    add_property_resolver({tick_rate_in}, {},
        [&tick_rate_in = *tick_rate_in, chan, this]() {
            RFNOC_LOG_TRACE("Calling resolver for `tick_rate_in'@" << chan);
            if (tick_rate_in.is_valid()) {
                this->set_tick_rate(tick_rate_in.get());
            }
        });
}
