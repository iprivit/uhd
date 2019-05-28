//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/types/time_spec.hpp>
#include <uhdlib/rfnoc/client_zero.hpp>
#include <uhdlib/utils/narrow.hpp>
#include <string>
#include <thread>

using namespace uhd::rfnoc;
using namespace uhd::rfnoc::detail;
using namespace std::chrono_literals;

namespace {
constexpr std::chrono::milliseconds DEFAULT_POLL_TIMEOUT = 1000ms;
constexpr std::chrono::milliseconds DEFAULT_POLL_PERIOD  = 10ms;
constexpr uint32_t DEFAULT_FLUSH_TIMEOUT = 100; // num cycles (hardware-timed)

// Read Register Addresses
//! Register address of the protocol version
constexpr int PROTOVER_ADDR = 0 * 4;
//! Register address of the port information
constexpr int PORT_CNT_ADDR = 1 * 4;
//! Register address of the edge information
constexpr int EDGE_CNT_ADDR = 2 * 4;
//! Register address of the device information
constexpr int DEVICE_INFO_ADDR = 3 * 4;
//! (Write) Register address of the flush and reset controls
constexpr int FLUSH_RESET_ADDR = 1 * 4;

//! Base address of the adjacency list
constexpr size_t ADJACENCY_BASE_ADDR = 0x10000;
//! Each port is allocated this many registers in the backend register space
constexpr size_t REGS_PER_PORT = 16;
} // namespace

client_zero::client_zero(register_iface::sptr reg)
    : uhd::rfnoc::register_iface_holder(reg)
{
    // The info we need is static, so we can read it all up front, and store the
    // parsed information.
    const uint32_t proto_reg_val       = regs().peek32(PROTOVER_ADDR);
    const uint32_t port_reg_val        = regs().peek32(PORT_CNT_ADDR);
    const uint32_t edge_reg_val        = regs().peek32(EDGE_CNT_ADDR);
    const uint32_t device_info_reg_val = regs().peek32(DEVICE_INFO_ADDR);

    // Parse the PROTOVER_ADDR register
    _proto_ver = proto_reg_val & 0xFFFF;

    // Parse the PORT_CNT_ADDR register
    _has_chdr_crossbar    = bool(port_reg_val & (1 << 31));
    _num_transports       = uhd::narrow_cast<uint16_t>((port_reg_val & 0x3FF00000) >> 20);
    _num_blocks           = uhd::narrow_cast<uint16_t>((port_reg_val & 0x000FFC00) >> 10);
    _num_stream_endpoints = uhd::narrow_cast<uint16_t>((port_reg_val & 0x000003FF));

    // Parse the EDGE_CNT_ADDR register
    // The only non-zero entry here is _num_edges
    _num_edges = edge_reg_val;

    // Parse the DEVICE_INFO_ADDR register
    _device_type = (device_info_reg_val & 0xFFFF0000) >> 16;

    // Read the adjacency list
    _adjacency_list = _get_adjacency_list();

    // Set the default flushing timeout for each block
    for (uint16_t portno = 1 + get_num_stream_endpoints();
         portno < (get_num_blocks() + get_num_stream_endpoints());
         ++portno) {
        set_flush_timeout(DEFAULT_FLUSH_TIMEOUT, portno);
    }
}

//! Helper function to read the adjacency list
std::vector<client_zero::edge_def_t> client_zero::_get_adjacency_list()
{
    // Read the header, which includes the number of entries in the list
    size_t num_entries = regs().peek32(ADJACENCY_BASE_ADDR) & 0x3FFF;

    // Construct the adjacency list by iterating through and reading each entry
    std::vector<edge_def_t> adj_list;
    adj_list.reserve(num_entries);

    // The first entry is at offset 1
    auto edge_reg_vals = regs().block_peek32(ADJACENCY_BASE_ADDR + 4, num_entries);

    // Unpack the struct
    // Note: we construct the adjacency list with empty block IDs. We'll fill them in
    //       when we make the block controllers
    for (uint32_t edge_reg_val : edge_reg_vals) {
        adj_list.push_back({uhd::narrow_cast<uint16_t>((edge_reg_val & 0xFFC00000) >> 22),
            uhd::narrow_cast<uint8_t>((edge_reg_val & 0x003F0000) >> 16),
            uhd::narrow_cast<uint16_t>((edge_reg_val & 0x0000FFC0) >> 6),
            uhd::narrow_cast<uint8_t>((edge_reg_val & 0x0000003F) >> 0)});
    }
    return adj_list;
}

uint32_t client_zero::get_noc_id(uint16_t portno)
{
    _check_port_number(portno);
    // The NOC ID is the second entry in the port's register space
    return regs().peek32(_get_port_base_addr(portno) + 4);
}

bool client_zero::get_flush_active(uint16_t portno)
{
    // The flush active flag is in the 0th (bottom) bit
    return bool(_get_flush_status_flags(portno) & 1);
}

