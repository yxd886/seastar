#ifdef HAVE_DPDK

#ifndef _FDIR_DEVICE_HH
#define _FDIR_DEVICE_HH

#include <memory>
#include "net/net.hh"
#include "core/sstring.hh"

namespace netstar{

std::unique_ptr<seastar::net::device> create_fdir_device(
                                        uint8_t port_idx = 0,
                                        uint8_t num_queues = seastar::smp::count,
                                        bool use_lro = false,
                                        bool enable_fc = false);
} // namespace netstar

#endif // _NETSTAR_DPDK_DEVICE_HH

#endif // HAVE_DPDK
