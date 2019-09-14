//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/types/endianness.hpp>
#include <uhd/utils/byteswap.hpp>
#include <uhdlib/rfnoc/chdr_packet.hpp>
#include <uhdlib/rfnoc/chdr_types.hpp>
#include <boost/format.hpp>
#include <boost/test/unit_test.hpp>
#include <iostream>

using namespace uhd;
using namespace uhd::rfnoc;
using namespace uhd::rfnoc::chdr;

constexpr size_t MAX_BUF_SIZE_BYTES = 1024;
constexpr size_t MAX_BUF_SIZE_WORDS = MAX_BUF_SIZE_BYTES / sizeof(uint64_t);
constexpr size_t NUM_ITERS          = 5000;

static const chdr_packet_factory chdr64_be_factory(CHDR_W_64, ENDIANNESS_BIG);
static const chdr_packet_factory chdr256_be_factory(CHDR_W_256, ENDIANNESS_BIG);
static const chdr_packet_factory chdr64_le_factory(CHDR_W_64, ENDIANNESS_LITTLE);
static const chdr_packet_factory chdr256_le_factory(CHDR_W_256, ENDIANNESS_LITTLE);

uint64_t rand64()
{
    return ((uint64_t)rand() << 32) | rand();
}

ctrl_payload populate_ctrl_payload()
{
    ctrl_payload pyld;
    pyld.dst_port    = rand64() & 0x03FF;
    pyld.src_port    = rand64() & 0x03FF;
    pyld.is_ack      = rand64() & 0x1;
    pyld.src_epid    = rand64() & 0xFFFF;
    pyld.data_vtr[0] = rand64() & 0xFFFFFFFF;
    pyld.byte_enable = rand64() & 0xF;
    pyld.op_code     = static_cast<ctrl_opcode_t>(rand64() % 8);
    pyld.status      = static_cast<ctrl_status_t>(rand64() % 4);
    if (rand64() % 2 == 0) {
        pyld.timestamp = rand64();
    } else {
        pyld.timestamp = boost::none;
    }
    return pyld;
}

strs_payload populate_strs_payload()
{
    strs_payload pyld;
    pyld.src_epid         = rand64() & 0xFFFF;
    pyld.status           = static_cast<strs_status_t>(rand64() % 4);
    pyld.capacity_bytes   = rand64() & 0xFFFFFFFFFF;
    pyld.capacity_pkts    = 0xFFFFFF;
    pyld.xfer_count_bytes = rand64();
    pyld.xfer_count_pkts  = rand64() & 0xFFFFFFFFFF;
    pyld.buff_info        = rand64() & 0xFFFF;
    pyld.status_info      = rand64() & 0xFFFFFFFFFFFF;
    return pyld;
}

strc_payload populate_strc_payload()
{
    strc_payload pyld;
    pyld.src_epid  = rand64() & 0xFFFF;
    pyld.op_code   = static_cast<strc_op_code_t>(rand64() % 3);
    pyld.op_data   = rand64() & 0xF;
    pyld.num_pkts  = rand64() & 0xFFFFFFFFFF;
    pyld.num_bytes = rand64();
    return pyld;
}

void byte_swap(uint64_t* buff)
{
    for (size_t i = 0; i < MAX_BUF_SIZE_WORDS; i++) {
        *(buff + i) = uhd::byteswap(*(buff + i));
    }
}

