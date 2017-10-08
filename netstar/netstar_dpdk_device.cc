/*
 * netstar
 */

#ifdef HAVE_DPDK

#include "netstar_dpdk_device.hh"

using namespace seastar;

namespace netstar{

class netstar_dpdk_device : public net::device{
    int _private_field;
public:
    explicit netstar_dpdk_device() :
    _private_field(0) {}
};

std::unique_ptr<net::device> create_netstar_dpdk_device(){
    return std::make_unique<netstar_dpdk_device>();
}

} // namespace netstar

#endif // HAVE_DPDK
