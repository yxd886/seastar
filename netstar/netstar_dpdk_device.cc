#ifdef HAVE_DPDK

#include "netstar_dpdk_device.hh"

namespace netstar{

netstar_dpdk_device::netstar_dpdk_device() :
        _private_field(0) {
}

std::unique_ptr<netstar_dpdk_device> create_netstar_dpdk_device(){
    return std::make_unique<netstar_dpdk_device>();
}

} // namespace netstar

#endif // HAVE_DPDK
