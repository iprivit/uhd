//
// Copyright 2017 Ettus Research, National Instruments Company
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "mpmd_link_if_ctrl_udp.hpp"
#include "mpmd_impl.hpp"
#include "mpmd_link_if_mgr.hpp"
#include <uhd/transport/udp_constants.hpp>
#include <uhd/transport/udp_simple.hpp>
#include <uhd/transport/udp_zero_copy.hpp>
#include <uhdlib/transport/inline_io_service.hpp>
#include <uhdlib/transport/udp_boost_asio_link.hpp>
#include <uhdlib/utils/narrow.hpp>
#include <string>

using namespace uhd;
using namespace uhd::transport;
using namespace uhd::mpmd::xport;

const uhd::rfnoc::chdr::chdr_packet_factory mpmd_link_if_ctrl_udp::_pkt_factory(
    uhd::rfnoc::CHDR_W_64, ENDIANNESS_BIG);

namespace {

//! Maximum CHDR packet size in bytes
const size_t MPMD_10GE_DATA_FRAME_MAX_SIZE = 8000;

//! Maximum CHDR packet size in bytes
const size_t MPMD_10GE_ASYNCMSG_FRAME_MAX_SIZE = 1472;

//! Number of send/recv frames
const size_t MPMD_ETH_NUM_FRAMES = 32;

//!
const double MPMD_BUFFER_DEPTH = 20.0e-3; // s
//! For MTU discovery, the time we wait for a packet before calling it
// oversized (seconds).
const double MPMD_MTU_DISCOVERY_TIMEOUT = 0.02;

// TODO: move these to appropriate header file for all other devices
const size_t MAX_RATE_1GIGE  = 1e9 / 8; // byte/s
const size_t MAX_RATE_10GIGE = 10e9 / 8; // byte/s


mpmd_link_if_ctrl_udp::udp_link_info_map get_udp_info_from_xport_info(
    const mpmd_link_if_mgr::xport_info_list_t& link_info_list)
{
    mpmd_link_if_ctrl_udp::udp_link_info_map result;
    for (const auto& link_info : link_info_list) {
        if (!link_info.count("ipv4")) {
            UHD_LOG_ERROR("MPMD::XPORT::UDP",
                "Invalid response from get_chdr_link_options()! No `ipv4' key!");
            throw uhd::runtime_error(
                "Invalid response from get_chdr_link_options()! No `ipv4' key!");
        }
        if (!link_info.count("port")) {
            UHD_LOG_ERROR("MPMD::XPORT::UDP",
                "Invalid response from get_chdr_link_options()! No `port' key!");
            throw uhd::runtime_error(
                "Invalid response from get_chdr_link_options()! No `port' key!");
        }
        const std::string udp_port = link_info.at("port");
        const size_t link_rate     = link_info.count("link_rate")
                                     ? std::stoul(link_info.at("link_rate"))
                                     : MAX_RATE_1GIGE;
        result.emplace(link_info.at("ipv4"),
            mpmd_link_if_ctrl_udp::udp_link_info_t{udp_port, link_rate});
    }

    return result;
}

std::vector<std::string> get_addrs_from_mb_args(const uhd::device_addr_t& mb_args,
    const mpmd_link_if_ctrl_udp::udp_link_info_map& link_info_list)
{
    // mb_args must always include addr
    if (not mb_args.has_key(FIRST_ADDR_KEY)) {
        UHD_LOG_WARNING("MPMD::XPORT::UDP",
            "The `" << FIRST_ADDR_KEY
                    << "' key must be specified in "
                       "device args to create an Ethernet transport to an RFNoC block");
        return {};
    }
    std::vector<std::string> addrs{mb_args[FIRST_ADDR_KEY]};
    if (mb_args.has_key(SECOND_ADDR_KEY)) {
        addrs.push_back(mb_args[SECOND_ADDR_KEY]);
    }
    // This is where in UHD we encode the knowledge about what
    // get_chdr_link_options() returns to us.
    for (const auto& ip_addr : addrs) {
        if (link_info_list.count(ip_addr)) {
            continue;
        }
        UHD_LOG_WARNING("MPMD::XPORT::UDP",
            "Cannot create UDP link to device: The IP address `"
                << ip_addr << "' is requested, but not reachable.");
        return {};
    }

    return addrs;
}

/*! Do a binary search to discover MTU
 *
 * Uses the MPM echo service to figure out MTU. We simply send a bunch of
 * packets and see if they come back until we converged on the path MTU.
 * The end result must lie between \p min_frame_size and \p max_frame_size.
 *
 * \param address IP address
 * \param port UDP port (yeah it's a string!)
 * \param min_frame_size Minimum frame size, initialize algorithm to start
 *                       with this value
 * \param max_frame_size Maximum frame size, initialize algorithm to start
 *                       with this value
 * \param echo_timeout Timeout value in seconds. For frame sizes that
 *                     exceed the MTU, we don't expect a response, and this
 *                     is the amount of time we'll wait before we assume
 *                     the frame size exceeds the MTU.
 */
size_t discover_mtu(const std::string& address,
    const std::string& port,
    size_t min_frame_size,
    size_t max_frame_size,
    const double echo_timeout = 0.020)
{
    const size_t echo_prefix_offset = uhd::mpmd::mpmd_impl::MPM_ECHO_CMD.size();
    const size_t mtu_hdr_len        = echo_prefix_offset + 10;
    UHD_ASSERT_THROW(min_frame_size < max_frame_size);
    UHD_ASSERT_THROW(min_frame_size % 4 == 0);
    UHD_ASSERT_THROW(max_frame_size % 4 == 0);
    UHD_ASSERT_THROW(min_frame_size >= echo_prefix_offset + mtu_hdr_len);
    using namespace uhd::transport;
    // The return port will probably differ from the discovery port, so we
    // need a "broadcast" UDP connection; using make_connected() would
    // drop packets
    udp_simple::sptr udp = udp_simple::make_broadcast(address, port);
    std::string send_buf(uhd::mpmd::mpmd_impl::MPM_ECHO_CMD);
    send_buf.resize(max_frame_size, '#');
    UHD_ASSERT_THROW(send_buf.size() == max_frame_size);
    std::vector<uint8_t> recv_buf;
    recv_buf.resize(max_frame_size, ' ');

    // Little helper to check returned packets match the sent ones
    auto require_bufs_match = [&recv_buf, &send_buf, mtu_hdr_len](const size_t len) {
        if (len < mtu_hdr_len
            or std::memcmp((void*)&recv_buf[0], (void*)&send_buf[0], mtu_hdr_len) != 0) {
            throw uhd::runtime_error("Unexpected content of MTU "
                                     "discovery return packet!");
        }
    };
    UHD_LOG_TRACE("MPMD", "Determining UDP MTU... ");
    size_t seq_no = 0;
    while (min_frame_size < max_frame_size) {
        // Only test multiples of 4 bytes!
        const size_t test_frame_size = (max_frame_size / 2 + min_frame_size / 2 + 3)
                                       & ~size_t(3);
        // Encode sequence number and current size in the string, makes it
        // easy to debug in code or Wireshark. Is also used for identifying
        // response packets.
        std::sprintf(
            &send_buf[echo_prefix_offset], ";%04lu,%04lu", seq_no++, test_frame_size);
        UHD_LOG_TRACE("MPMD", "Testing frame size " << test_frame_size);
        udp->send(boost::asio::buffer(&send_buf[0], test_frame_size));

        const size_t len = udp->recv(boost::asio::buffer(recv_buf), echo_timeout);
        if (len == 0) {
            // Nothing received, so this is probably too big
            max_frame_size = test_frame_size - 4;
        } else if (len >= test_frame_size) {
            // Size went through, so bump the minimum
            require_bufs_match(len);
            min_frame_size = test_frame_size;
        } else if (len < test_frame_size) {
            // This is an odd case. Something must have snipped the packet
            // on the way back. Still, we'll just back off and try
            // something smaller.
            UHD_LOG_DEBUG("MPMD", "Unexpected packet truncation during MTU discovery.");
            require_bufs_match(len);
            max_frame_size = len;
        }
    }
    UHD_LOG_DEBUG("MPMD", "Path MTU for address " << address << ": " << min_frame_size);
    return min_frame_size;
}

} // namespace


