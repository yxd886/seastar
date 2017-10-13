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

struct tester{
    ~tester(){
        printf("Thread %d: tester object is destroyed\n", engine().cpu_id());
    }
    void call(int i){
        printf("Thread %d: test object 's call method is called with integer %d \n",
                engine().cpu_id(), i);
    }
};

template<class Base>
class work_unit{
    Base* _work_unit_impl;
public:

    template<typename... Args>
    work_unit(Args&&... args){
        std::unique_ptr<Base> ptr = std::make_unique<Base>(std::forward<Args>(args)...);
        _work_unit_impl = ptr.get();
        engine().at_destroy([ptr = std::move(ptr)](){});
    }

    Base* get_impl(){
        return _work_unit_impl;
    }

    future<> stop() {
        printf("Thread %d: work_unit object is destroyed\n", engine().cpu_id());
        return make_ready_future<>();
    }
};

/*
 * class work_unit{
 * public:
 *   future<> receive_packet(uint16_t port_id, packet pkt){
 *      if(port_id = 0){
 *          ports[1]->send(pkt);
 *          return make_ready_future<>();
 *      }
 *      if(port_id = 1){
 *          ports[0]->send(pkt);
 *          return make_ready_future<>();
 *      }
 *   }
 *   future<>
 * }
 *
 */

} // namespace netstar

#endif // _PLAY_GROUND_HH
