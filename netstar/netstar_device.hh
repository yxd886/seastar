#ifndef _NETSTAR_DEVICE_HH
#define _NETSTAR_DEVICE_HH

// netstar_device is a warper to

#include <memory>
#include <map>
#include <boost/program_options.hpp>

#include "netstar_dpdk_device.hh"
#include "net/proxy.hh"
#include "net/net.hh"
#include "core/reactor.hh"

using namespace seastar;

namespace netstar{

class netstar_qp_wraper {
    net::device* _dev;
    uint16_t _cpu_id;
    unsigned _send_count;
    unsigned _failed_send_count;
    std::unique_ptr<net::qp> _qp;
    std::shared_ptr<net::device> _dev_lifetime_holder;
    circular_buffer<net::packet> _sendq;
public:
    explicit netstar_qp_wraper(std::shared_ptr<net::device> sdev, boost::program_options::variables_map opts):
        _dev(sdev.get()),
        _cpu_id(engine().cpu_id()),
        _send_count(0),
        _failed_send_count(0){
        if(_cpu_id < _dev->hw_queues_count()){
            _qp = _dev->init_local_queue(opts, _cpu_id);

            std::map<unsigned, float> cpu_weights;
            for (unsigned i = _dev->hw_queues_count() + _cpu_id % _dev->hw_queues_count(); i < smp::count; i+= _dev->hw_queues_count()) {
                cpu_weights[i] = 1;
            }
            cpu_weights[_cpu_id] = opts["hw-queue-weight"].as<float>();
            _qp->configure_proxies(cpu_weights);

            _dev->update_local_queue(_qp.get());
        }
        else{
            auto master = _cpu_id % _dev->hw_queues_count();
            _qp = create_proxy_net_device(master, _dev);

            _dev->update_local_queue(_qp.get());
        }

        _dev_lifetime_holder = std::move(sdev);

        if(_cpu_id < _dev->hw_queues_count()){
            _qp->register_packet_provider([this](){

            });
        }
    }
    future<> send(net::packet p){
        if(_cpu_id >= _dev->hw_queues_count()){
            printf("WARNING: Send packet from local port. But the  \\"
                   "local port has no physical send queue. Silently free the packets.\n");
            return make_ready_future<>();
        }
        _local_qp->proxy_send(std::move(p));
        return make_ready_future<>();
    }
    future<> send_from_core(uint16_t dest_cpu_id, net::packet p){
        if(dest_cpu_id >= _dev->hw_queues_count()){
            printf("WARNING: Send packet from core %d. But the \\"
                    "local port has no physical send queue. Silently free the packets.\n",
                    dest_cpu_id);
            return make_ready_future<>();
        }
        if(dest_cpu_id == _cpu_id){
            _local_qp->proxy_send(std::move(p));
        }
        else{
            auto qp = &(_dev->queue_for_cpu(dest_cpu_id));
            auto cpu = _cpu_id;
            smp::submit_to(dest_cpu_id, [qp, cpu]() mutable {
                qp->proxy_send(p.free_on_cpu(cpu));
            });
        }
        return make_ready_future<>();
    }
    future<> unsafe_send_from_core(uint16_t dest_cpu_id, net::packet p){
        if(dest_cpu_id >= _dev->hw_queues_count()){
            printf("WARNING: Send packet from core %d. But the \\"
                   "local port has no physical send queue. Silently free the packets.\n",
                   dest_cpu_id);
            return make_ready_future<>();
        }
        if(dest_cpu_id == _cpu_id){
            _local_qp->proxy_send(std::move(p));
        }
        else{
            auto qp = &(_dev->queue_for_cpu(dest_cpu_id));
            smp::submit_to(dest_cpu_id, [qp]() mutable {
                qp->proxy_send(p);
            });
        }
        return make_ready_future<>();
    }
    future<>
};

} // namespace netstar

#endif // _NETSTAR_DEVICE_HH
