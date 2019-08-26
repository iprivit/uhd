//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/exception.hpp>
#include <uhd/rfnoc/defaults.hpp>
#include <uhd/rfnoc/fft_block_control.hpp>
#include <uhd/rfnoc/registry.hpp>

#include <string>

namespace {

constexpr int DEFAULT_SIZE         = 256;
const uhd::rfnoc::FFT_SHIFT DEFAULT_SHIFT            = uhd::rfnoc::FFT_SHIFT::NORMAL;
const uhd::rfnoc::FFT_DIRECTION DEFAULT_DIRECTION            = uhd::rfnoc::FFT_DIRECTION::FORWARD;
constexpr int DEFAULT_SCALING        = 1706;

const uhd::rfnoc::io_type_t DEFAULT_TYPE = uhd::rfnoc::IO_TYPE_SC16;

} // namespace

using namespace uhd::rfnoc;


const uint32_t fft_block_control::RB_FFT_RESET     = 0;
const uint32_t fft_block_control::RB_MAGNITUDE_OUT     = 8;
const uint32_t fft_block_control::RB_FFT_SIZE_LOG2     = 16;
const uint32_t fft_block_control::RB_FFT_DIRECTION     = 24;
const uint32_t fft_block_control::RB_FFT_SCALING     = 32;
const uint32_t fft_block_control::RB_FFT_SHIFT_CONFIG     = 40;

const uint32_t fft_block_control::SR_FFT_RESET        = 131 * 8;
const uint32_t fft_block_control::SR_FFT_SIZE_LOG2        = 132 * 8;
const uint32_t fft_block_control::SR_MAGNITUDE_OUT        = 133 * 8;
const uint32_t fft_block_control::SR_FFT_DIRECTION        = 134 * 8;
const uint32_t fft_block_control::SR_FFT_SCALING        = 135 * 8;
const uint32_t fft_block_control::SR_FFT_SHIFT_CONFIG        = 136 * 8;

class fft_block_control_impl : public fft_block_control
{
public:
    RFNOC_BLOCK_CONSTRUCTOR(fft_block_control)
    {
        register_issue_stream_cmd();
    }

    void reset()
    {
        regs().poke32(SR_FFT_RESET, uint32_t(1));
        regs().poke32(SR_FFT_RESET, uint32_t(0));
    }


private:

    /**************************************************************************
     * Initialization
     *************************************************************************/
    void _register_props()
    {
        // register block specific properties
        register_property(&_size, [this]() {
            this->regs().poke32(SR_FFT_SIZE_LOG2, uint32_t(this->_size.get()));
        });
        register_property(&_magnitude, [this]() {
            this->regs().poke32(SR_MAGNITUDE_OUT, uint32_t(this->_magnitude.get()));
        });
        register_property(&_direction, [this]() {
            this->regs().poke32(SR_MAGNITUDE_OUT, uint32_t(this->_direction.get()));
        });
        register_property(&_scaling, [this]() {
            this->regs().poke32(SR_MAGNITUDE_OUT, uint32_t(this->_scaling.get()));
        });
        register_property(&_shift, [this]() {
            this->regs().poke32(SR_MAGNITUDE_OUT, uint32_t(this->_shift.get()));
        });

        //register edge properties
        register_property(&_type_in);
        register_property(&_type_out);

        //add resolvers for type (keeps it constant)
        add_property_resolver({&_type_in}, {&_type_in}, [& type_in = _type_in]() {
            type_in.set(IO_TYPE_SC16);
        });
        add_property_resolver({&_type_out}, {&_type_out}, [& type_out = _type_out]() {
            type_out.set(IO_TYPE_SC16);
        });

    }

    void register_issue_stream_cmd()
    {
        register_action_handler(ACTION_KEY_STREAM_CMD,
            [this](const res_source_info& src, action_info::sptr action) {
                stream_cmd_action_info::sptr stream_cmd_action =
                    std::dynamic_pointer_cast<stream_cmd_action_info>(action);
                if (!stream_cmd_action) {
                    throw uhd::runtime_error(
                        "Received stream_cmd of invalid action type!");
                }
                issue_stream_cmd_action_handler(src, stream_cmd_action);
            });
    }

    void issue_stream_cmd_action_handler(
        const res_source_info& src, stream_cmd_action_info::sptr stream_cmd_action)
    {
        res_source_info dst_edge{res_source_info::invert_edge(src.type)};
        uhd::stream_cmd_t::stream_mode_t stream_mode =
            stream_cmd_action->stream_cmd.stream_mode;
        RFNOC_LOG_TRACE("Received stream command: " << char(stream_mode) << " to "
                                                    << src.to_string()
                                                    << ", id==" << stream_cmd_action->id);
        auto new_action        = stream_cmd_action_info::make(stream_mode);
        new_action->stream_cmd = stream_cmd_action->stream_cmd;
        if (stream_mode == uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE
            || stream_mode == uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_MORE) {
            if (src.type == res_source_info::INPUT_EDGE) {
             //   new_action->stream_cmd.num_samps *= _interp.at(chan).get();
            } else {
               // new_action->stream_cmd.num_samps /= _interp.at(chan).get();
            }
            RFNOC_LOG_TRACE("Forwarding num_samps stream command, new value is "
                            << new_action->stream_cmd.num_samps);
        } else {
            RFNOC_LOG_TRACE("Forwarding continuous stream command...")
        }

        post_action(dst_edge, new_action);
    }

    property_t<int> _size{PROP_KEY_SPP, {res_source_info::USER}};
    property_t<int> _magnitude = property_t<int>{PROP_KEY_MAGNITUDE, {res_source_info::USER}};
    property_t<int> _direction = property_t<int>{PROP_KEY_DIRECTION, {res_source_info::USER}};
    property_t<int> _scaling = property_t<int>{PROP_KEY_SCALING, {res_source_info::USER}};
    property_t<int> _shift = property_t<int>{PROP_KEY_SHIFT, {res_source_info::USER}};

    property_t<std::string> _type_in = property_t<std::string>{PROP_KEY_TYPE, IO_TYPE_SC16, {res_source_info::INPUT_EDGE}};
    property_t<std::string> _type_out = property_t<std::string>{PROP_KEY_TYPE, IO_TYPE_SC16, {res_source_info::OUTPUT_EDGE}};
};

UHD_RFNOC_BLOCK_REGISTER_DIRECT(
    fft_block_control, 0xFF700000, "FFT", CLOCK_KEY_GRAPH, "bus_clk")
