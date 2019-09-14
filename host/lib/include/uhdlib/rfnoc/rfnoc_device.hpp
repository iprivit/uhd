//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef INCLUDED_LIBUHD_RFNOC_DEVICE_HPP
#define INCLUDED_LIBUHD_RFNOC_DEVICE_HPP

#include <uhd/device.hpp>
#include <uhd/exception.hpp>
#include <uhd/rfnoc/mb_controller.hpp>
#include <uhdlib/rfnoc/client_zero.hpp>
#include <uhdlib/rfnoc/mb_iface.hpp>
#include <boost/shared_ptr.hpp>

namespace uhd { namespace rfnoc { namespace detail {

/*! Extends uhd::device with features required to operate in an rfnoc_graph
 */
class rfnoc_device : public uhd::device
{
public:
    using sptr = boost::shared_ptr<rfnoc_device>; // FIXME make std::shared_ptr when
                                                  // uhd::device is ready

    rfnoc_device()
    {
        _type = uhd::device::USRP;
        _tree = uhd::property_tree::make();
    }

    /*! Return a reference to the mb_iface for a given motherboard
     */
    virtual uhd::rfnoc::mb_iface& get_mb_iface(const size_t mb_idx) = 0;

    //! Return a reference to an MB controller
    mb_controller::sptr get_mb_controller(const size_t mb_idx) const
    {
        return _mbc_registry.at(mb_idx);
    }

    //! Directly getting a streamer no longer supported
    uhd::rx_streamer::sptr get_rx_stream(const stream_args_t&)
    {
        UHD_THROW_INVALID_CODE_PATH();
    }

    //! Directly getting a streamer no longer supported
    uhd::tx_streamer::sptr get_tx_stream(const stream_args_t&)
    {
        UHD_THROW_INVALID_CODE_PATH();
    }

    //! Directly getting async messages no longer supported
    bool recv_async_msg(uhd::async_metadata_t&, double)
    {
        UHD_THROW_INVALID_CODE_PATH();
    }


protected:
    void register_mb_controller(const size_t mb_idx, mb_controller::sptr mbc)
    {
        _mbc_registry.emplace(mb_idx, mbc);
    }

private:
    std::unordered_map<size_t, mb_controller::sptr> _mbc_registry;
}; // class rfnoc_device

}}} // namespace uhd::rfnoc::detail

#endif /* INCLUDED_LIBUHD_RFNOC_DEVICE_HPP */