bool client_zero::get_flush_done(uint16_t portno)
{
    // The flush done flag is in the 1st bit
    return bool(_get_flush_status_flags(portno) & (1 << 1));
}

bool client_zero::poll_flush_done(
    uint16_t portno, std::chrono::milliseconds timeout = DEFAULT_POLL_TIMEOUT)
{
    _check_port_number(portno);
    auto start = std::chrono::steady_clock::now();
    while (!get_flush_done(portno)) {
        if (std::chrono::steady_clock::now() > (start + timeout)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(DEFAULT_POLL_PERIOD));
    }
    return true;
}

void client_zero::set_flush_timeout(uint32_t timeout, uint16_t portno)
{
    _check_port_number(portno);
    // The flush timeout register is the first write register
    regs().poke32(_get_port_base_addr(portno), timeout);
}

void client_zero::set_flush(uint16_t portno)
{
    _check_port_number(portno);
    // The flush and reset registers are the second write register
    regs().poke32(
        _get_port_base_addr(portno) + FLUSH_RESET_ADDR, 1 /* 0th (bottom) bit */);
}

bool client_zero::complete_flush(uint16_t portno)
{
    _check_port_number(portno);
    set_flush(portno);
    return poll_flush_done(portno);
}

void client_zero::reset_ctrl(uint16_t portno)
{
    _check_port_number(portno);
    // The flush and reset registers are the second write register
    regs().poke32(_get_port_base_addr(portno) + FLUSH_RESET_ADDR, (1 << 1) /* 1st bit */);
    std::this_thread::sleep_for(100us);
    regs().poke32(_get_port_base_addr(portno) + FLUSH_RESET_ADDR, (1 << 1));
}

void client_zero::reset_chdr(uint16_t portno)
{
    _check_port_number(portno);
    // The flush and reset registers are the second write register
    regs().poke32(_get_port_base_addr(portno) + FLUSH_RESET_ADDR, (1 << 2) /* 2nd bit */);
    std::this_thread::sleep_for(1ms);
    regs().poke32(_get_port_base_addr(portno) + FLUSH_RESET_ADDR, (1 << 2));
}

client_zero::block_config_info client_zero::get_block_info(uint16_t portno)
{
    _check_port_number(portno);
    // The block and ctrl information is in the port's first register
    uint32_t config_reg_val = regs().peek32(_get_port_base_addr(portno) + 0);
    // The block and ctrl information is in the port's third register
    uint32_t data_reg_val = regs().peek32(_get_port_base_addr(portno) + 8);
    return {uhd::narrow_cast<uint8_t>((config_reg_val & 0x0000003F) >> 0),
        uhd::narrow_cast<uint8_t>((config_reg_val & 0x00000FC0) >> 6),
        uhd::narrow_cast<uint8_t>((config_reg_val & 0x0003F000) >> 12),
        uhd::narrow_cast<uint8_t>((config_reg_val & 0x00FC0000) >> 18),
        uhd::narrow_cast<uint8_t>((config_reg_val & 0xFF000000) >> 24),
        uhd::narrow_cast<uint8_t>((data_reg_val & 0x000000FC) >> 2)};
}

uint32_t client_zero::_get_port_base_addr(uint16_t portno)
{
    return REGS_PER_PORT * portno * 4;
}

void client_zero::_check_port_number(uint16_t portno)
{
    auto num_ports = get_num_blocks() + get_num_stream_endpoints() + 1;

    if (portno >= num_ports) {
        throw uhd::index_error(
            std::string("Client zero attempted to query unconnected port: ")
            + std::to_string(portno));
    } else if (portno <= get_num_stream_endpoints()) {
        throw uhd::index_error(
            std::string("Client zero attempted to query stream endpoint: ")
            + std::to_string(portno));
    }
}

uint32_t client_zero::_get_flush_status_flags(uint16_t portno)
{
    _check_port_number(portno);
    // The flush status flags are in the third register of the port
    return regs().peek32(_get_port_base_addr(portno) + 8);
}

client_zero::sptr client_zero::make(chdr_ctrl_endpoint& chdr_ctrl_ep, sep_id_t dst_epid)
{
    // Create a control port endpoint for client zero
    static constexpr uint16_t CLIENT_ZERO_PORT         = 0;
    static constexpr size_t CLIENT_ZERO_BUFF_CAPACITY  = 32;
    static constexpr size_t CLIENT_ZERO_MAX_ASYNC_MSGS = 0;
    static clock_iface client_zero_clk{"client_zero"};
    client_zero_clk.set_running(true); // Client zero clock must be always-on.
    client_zero_clk.set_freq(100e6); // The freq is unused. No timed ops or sleeps.

    return std::make_shared<client_zero>(chdr_ctrl_ep.get_ctrlport_ep(dst_epid,
        CLIENT_ZERO_PORT,
        CLIENT_ZERO_BUFF_CAPACITY,
        CLIENT_ZERO_MAX_ASYNC_MSGS,
        client_zero_clk,
        client_zero_clk));
}
