//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef INCLUDED_UHD_TRANSPORT_UDP_BOOST_ASIO_LINK_HPP
#define INCLUDED_UHD_TRANSPORT_UDP_BOOST_ASIO_LINK_HPP

#include <uhd/config.hpp>
#include <uhd/transport/buffer_pool.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhdlib/transport/link_base.hpp>
#include <uhdlib/transport/links.hpp>
#include <uhdlib/transport/udp_common.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <vector>

namespace uhd { namespace transport {

class udp_boost_asio_frame_buff : public frame_buff
{
public:
    udp_boost_asio_frame_buff(void* mem)
    {
        _data = mem;
    }
};

class udp_boost_asio_link : public recv_link_base<udp_boost_asio_link>,
                            public send_link_base<udp_boost_asio_link>
{
public:
    using sptr = std::shared_ptr<udp_boost_asio_link>;

    /*!
     * Make a new udp link.
     *
     * \param addr a string representing the destination address
     * \param port a string representing the destination port
     * \param params Values for frame sizes, num frames, and buffer sizes
     * \param[out] recv_socket_buff_size Returns the recv socket buffer size
     * \param[out] send_socket_buff_size Returns the send socket buffer size
     */
    static sptr make(const std::string& addr,
        const std::string& port,
        const link_params_t& params,
        size_t& recv_socket_buff_size,
        size_t& send_socket_buff_size);

    /*! Return the local port of the UDP connection. Port is in host byte order.
     *
     * \returns Port number or 0 if port number couldn't be identified.
     */
    uint16_t get_local_port() const;

    /*! Return the local IP address of the UDP connection as a dotted string.
     *
     * \returns IP address as a string or empty string if the IP address could
     *          not be identified.
     */
    std::string get_local_addr() const;

private:
    using recv_link_base_t = recv_link_base<udp_boost_asio_link>;
    using send_link_base_t = send_link_base<udp_boost_asio_link>;

    // Friend declarations to allow base classes to call private methods
    friend recv_link_base_t;
    friend send_link_base_t;

    udp_boost_asio_link(
        const std::string& addr, const std::string& port, const link_params_t& params);

    size_t resize_recv_socket_buffer(size_t num_bytes);
    size_t resize_send_socket_buffer(size_t num_bytes);

    // Methods called by recv_link_base
    UHD_FORCE_INLINE size_t get_recv_buff_derived(frame_buff& buff, int32_t timeout_ms)
    {
        return recv_udp_packet(_sock_fd, buff.data(), get_recv_frame_size(), timeout_ms);
    }

    UHD_FORCE_INLINE void release_recv_buff_derived(frame_buff& /*buff*/)
    {
        // No-op
    }

    // Methods called by send_link_base
    UHD_FORCE_INLINE bool get_send_buff_derived(
        frame_buff& /*buff*/, int32_t /*timeout_ms*/)
    {
        return true;
    }

    UHD_FORCE_INLINE void release_send_buff_derived(frame_buff& buff)
    {
        send_udp_packet(_sock_fd, buff.data(), buff.packet_size());
    }

    buffer_pool::sptr _recv_memory_pool;
    buffer_pool::sptr _send_memory_pool;

    std::vector<udp_boost_asio_frame_buff> _recv_buffs;
    std::vector<udp_boost_asio_frame_buff> _send_buffs;

    boost::asio::io_service _io_service;
    std::shared_ptr<boost::asio::ip::udp::socket> _socket;
    int _sock_fd;
};

}} // namespace uhd::transport

#endif /* INCLUDED_UHD_TRANSPORT_UDP_BOOST_ASIO_LINK_HPP */