BOOST_AUTO_TEST_CASE(chdr_ctrl_packet_no_swap_64)
{
    uint64_t buff[MAX_BUF_SIZE_WORDS];

    chdr_ctrl_packet::uptr tx_pkt  = chdr64_be_factory.make_ctrl();
    chdr_ctrl_packet::cuptr rx_pkt = chdr64_be_factory.make_ctrl();

    for (size_t i = 0; i < NUM_ITERS; i++) {
        chdr_header hdr   = chdr_header(rand64());
        ctrl_payload pyld = populate_ctrl_payload();

        memset(buff, 0, MAX_BUF_SIZE_BYTES);
        tx_pkt->refresh(buff, hdr, pyld);
        BOOST_CHECK(tx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(tx_pkt->get_payload() == pyld);

        rx_pkt->refresh(buff);
        BOOST_CHECK(rx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(rx_pkt->get_payload() == pyld);

        std::cout << pyld.to_string();
    }
}

BOOST_AUTO_TEST_CASE(chdr_ctrl_packet_no_swap_256)
{
    uint64_t buff[MAX_BUF_SIZE_WORDS];

    chdr_ctrl_packet::uptr tx_pkt  = chdr256_be_factory.make_ctrl();
    chdr_ctrl_packet::cuptr rx_pkt = chdr256_be_factory.make_ctrl();

    for (size_t i = 0; i < NUM_ITERS; i++) {
        chdr_header hdr   = chdr_header(rand64());
        ctrl_payload pyld = populate_ctrl_payload();

        memset(buff, 0, MAX_BUF_SIZE_BYTES);
        tx_pkt->refresh(buff, hdr, pyld);
        BOOST_CHECK(tx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(tx_pkt->get_payload() == pyld);

        rx_pkt->refresh(buff);
        BOOST_CHECK(rx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(rx_pkt->get_payload() == pyld);
    }
}

BOOST_AUTO_TEST_CASE(chdr_ctrl_packet_swap_64)
{
    uint64_t buff[MAX_BUF_SIZE_WORDS];

    chdr_ctrl_packet::uptr tx_pkt  = chdr64_be_factory.make_ctrl();
    chdr_ctrl_packet::cuptr rx_pkt = chdr64_le_factory.make_ctrl();

    for (size_t i = 0; i < NUM_ITERS; i++) {
        chdr_header hdr   = chdr_header(rand64());
        ctrl_payload pyld = populate_ctrl_payload();

        memset(buff, 0, MAX_BUF_SIZE_BYTES);
        tx_pkt->refresh(buff, hdr, pyld);
        BOOST_CHECK(tx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(tx_pkt->get_payload() == pyld);

        byte_swap(buff);

        rx_pkt->refresh(buff);
        BOOST_CHECK(rx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(rx_pkt->get_payload() == pyld);
    }
}

BOOST_AUTO_TEST_CASE(chdr_ctrl_packet_swap_256)
{
    uint64_t buff[MAX_BUF_SIZE_WORDS];

    chdr_ctrl_packet::uptr tx_pkt  = chdr256_be_factory.make_ctrl();
    chdr_ctrl_packet::cuptr rx_pkt = chdr256_le_factory.make_ctrl();

    for (size_t i = 0; i < NUM_ITERS; i++) {
        chdr_header hdr   = chdr_header(rand64());
        ctrl_payload pyld = populate_ctrl_payload();

        memset(buff, 0, MAX_BUF_SIZE_BYTES);
        tx_pkt->refresh(buff, hdr, pyld);
        BOOST_CHECK(tx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(tx_pkt->get_payload() == pyld);

        byte_swap(buff);

        rx_pkt->refresh(buff);
        BOOST_CHECK(rx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(rx_pkt->get_payload() == pyld);
    }
}

BOOST_AUTO_TEST_CASE(chdr_strs_packet_no_swap_64)
{
    uint64_t buff[MAX_BUF_SIZE_WORDS];

    chdr_strs_packet::uptr tx_pkt  = chdr64_be_factory.make_strs();
    chdr_strs_packet::cuptr rx_pkt = chdr64_be_factory.make_strs();

    for (size_t i = 0; i < NUM_ITERS; i++) {
        chdr_header hdr   = chdr_header(rand64());
        strs_payload pyld = populate_strs_payload();

        memset(buff, 0, MAX_BUF_SIZE_BYTES);
        tx_pkt->refresh(buff, hdr, pyld);
        BOOST_CHECK(tx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(tx_pkt->get_payload() == pyld);

        rx_pkt->refresh(buff);
        BOOST_CHECK(rx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(rx_pkt->get_payload() == pyld);

        std::cout << pyld.to_string();
    }
}

BOOST_AUTO_TEST_CASE(chdr_strc_packet_no_swap_64)
{
    uint64_t buff[MAX_BUF_SIZE_WORDS];

    chdr_strc_packet::uptr tx_pkt  = chdr64_be_factory.make_strc();
    chdr_strc_packet::cuptr rx_pkt = chdr64_be_factory.make_strc();

    for (size_t i = 0; i < NUM_ITERS; i++) {
        chdr_header hdr   = chdr_header(rand64());
        strc_payload pyld = populate_strc_payload();

        memset(buff, 0, MAX_BUF_SIZE_BYTES);
        tx_pkt->refresh(buff, hdr, pyld);
        BOOST_CHECK(tx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(tx_pkt->get_payload() == pyld);

        rx_pkt->refresh(buff);
        BOOST_CHECK(rx_pkt->get_chdr_header() == hdr);
        BOOST_CHECK(rx_pkt->get_payload() == pyld);

        std::cout << pyld.to_string();
    }
}
