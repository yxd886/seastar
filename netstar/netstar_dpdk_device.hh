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

std::unique_ptr<seastar::net::device> create_netstar_dpdk_net_device(
                                    uint8_t port_idx = 0,
                                    uint8_t num_queues = 4,
                                    bool use_lro = false,
                                    bool enable_fc = false);
} // namespace netstar

#endif // _NETSTAR_DPDK_DEVICE_HH

#endif // HAVE_DPDK
