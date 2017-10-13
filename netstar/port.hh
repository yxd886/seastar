#ifndef _PORT_HH
#define _PORT_HH

#include <memory>

#include "queue.hh"

using namespace seastar;

namespace netstar{

class netstar_port{
    std::unique_ptr<net::device> _device;
    std::unique_ptr<netstar_queue*[]> _queues;
public:
    explicit netstar_port(std::unique_ptr<net::device> device) :
        _device(std::move(device)) {
        _queues = std::make_unique<netstar_queue*[]>(smp::count);
    }

    void init_queue(unsigned qid, boost::program_options::variables_map opts){
        net::device* dev = _device.get();
        auto queue = std::make_unique<netstar_queue>(dev, qid, opts);
        _queues[qid] = queue.get();
        engine().at_destroy([queue = std::move(queue)] {});
    }

    future<> send(net::packet p);

    subscription<net::packet>
    receive(std::function<future<> (net::packet)> next_packet) {
        return _queues[engine().cpu_id()]->receive(std::move(next_packet));
    }

    future<> link_ready(){
        return _device->link_ready();
    }
};

namespace env{

static std::vector<std::unique_ptr<netstar_port>> ports;

static future<> create_netstar_port(std::unique_ptr<net::device> device,
                                    boost::program_options::variables_map& opts);

} // namespace env

} // namespace netstar

#endif // _PORT_HH
