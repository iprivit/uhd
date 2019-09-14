//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef INCLUDED_LIBUHD_DIRTIFYIER_HPP
#define INCLUDED_LIBUHD_DIRTIFYIER_HPP

#include <uhd/rfnoc/property.hpp>

namespace uhd { namespace rfnoc {

/*! This is a special class for property that is always dirty. This is useful
 * to force property resolutions in certain cases.
 *
 * Note: This has nothing to do with 'dirtify' in the CGI/graphics sense.
 */
class dirtifier_t : public property_base_t
{
public:
    dirtifier_t()
        : property_base_t("__ALWAYS_DIRTY__", res_source_info(res_source_info::FRAMEWORK))
    {
        // nop
    }

    //! This property is always dirty
    bool is_dirty() const
    {
        return true;
    }

    //! This property is always invalid
    bool is_valid() const
    {
        return false;
    }

    //! This property is never equal to anything else
    bool equal(property_base_t*) const
    {
        return false;
    }

    //! Always dirty, so this can be called as often as we like
    void force_dirty() {}

private:
    //! This property cannot be marked clean, but nothing happens if you try
    void mark_clean() {}

    //! The value from this property cannot be forwarded
    void forward(property_base_t*)
    {
        throw uhd::type_error("Cannot forward to or from dirtifier property!");
    }

    //! This property never has the same type as another type
    bool is_type_equal(property_base_t*) const
    {
        return false;
    }
};

}} /* namespace uhd::rfnoc */

#endif /* INCLUDED_LIBUHD_DIRTIFYIER_HPP */
