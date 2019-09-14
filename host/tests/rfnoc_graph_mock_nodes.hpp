//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef INCLUDED_LIBUHD_TESTS_MOCK_NODES_HPP
#define INCLUDED_LIBUHD_TESTS_MOCK_NODES_HPP

#include <uhd/rfnoc/defaults.hpp>
#include <uhd/rfnoc/node.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhdlib/rfnoc/node_accessor.hpp>

using namespace uhd::rfnoc;

constexpr int MAX_DECIM       = 512;
constexpr double DEFAULT_RATE = 1e9;
constexpr int DEFAULT_DECIM   = 1;

/*! Mock Radio node
 *
 * - "Full Duplex"
 * - Has two master clock rates: 100e6 and 200e6
 * - RSSI is a read-only prop that always needs updating
 */
class mock_radio_node_t : public node_t
{
public:
    mock_radio_node_t(const size_t radio_idx) : _radio_idx(radio_idx)
    {
        register_property(&_samp_rate_in);
        register_property(&_samp_rate_out);
        register_property(&_master_clock_rate);
        register_property(&_rssi);

        // Resolver for the input rate: We don't actually try and be clever, we
        // always reset the rate back to the TX rate.
        add_property_resolver({&_samp_rate_in},
            {&_samp_rate_in},
            [& samp_rate_in        = _samp_rate_in,
                &master_clock_rate = _master_clock_rate,
                this]() {
                UHD_LOG_INFO(get_unique_id(), " Calling resolver for `samp_rate_in'...");
                samp_rate_in = master_clock_rate.get();
            });
        add_property_resolver({&_samp_rate_out},
            {&_samp_rate_out},
            [this]() {
                UHD_LOG_INFO(get_unique_id(), " Calling resolver for `samp_rate_out'...");
                if (this->disable_samp_out_resolver) {
                    _samp_rate_out = this->force_samp_out_value;
                    UHD_LOG_DEBUG(get_unique_id(),
                        "Forcing samp_rate_out to " << _samp_rate_out.get());
                    return;
                }
                this->_samp_rate_out = this->_master_clock_rate.get();
            });
        add_property_resolver({&_master_clock_rate},
            {&_master_clock_rate, &_samp_rate_in, &_samp_rate_out},
            [& samp_rate_out       = _samp_rate_out,
                &samp_rate_in      = _samp_rate_in,
                &master_clock_rate = _master_clock_rate,
                this]() {
                UHD_LOG_INFO(get_unique_id(), " Calling resolver for `master_clock_rate'...");
                if (_master_clock_rate.get() > 150e6) {
                    _master_clock_rate = 200e6;
                } else {
                    _master_clock_rate = 100e6;
                }
                _samp_rate_in = _master_clock_rate.get();
                if (!this->disable_samp_out_resolver) {
                    _samp_rate_out = _master_clock_rate.get();
                } else {
                    _samp_rate_out = this->force_samp_out_value;
                    UHD_LOG_DEBUG(get_unique_id(),
                        "Forcing samp_rate_out to " << _samp_rate_out.get());
                }
            });
        // By depending on ALWAYS_DIRTY, this property is always updated:
        add_property_resolver({&ALWAYS_DIRTY},
            {&_rssi},
            [this]() {
                UHD_LOG_INFO(get_unique_id(), " Calling resolver for `rssi'...");
                rssi_resolver_count++;
                _rssi = static_cast<double>(rssi_resolver_count);
            });


        set_action_forwarding_policy(forwarding_policy_t::DROP);

        register_action_handler(ACTION_KEY_STREAM_CMD,
            [this](const res_source_info& src, action_info::sptr action) {
                stream_cmd_action_info::sptr stream_cmd_action =
                    std::dynamic_pointer_cast<stream_cmd_action_info>(action);
                UHD_ASSERT_THROW(stream_cmd_action);
                uhd::stream_cmd_t::stream_mode_t stream_mode =
                    stream_cmd_action->stream_cmd.stream_mode;
                RFNOC_LOG_INFO("Received stream command: " << stream_mode << " to "
                                                           << src.to_string()
                                                           << ", id==" << action->id);
                if (stream_mode == uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS) {
                    UHD_LOG_INFO(get_unique_id(), "Starting Stream!");
                } else if (stream_mode == uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS) {
                    UHD_LOG_INFO(get_unique_id(), "Stopping Stream!");
                } else {
                    this->last_num_samps = stream_cmd_action->stream_cmd.num_samps;
                    RFNOC_LOG_INFO("Streaming num samps: " <<  this->last_num_samps);
                }
            });
    }

    void update_fwd_policy(forwarding_policy_t policy)
    {
        set_action_forwarding_policy(policy);
    }

    std::string get_unique_id() const
    {
        return "MOCK_RADIO" + std::to_string(_radio_idx);
    }

    size_t get_num_input_ports() const
    {
        return 1;
    }

    size_t get_num_output_ports() const
    {
        return 1;
    }

