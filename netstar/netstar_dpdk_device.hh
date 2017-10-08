/*
 * netstar
 */

#ifdef HAVE_DPDK

#ifndef _NETSTAR_DPDK_DEVICE_HH
#define _NETSTAR_DPDK_DEVICE_HH

#include <memory>

namespace netstar{

class netstar_dpdk_device{
    int _private_field;
public:
    explicit netstar_dpdk_device();
};

std::unique_ptr<netstar_dpdk_device> create_netstar_dpdk_device();

} // namespace netstar

#endif // _NETSTAR_DPDK_DEVICE_HH

#endif // HAVE_DPDK
