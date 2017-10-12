#ifndef _PORT_QUEUE_HH
#define _PORT_QUEUE_HH

#include <vector>
#include <memory>

#include "net/net.hh"
#include "core/future.hh"
#include "core/stream.hh"

using namespace seastar;

class netstar_queue;

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

    future<> send(net::packet p){
        return _queues[engine().cpu_id()]->send(std::move(p));
    }

    subscription<net::packet>
    receive(std::function<future<> (net::packet)> next_packet) {
        return _queues[engine().cpu_id()]->receive(std::move(next_packet));
    }

    future<> link_ready(){
        return _device->link_ready();
    }
};

#endif // _PORT_QUEUE_HH
