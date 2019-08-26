//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef INCLUDED_LIBUHD_FFT_BLOCK_CONTROL_HPP
#define INCLUDED_LIBUHD_FFT_BLOCK_CONTROL_HPP

#include <uhd/config.hpp>
#include <uhd/rfnoc/noc_block_base.hpp>

namespace uhd { namespace rfnoc {

enum class FFT_SHIFT { NORMAL, REVERSE, NATURAL };
enum class FFT_DIRECTION { REVERSE, FORWARD };
enum class FFT_MAGNITUDE { COMPLEX, MAGNITUDE , MAGNITUDE_SQUARED };

/*! FFT Block Control Class
 */
class UHD_API fft_block_control : public noc_block_base
{
public:
    RFNOC_DECLARE_BLOCK(fft_block_control)

    // Readback addresses
    static const uint32_t RB_FFT_RESET;
    static const uint32_t RB_MAGNITUDE_OUT;
    static const uint32_t RB_FFT_SIZE_LOG2;
    static const uint32_t RB_FFT_DIRECTION;
    static const uint32_t RB_FFT_SCALING;
    static const uint32_t RB_FFT_SHIFT_CONFIG;
    // Write addresses
    static const uint32_t SR_FFT_RESET;
    static const uint32_t SR_FFT_SIZE_LOG2;
    static const uint32_t SR_MAGNITUDE_OUT;
    static const uint32_t SR_FFT_DIRECTION;
    static const uint32_t SR_FFT_SCALING;
    static const uint32_t SR_FFT_SHIFT_CONFIG;

    void reset();
};

}} // namespace uhd::rfnoc

#endif /* INCLUDED_LIBUHD_FFT_BLOCK_CONTROL_HPP */
