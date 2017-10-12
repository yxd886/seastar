#include "port.hh"

using namespace seastar;

namespace netstar{

inline future<> netstar_port::send(net::packet p){
    return _queues[engine().cpu_id()]->send(std::move(p));
}

} // namespace netstar