/******************************************************************************
 * Structors
 *****************************************************************************/
mpmd_link_if_ctrl_udp::mpmd_link_if_ctrl_udp(const uhd::device_addr_t& mb_args,
    const mpmd_link_if_mgr::xport_info_list_t& xport_info)
    : _mb_args(mb_args)
    , _recv_args(filter_args(mb_args, "recv"))
    , _send_args(filter_args(mb_args, "send"))
    , _udp_info(get_udp_info_from_xport_info(xport_info))
    , _mtu(MPMD_10GE_DATA_FRAME_MAX_SIZE)
{
    const std::string mpm_discovery_port = _mb_args.get(
        mpmd_impl::MPM_DISCOVERY_PORT_KEY, std::to_string(mpmd_impl::MPM_DISCOVERY_PORT));
    auto discover_mtu_for_ip = [mpm_discovery_port](const std::string& ip_addr) {
        return discover_mtu(ip_addr,
            mpm_discovery_port,
            IP_PROTOCOL_MIN_MTU_SIZE - IP_PROTOCOL_UDP_PLUS_IP_HEADER,
            MPMD_10GE_DATA_FRAME_MAX_SIZE,
            MPMD_MTU_DISCOVERY_TIMEOUT);
    };

    const std::vector<std::string> requested_addrs(
        get_addrs_from_mb_args(mb_args, _udp_info));
    for (const auto& ip_addr : requested_addrs) {
        try {
            // If MTU discovery fails, we gracefully recover, but declare that
            // link invalid.
            _mtu = std::min(_mtu, discover_mtu_for_ip(ip_addr));
            _available_addrs.push_back(ip_addr);
        } catch (const uhd::exception& ex) {
            UHD_LOG_WARNING("MPMD::XPORT::UDP",
                "Error during MTU discovery on address " << ip_addr << ": " << ex.what());
        }
    }
}