    // Some public attributes that help debugging
    size_t rssi_resolver_count = 0;
    bool disable_samp_out_resolver = false;
    double force_samp_out_value = 23e6;

    size_t last_num_samps = 0;

private:
    const size_t _radio_idx;

    property_t<double> _samp_rate_in{
        "samp_rate", 200e6, {res_source_info::INPUT_EDGE}};
    property_t<double> _samp_rate_out{
        "samp_rate", 200e6, {res_source_info::OUTPUT_EDGE}};
    property_t<double> _master_clock_rate{
        "master_clock_rate", 200e6, {res_source_info::USER}};
    property_t<double> _rssi{"rssi", 0, {res_source_info::USER}};
};

/*! Mock DDC node
 *
 * - Single channel
 * - Does simple coercion of decimation
 * - Keeps output and input rates consistent with decimation
 */
class mock_ddc_node_t : public node_t
{
public:
    mock_ddc_node_t()
    {
        register_property(&_samp_rate_in);
        register_property(&_samp_rate_out);
        register_property(&_decim);

        // Resolver for _decim: This gets executed when the user directly
        // modifies _decim. The desired behaviour is to coerce it first, then
        // keep the input rate constant, and re-calculate the output rate.
        add_property_resolver({&_decim},
            {&_decim, &_samp_rate_out},
            [& decim           = _decim,
                &samp_rate_out = _samp_rate_out,
                &samp_rate_in  = _samp_rate_in]() {
                UHD_LOG_INFO("MOCK DDC", "Calling resolver for `decim'...");
                decim         = coerce_decim(decim.get());
                samp_rate_out = samp_rate_in.get() / decim.get();
            });
        // Resolver for the input rate: We try and match decim so that the output
        // rate is not modified. If decim needs to be coerced, only then the
        // output rate is modified.
        add_property_resolver({&_samp_rate_in},
            {&_decim, &_samp_rate_out},
            [& decim           = _decim,
                &samp_rate_out = _samp_rate_out,
                &samp_rate_in  = _samp_rate_in]() {
                UHD_LOG_INFO("MOCK DDC", "Calling resolver for `samp_rate_in'...");
                decim = coerce_decim(int(samp_rate_in.get() / samp_rate_out.get()));
                samp_rate_out = samp_rate_in.get() / decim.get();
            });
        // Resolver for the output rate: Like the previous one, but flipped.
        add_property_resolver({&_samp_rate_out},
            {&_decim, &_samp_rate_in},
            [& decim           = _decim,
                &samp_rate_out = _samp_rate_out,
                &samp_rate_in  = _samp_rate_in]() {
                UHD_LOG_INFO("MOCK DDC", "Calling resolver for `samp_rate_out'...");
                decim = coerce_decim(int(samp_rate_in.get() / samp_rate_out.get()));
                samp_rate_in = samp_rate_out.get() * decim.get();
            });

        register_action_handler(
            ACTION_KEY_STREAM_CMD, [this](const res_source_info& src, action_info::sptr action) {
                res_source_info dst_edge{
                    res_source_info::invert_edge(src.type), src.instance};
                stream_cmd_action_info::sptr stream_cmd_action =
                    std::dynamic_pointer_cast<stream_cmd_action_info>(action);
                UHD_ASSERT_THROW(stream_cmd_action);
                uhd::stream_cmd_t::stream_mode_t stream_mode =
                    stream_cmd_action->stream_cmd.stream_mode;
                RFNOC_LOG_INFO("Received stream command: " << stream_mode << " to "
                                                           << src.to_string()
                                                           << ", id==" << action->id);
                auto new_action = stream_cmd_action_info::make(stream_mode);
                new_action->stream_cmd = stream_cmd_action->stream_cmd;
                if (stream_mode == uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE
                    || stream_mode == uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_MORE) {
                    if (src.type == res_source_info::OUTPUT_EDGE) {
                        RFNOC_LOG_INFO("Multiplying num_samps by " << _decim.get());
                        new_action->stream_cmd.num_samps *= _decim.get();
                    } else {
                        RFNOC_LOG_INFO("Dividing num_samps by " << _decim.get());
                        new_action->stream_cmd.num_samps /= _decim.get();
                    }
                }

                RFNOC_LOG_INFO("Forwarding stream_cmd, num_samps is "
                               << new_action->stream_cmd.num_samps << ", id==" << new_action->id);
                post_action(dst_edge, new_action);
            });
    }

    std::string get_unique_id() const { return "MOCK_DDC"; }

    size_t get_num_input_ports() const
    {
        return 1;
    }

    size_t get_num_output_ports() const
    {
        return 1;
    }

    // Simplified coercer: Let's pretend like we can hit all even rates or 1
    // for all rates <= MAX_DECIM
    static int coerce_decim(const int requested_decim)
    {
        if (requested_decim <= 1) {
            return 1;
        }
        return std::min(requested_decim - (requested_decim % 2), MAX_DECIM);
    }


