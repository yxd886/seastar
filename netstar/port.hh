#ifndef _PORT_HH
#define _PORT_HH

#include <memory>

#include "net/net.hh"
#include "core/future.hh"
#include "core/stream.hh"
#include "net/proxy.hh"
#include "per_core_objs.hh"
#include "core/semaphore.hh"
#include "core/shared_ptr.hh"

using namespace seastar;

namespace netstar{

class port{
    unsigned _failed_send_count;
    uint16_t _qid;
    uint16_t _port_id;
    net::device* _dev;
    std::unique_ptr<net::qp> _qp;
    circular_buffer<net::packet> _sendq;
    std::unique_ptr<semaphore> _queue_space;
public:
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

        _queue_space = std::make_unique<semaphore>(212992);
    }

    ~port(){
        // Extend the life time of _queue_space.
        // When port is deconstructed, both _sendq and _qp contain some packets with a customized deletor like this:
        // [qs = _queue_space.get(), len] { qs->signal(len); }.
        // These packets will also be deconstructed when deconstructing the port.
        // Therefore we must ensure that _queue_space lives until the port is completely deconstructed.
        // What we have done here is to move the _queue_space into a fifo, that will be called later after the port.
        // Because the we use per_core_objs to construct the port, the port is actually placed in a position that
        // is closer to the head of the fifo. So we guarantee that _queue_space lives until port is fully deconstructed.
        // However, we do have to ensure that the port is constructed only by per_core_objs, otherwise this hack
        // doesn't work and abort seastar exit processs.
        // BTW: This hack saves about 100000pkts/s send rate, which I think to be important.
        engine().at_destroy([queue_space_sptr = std::move(_queue_space)]{});
    }

    port(const port& other) = delete;
    port(port&& other)  = delete;
    port& operator=(const port& other) = delete;
    port& operator=(port&& other) = delete;

    inline future<> send(net::packet p){
        assert(_qid < _dev->hw_queues_count());
        auto len = p.len();
        return _queue_space->wait(len).then([this, len, p = std::move(p)] () mutable {
            p = net::packet(std::move(p), make_deleter([qs = _queue_space.get(), len] { qs->signal(len); }));
            _sendq.push_back(std::move(p));
        });
    }
    inline future<> linearize_and_send(net::packet p){
        assert(_qid < _dev->hw_queues_count());
        auto len = p.len();
        return _queue_space->wait(len).then([this, len, p = std::move(p)] () mutable {
            p = net::packet(std::move(p), make_deleter([qs = _queue_space.get(), len] { qs->signal(len); }));
            p.linearize();
            _sendq.push_back(std::move(p));
        });
    }
    subscription<net::packet>
    receive(std::function<future<> (net::packet)> next_packet) {
        return _dev->receive(std::move(next_packet));
    }

    future<> stop(){
        return make_ready_future<>();
    }

    uint16_t port_id(){
        return _port_id;
    }

    net::ethernet_address get_eth_addr(){
        return _dev->hw_address();
    }
    const rss_key_type& get_rss_key(){
        return _dev->rss_key();
    }
};

class ports_env{
    std::vector<per_core_objs<port>> _ports_vec;
    std::vector<std::unique_ptr<net::device>> _devs_vec;
    std::vector<uint16_t> _port_ids_vec;
    std::vector<std::vector<bool>> _core_book_keeping;

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
        assert(port_check(opts, port_id));

        _ports_vec.emplace_back();
        _devs_vec.push_back(fn(port_id, queue_num));
        _port_ids_vec.push_back(port_id);
        _core_book_keeping.push_back(std::vector<bool>(smp::count, false));

        auto& ports = _ports_vec.back();
        auto dev  = _devs_vec.back().get();

        return ports.start(opts, dev, port_id).then([dev]{
            return dev->link_ready();
        });
    }
    per_core_objs<port>& get_ports(unsigned id){
        assert(id<_ports_vec.size());
        return std::ref(_ports_vec[id]);
    }
    size_t count(){
        return _ports_vec.size();
    }

    bool check_assigned_to_core(uint16_t port_id, uint16_t core_id){
        assert(port_id<_core_book_keeping.size() && core_id<smp::count);

        return _core_book_keeping[port_id][core_id];
    }
    void set_port_on_core(uint16_t port_id, uint16_t core_id){
        assert(port_id<_core_book_keeping.size() && core_id<smp::count);

        _core_book_keeping[port_id][core_id] = true;
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
