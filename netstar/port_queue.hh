#ifndef _PORT_QUEUE_HH
#define _PORT_QUEUE_HH

#include <vector>
#include <memory>

#include "net/net.hh"
#include "core/future.hh"
#include "core/stream.hh"
#include "net/proxy.hh"

using namespace seastar;

class netstar_queue{
    unsigned _failed_send_count;
    uint16_t _qid;
    net::device* _dev;
    std::unique_ptr<net::qp> _qp;
    circular_buffer<net::packet> _sendq;
public:
    static constexpr size_t max_sendq_length = 100;

    explicit netstar_queue(net::device* dev, uint16_t qid, boost::program_options::variables_map opts) :
        _failed_send_count(0),
        _qid(qid),
        _dev(dev){
        if(_qid < _dev->hw_queues_count()){
            _qp = _dev->init_local_queue(opts, _qid);

            std::map<unsigned, float> cpu_weights;
            for (unsigned i = _dev->hw_queues_count() + _qid % _dev->hw_queues_count(); i < smp::count; i+= _dev->hw_queues_count()) {
                cpu_weights[i] = 1;
            }
            cpu_weights[_qid] = opts["hw-queue-weight"].as<float>();
            _qp->configure_proxies(cpu_weights);

            _dev->update_local_queue(_qp.get());
        }
        else{
            auto master = _qid % _dev->hw_queues_count();
            _qp = create_proxy_net_device(master, _dev);

            _dev->update_local_queue(_qp.get());
        }

        if(_qid < _dev->hw_queues_count()){
            _qp->register_packet_provider([this](){
                std::experimental::optional<net::packet> p;
                if (!_sendq.empty()) {
                    p = std::move(_sendq.front());
                    _sendq.pop_front();
                }
                return p;
            });
        }
    }

    future<> send(net::packet p){
        if(_qid >= _dev->hw_queues_count()){
            printf("WARNING: Send packet from local port. But the \\"
                   "local port has no physical send queue. Silently\\"
                   " free the packets.\n");
            return make_ready_future<>();
        }
        if(_sendq.size() >= max_sendq_length){
            // _sendq is too long, siliently drop the packet
            _failed_send_count += 1;
            return make_ready_future<>();
        }
        else{
            _sendq.push_back(std::move(p));
            return make_ready_future<>();
        }
    }

    subscription<net::packet>
    receive(std::function<future<> (net::packet)> next_packet) {
        return _dev->receive(std::move(next_packet));
    }

    void compile();
};

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