/******************************************************************************
 * API
 *****************************************************************************/
uhd::transport::both_links_t mpmd_link_if_ctrl_udp::get_link(const size_t link_idx,
    const uhd::transport::link_type_t /*link_type*/,
    const uhd::device_addr_t& /*link_args*/)
{
    UHD_ASSERT_THROW(link_idx < _available_addrs.size());
    const std::string ip_addr  = _available_addrs.at(link_idx);
    const std::string udp_port = _udp_info.at(ip_addr).udp_port;

    /* FIXME: Should have common infrastructure for creating I/O services */
    auto io_srv = uhd::transport::inline_io_service::make();
    link_params_t link_params;
    link_params.num_recv_frames = MPMD_ETH_NUM_FRAMES; // FIXME
    link_params.num_send_frames = MPMD_ETH_NUM_FRAMES; // FIXME
    link_params.recv_frame_size = get_mtu(uhd::RX_DIRECTION); // FIXME
    link_params.send_frame_size = get_mtu(uhd::TX_DIRECTION); // FIXME
    link_params.recv_buff_size  = MPMD_BUFFER_DEPTH * MAX_RATE_10GIGE; // FIXME
    link_params.send_buff_size  = MPMD_BUFFER_DEPTH * MAX_RATE_10GIGE; // FIXME
    auto link                   = uhd::transport::udp_boost_asio_link::make(ip_addr,
        udp_port,
        link_params,
        link_params.recv_buff_size, // FIXME
        link_params.send_buff_size); // FIXME
    io_srv->attach_send_link(link);
    io_srv->attach_recv_link(link);
    return std::tuple<io_service::sptr,
        send_link_if::sptr,
        size_t,
        recv_link_if::sptr,
        size_t,
        bool>(
        io_srv, link, link_params.send_buff_size, link, link_params.recv_buff_size, true);
}

size_t mpmd_link_if_ctrl_udp::get_num_links() const
{
    return _available_addrs.size();
}

//! Return the rate of the underlying link in bytes/sec
double mpmd_link_if_ctrl_udp::get_link_rate(const size_t link_idx) const
{
    UHD_ASSERT_THROW(link_idx < get_num_links());
    return _udp_info.at(_available_addrs.at(link_idx)).link_rate;
}

const uhd::rfnoc::chdr::chdr_packet_factory&
mpmd_link_if_ctrl_udp::get_packet_factory() const
{
    return _pkt_factory;
}
