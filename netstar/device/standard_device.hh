/*
 * netstar
 */

#ifdef HAVE_DPDK

#ifndef _STANDARD_DEVICE_HH
#define _STANDARD_DEVICE_HH

#include <memory>
#include "net/net.hh"

namespace netstar{

/*namespace standard_device {

template <bool HugetlbfsMemBackend>
class dpdk_qp;

}*/ // namespace standard_device

std::unique_ptr<seastar::net::device> create_standard_device(
                                    uint8_t port_idx = 0,
                                    uint8_t num_queues = 4,
                                    bool use_lro = false,
                                    bool enable_fc = false);
} // namespace netstar

#endif // _STANDARD_DEVICE_HH

#endif // HAVE_DPDK
