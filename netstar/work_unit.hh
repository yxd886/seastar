#ifndef _WORK_UNIT_HH
#define _WORK_UNIT_HH

#include <experimental/optional>
#include "core/future.hh"
#include "port_env.hh"

namespace netstar{

template<typename T>
class work_unit{
    using sub = subscription<net::packet>;
    using sub_option = std::experimental::optional<sub>;

    per_core_objs<T>* _all_objs;
    std::vector<port*> _all_ports;
    std::vector<sub_option> _all_subs;
    semaphore _forward_queue_length = {100};
protected:
    std::vector<port*>& ports(){
        return std::ref(_all_ports);
    }
    per_core_objs<T>* peers(){
        return _all_objs;
    }

    virtual future<> receive_from_port(uint16_t port_id, net::packet pkt) {
        printf("WARNING: Siliently drop the received packet.\n");
        return make_ready_future<>();
    }
    virtual void receive_forwarded(unsigned from_core, net::packet pkt) {
        printf("WARNING: Siliently drop the received forwarded packet.\n");
    }

    explicit work_unit(per_core_objs<T>* objs): _all_objs(objs) {}
public:
    void configure_ports(ports_env& env, unsigned first_pos, unsigned last_pos){
        assert(first_pos<=last_pos && _all_ports.size() == 0);
        for(auto i = first_pos; i<=last_pos; i++){
            auto& p = env.local_port(i);
            _all_ports.push_back(&p);
        }
        _all_subs.resize(_all_ports.size());
    }

    void configure_receive_fn(uint16_t port_id,
                           std::function<future<> (net::packet)> receive_fn){
        assert(port_id<_all_subs.size() && !_all_subs[port_id]);

        _all_subs[port_id].emplace(
            (*_all_ports.at(port_id)).receive(std::move(receive_fn))
        );
    }
    void configure_receive_fn_for_all_ports(){
        assert(_all_subs.size()>0 && _all_ports.size()==_all_subs.size());

        for(auto i=0; i<_all_subs.size(); i++){
            assert(!_all_subs[i]);
            _all_subs[i].emplace(
                _all_ports[i]->receive([this, i](net::packet pkt){
                    return receive_from_port(i, std::move(pkt));
                })
            );
        }
    }

    inline future<> send_from_port(uint16_t port_id, net::packet pkt){
        return (*_all_ports.at(port_id)).send(std::move(pkt));
    }
    inline future<> forward_to(unsigned dst_core, net::packet pkt){
        return _forward_queue_length.wait(1).then([this, dst_core, pkt = std::move(pkt)] () mutable{
           auto src_core = engine().cpu_id();
           auto& peer = _all_objs->get_obj(dst_core);

           smp::submit_to(dst_core, [this, &peer, src_core, pkt = std::move(pkt)] () mutable {
               peer.receive_forwarded(src_core, pkt.free_on_cpu(src_core, []{}));
           }).then([this]{
               _forward_queue_length.signal(1);
           });
        });
    }
};

} // namespace netstar

#endif
