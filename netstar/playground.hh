#ifndef _PLAY_GROUND_HH
#define _PLAY_GROUND_HH

#include <memory>
#include <map>
#include <boost/program_options.hpp>

#include "netstar_dpdk_device.hh"
#include "net/proxy.hh"
#include "net/net.hh"
#include "core/reactor.hh"
#include "core/distributed.hh"
#include "core/apply.h"

using namespace seastar;

namespace netstar{

template<class Base>
class work_unit{
    Base* _work_unit_impl;
public:

    template<typename... Args>
    work_unit(Args&& args){
        std::unique_ptr<Base> ptr = std::make_unique<Base>(std::forward<Args>(args)...);
        _work_unit_impl = ptr.get();
        engine().at_destroy([ptr = std::move(ptr)](){});
    }

    Base* get_impl(){
        return _work_unit_impl;
    }
};

} // namespace netstar

#endif // _PLAY_GROUND_HH
