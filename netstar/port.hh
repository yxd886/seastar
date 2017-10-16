#ifndef _PORT_HH
#define _PORT_HH

#include <memory>

#include "net/net.hh"
#include "core/future.hh"
#include "core/stream.hh"
#include "net/proxy.hh"
#include "per_core_objs.hh"

using namespace seastar;

namespace netstar{

class netstar_port{
    unsigned _failed_send_count;
    uint16_t _qid;
    uint16_t _port_id;
    net::device* _dev;
    std::unique_ptr<net::qp> _qp;
    circular_buffer<net::packet> _sendq;
public:
    static constexpr size_t max_sendq_length = 100;

    explicit netstar_port(boost::program_options::variables_map opts,
                          net::device* dev,
                          uint16_t port_id) :
        _failed_send_count(0),
        _qid(engine().cpu_id()),
        _port_id(port_id),
        _dev(dev) {
        if(_qid < _dev->hw_queues_count()){
            _qp = _dev->init_local_queue(opts, _qid);

            std::map<unsigned, float> cpu_weights;
            for (unsigned i = _dev->hw_queues_count() + _qid % _dev->hw_queues_count();
                 i < smp::count;
                 i+= _dev->hw_queues_count()) {
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
        if(_qid >= _dev->hw_queues_count() ||
           _sendq.size() >= max_sendq_length){
            printf("WARNING: Send error! Siliently drop the packets\n");
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
};

future<> create_ports(per_core_objs<netstar_port>* ports,
                      boost::program_options::variables_map& opts,
                      net::device* dev,
                      uint16_t dev_port_id);

} // namespace netstar

#endif // _PORT_HH
