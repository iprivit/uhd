//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/rfnoc/actions.hpp>
#include <atomic>

using namespace uhd::rfnoc;

namespace {
    // A static counter, which we use to uniquely label actions
    std::atomic<size_t> action_counter{0};
}

action_info::action_info(const std::string& key_) : id(action_counter++), key(key_)
{
    // nop
}

//! Factory function
action_info::sptr action_info::make(const std::string& key)
{
    if (key == ACTION_KEY_STREAM_CMD) {
        return stream_cmd_action_info::make(
            uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
    }
    //return std::make_shared<action_info>(key);
    return sptr(new action_info(key));
}

stream_cmd_action_info::stream_cmd_action_info(
    const uhd::stream_cmd_t::stream_mode_t stream_mode)
    : action_info(ACTION_KEY_STREAM_CMD), stream_cmd(stream_mode)
{
    // nop
}

stream_cmd_action_info::sptr stream_cmd_action_info::make(
    const uhd::stream_cmd_t::stream_mode_t stream_mode)
{
    //return std::make_shared<action_info>(ACTION_KEY_STREAM_CMD);
    return sptr(new stream_cmd_action_info(stream_mode));
}