    // We make the properties global so we can inspect them, but that's not what
    // your supposed to do. However, we do keep the underscore notation, since that's
    // what they be called if they were in the class like they're supposed to.
    property_t<double> _samp_rate_in{
        "samp_rate", DEFAULT_RATE, {res_source_info::INPUT_EDGE}};
    property_t<double> _samp_rate_out{
        "samp_rate", DEFAULT_RATE, {res_source_info::OUTPUT_EDGE}};
    property_t<int> _decim{"decim", DEFAULT_DECIM, {res_source_info::USER}};

private:
    // This is where you normally put the properties
};


/*! FIFO
 *
 * Not much here -- we use it to test dynamic prop and action forwarding.
 */
class mock_fifo_t : public node_t
{
public:
    mock_fifo_t(const size_t num_ports) : _num_ports(num_ports)
    {
        set_prop_forwarding_policy(forwarding_policy_t::ONE_TO_ONE);
        set_action_forwarding_policy(forwarding_policy_t::ONE_TO_ONE);
    }

    std::string get_unique_id() const { return "MOCK_FIFO"; }

    size_t get_num_input_ports() const
    {
        return _num_ports;
    }

    size_t get_num_output_ports() const
    {
        return _num_ports;
    }


private:
    const size_t _num_ports;
};

/*! Streamer
 *
 * Not much here -- we use it to test dynamic prop and action forwarding.
 */
class mock_streamer_t : public node_t
{
public:
    mock_streamer_t(const size_t num_ports) : _num_ports(num_ports)
    {
        set_prop_forwarding_policy(forwarding_policy_t::DROP);
        set_action_forwarding_policy(forwarding_policy_t::DROP);
        register_property(&_samp_rate_user);
        register_property(&_samp_rate_in);
        add_property_resolver({&_samp_rate_user}, {&_samp_rate_in}, [this]() {
            UHD_LOG_INFO(get_unique_id(), "Calling resolver for `samp_rate_user'...");
            _samp_rate_in = _samp_rate_user.get();
        });
        add_property_resolver({&_samp_rate_in}, {}, [this]() {
            UHD_LOG_INFO(get_unique_id(), "Calling resolver for `samp_rate_in'...");
            // nop
        });
    }

    std::string get_unique_id() const
    {
        return "MOCK_STREAMER";
    }

    size_t get_num_input_ports() const
    {
        return _num_ports;
    }

    size_t get_num_output_ports() const
    {
        return _num_ports;
    }

    void issue_stream_cmd(uhd::stream_cmd_t stream_cmd, const size_t chan)
    {
        auto scmd =
            stream_cmd_action_info::make(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        scmd->stream_cmd = stream_cmd;
        post_action({res_source_info::INPUT_EDGE, chan}, scmd);
    }

private:
    property_t<double> _samp_rate_user{
        "samp_rate", 1e6, {res_source_info::USER}};
    property_t<double> _samp_rate_in{
        "samp_rate", 1e6, {res_source_info::INPUT_EDGE}};
    const size_t _num_ports;
};

/*! Terminator: Probe edge properties
 */
class mock_terminator_t : public node_t
{
public:
    static size_t counter;

    mock_terminator_t(const size_t num_ports)
        : _num_ports(num_ports), _term_count(counter++)
    {
        set_prop_forwarding_policy(forwarding_policy_t::DROP);
        set_action_forwarding_policy(forwarding_policy_t::DROP);
    }

    std::string get_unique_id() const
    {
        return "MOCK_TERMINATOR" + std::to_string(_term_count);
    }

    size_t get_num_input_ports() const
    {
        return _num_ports;
    }

    size_t get_num_output_ports() const
    {
        return _num_ports;
    }

    template <typename data_t>
    void set_edge_property(const std::string& id, data_t val, res_source_info edge_info)
    {
        UHD_ASSERT_THROW(edge_info.type == res_source_info::INPUT_EDGE
                         || edge_info.type == res_source_info::OUTPUT_EDGE);
        try {
            set_property<data_t>(id, val, edge_info);
        } catch (const uhd::lookup_error&) {
            node_accessor_t node_accessor{};
            auto edge_info_inverted = edge_info;
            edge_info_inverted.type = res_source_info::invert_edge(edge_info.type);
            property_t<data_t> new_prop(id, val, edge_info_inverted);
            node_accessor.forward_edge_property(this, edge_info.instance, &new_prop);
            set_property<data_t>(id, val, edge_info);
        }
    }

    template <typename data_t>
    data_t get_edge_property(const std::string& id, res_source_info edge_info)
    {
        UHD_ASSERT_THROW(edge_info.type == res_source_info::INPUT_EDGE
                         || edge_info.type == res_source_info::OUTPUT_EDGE);
        return get_property<data_t>(id, edge_info);
    }

private:
    const size_t _num_ports;
    const size_t _term_count;
};
size_t mock_terminator_t::counter = 0;

#endif /* INCLUDED_LIBUHD_TESTS_MOCK_NODES_HPP */
