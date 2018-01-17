/*
 * netstar
 */

#ifdef HAVE_DPDK

#ifndef _FIDR_DEVICE_HH
#define _FIDR_DEVICE_HH

#include <memory>
#include "net/net.hh"

namespace netstar{

namespace fdir_device {

template <bool HugetlbfsMemBackend>
class dpdk_qp;

} // fdir_device

std::unique_ptr<seastar::net::device> create_fdir_device(
                                    uint8_t port_idx = 0,
                                    uint8_t num_queues = 4,
                                    bool use_lro = false,
                                    bool enable_fc = false);
} // namespace netstar

#endif // _FIDR_DEVICE_HH

#endif // HAVE_DPDK
