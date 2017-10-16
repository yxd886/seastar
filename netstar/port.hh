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

class port{
    unsigned _failed_send_count;
    uint16_t _qid;
    uint16_t _port_id;
    net::device* _dev;
    std::unique_ptr<net::qp> _qp;
    circular_buffer<net::packet> _sendq;
public:
    static constexpr size_t max_sendq_length = 100;

    explicit port(boost::program_options::variables_map opts,
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

    ~port(){}
    port(const port& other) = delete;
    port(port&& other)  = delete;
    port& operator=(const port& other) = delete;
    port& operator=(port&& other) = delete;

    inline future<> send(net::packet p){
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

    future<> stop(){
        return make_ready_future<>();
    }
};

class ports_env{
    std::vector<per_core_objs<port>> _ports_vec;
    std::vector<std::unique_ptr<net::device>> _devs_vec;
    std::vector<uint16_t> _port_ids_vec;

public:
    explicit ports_env(){}
    ~ports_env(){}
    ports_env(const ports_env& other) = delete;
    ports_env(ports_env&& other)  = delete;
    ports_env& operator=(const ports_env& other) = delete;
    ports_env& operator=(ports_env&& other) = delete;

    future<> add_port(boost::program_options::variables_map& opts,
                      uint16_t port_id,
                      uint16_t queue_num,
                      std::function<std::unique_ptr<net::device>(uint16_t port_id,
                                                                 uint16_t queue_num)> fn){
        if(!port_check(opts, port_id)){
            return make_exception_future<>(std::runtime_error("Fail port check.\n"));
        }

        _ports_vec.emplace_back();
        _devs_vec.push_back(fn(port_id, queue_num));
        _port_ids_vec.push_back(port_id);

        auto ports = &(_ports_vec.back());
        auto dev  = _devs_vec.back().get();

        return ports->start(opts, dev, port_id).then([dev]{
            return dev->link_ready();
        });
    }

    per_core_objs<port>* get_ports(unsigned id){
        return &(_ports_vec.at(id));
    }

private:
    bool port_check(boost::program_options::variables_map& opts, uint16_t port_id){
        if(opts.count("network-stack") &&
           opts["network-stack"].as<std::string>() == "native" &&
           port_id == opts["dpdk-port-idx"].as<unsigned>()){
            return false;
        }
        for(auto id : _port_ids_vec){
            if(id == port_id){
                return false;
            }
        }
        return true;
    }
};

} // namespace netstar

#endif // _PORT_HH
