//
// Copyright 2014-2015 Ettus Research LLC
// Copyright 2018 Ettus Research, a National Instruments Company
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef INCLUDED_UHD_UTILS_CAST_HPP
#define INCLUDED_UHD_UTILS_CAST_HPP

#include <uhd/config.hpp>
#include <uhd/exception.hpp>
#include <sstream>
#include <string>

namespace uhd { namespace cast {
//! Convert a hexadecimal string into a value.
//
// Example:
//     uint16_t x = hexstr_cast<uint16_t>("0xDEADBEEF");
// Uses stringstream.
template <typename T>
inline T hexstr_cast(const std::string& in)
{
    T x;
    std::stringstream ss;
    ss << std::hex << in;
    ss >> x;
    return x;
}

//! Generic cast-from-string function
template <typename data_t>
data_t from_str(const std::string&)
{
    throw uhd::runtime_error("Cannot convert from string!");
}

template <>
UHD_API double from_str(const std::string& val);

template <>
UHD_API int from_str(const std::string& val);

template <>
UHD_API std::string from_str(const std::string& val);

}} // namespace uhd::cast

#endif /* INCLUDED_UHD_UTILS_CAST_HPP */
