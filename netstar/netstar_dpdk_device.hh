/*
 * netstar
 */

#ifdef HAVE_DPDK

#ifndef _NETSTAR_DPDK_DEVICE_HH
#define _NETSTAR_DPDK_DEVICE_HH

#include <memory>
#include "net/net.hh"
#include "core/sstring.hh"

namespace netstar{

std::unique_ptr<seastar::net::device> create_netstar_dpdk_device();

} // namespace netstar

#endif // _NETSTAR_DPDK_DEVICE_HH

#endif // HAVE_DPDK
