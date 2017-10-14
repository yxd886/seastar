#include "port.hh"

using namespace seastar;

namespace netstar{

inline future<> netstar_port::send(net::packet p){
    return _queues[engine().cpu_id()]->send(std::move(p));
}

std::vector<std::unique_ptr<netstar_port>> port_env::ports;

future<> port_env::create_netstar_port(std::unique_ptr<net::device> device,
                                  boost::program_options::variables_map& opts){
    ports.push_back(std::make_unique<netstar_port>(std::move(device)));

    auto pport = ports.back().get();
    auto sem = std::make_shared<semaphore>(0);

    for (unsigned i = 0; i < smp::count; i++) {
       smp::submit_to(i, [opts, pport] {
           uint16_t qid = engine().cpu_id();
           pport->init_queue(qid, opts);
       }).then([sem] {
           sem->signal();
       });
    }

    return sem->wait(smp::count).then([pport] {
        return pport->link_ready();
    });
}

} // namespace netstar
