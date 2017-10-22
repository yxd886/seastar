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
     // lw_shared_ptr<semaphore> _queue_space;
    // semaphore _queue_space = {212992};
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

        // _queue_space = make_lw_shared<semaphore>(212992);
        _queue_space = std::make_unique<semaphore>(212992);
    }

    ~port(){
        // auto queue_space_sptr = _queue_space;
        // engine().at_destroy([queue_space_sptr = std::move(queue_space_sptr)]{});
        _sendq.~circular_buffer();
        _qp.~unique_ptr();
        _queue_space.~unique_ptr();
    }

    port(const port& other) = delete;
    port(port&& other)  = delete;
    port& operator=(const port& other) = delete;
    port& operator=(port&& other) = delete;

    inline future<> send(net::packet p){
        assert(_qid < _dev->hw_queues_count());
        auto len = p.len();
        return _queue_space.wait(len).then([this, len, p = std::move(p)] () mutable {
            // auto qs = _queue_space;
            p = net::packet(std::move(p), make_deleter([qs = _queue_space.get(), len] { qs->signal(len); }));
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
