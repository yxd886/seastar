#ifndef _WORK_UNIT_HH
#define _WORK_UNIT_HH

#include <experimental/optional>
#include "core/future.hh"
#include "port.hh"

namespace netstar{

template<typename T>
class work_unit{
    static_assert(std::is_base_of<work_unit<T>, T>::value,
                  "T does not inherit from work_unit<T>\n");
    using sub = subscription<net::packet>;
    using optional = std::experimental::optional;

    bool _receive_fn_configured;
    per_core_objs<T>* _all_objs;
    std::vector<port*> _all_ports;
    std::vector<optional<sub>> _all_subs;
protected:
    std::vector<port*>& ports(){
        return std::ref(_all_ports);
    }
    per_core_objs<T>* objs(){
        return _all_objs;
    }

    virtual future<> receive_from_port(uint16_t port_id, net::packet pkt) = 0;
    virtual future<> receive_forwarded(unsigned from_core, net::packet pkt) = 0;
public:
    explicit work_unit(per_core_objs<T*>* objs):
        _receive_fn_configured (false),
        _all_objs(objs) {}

    void configure_ports(ports_env& env, unsigned first_pos, unsigned last_pos){
        assert(first_pos<last_pos &&
               last_pos<env.count() &&
               _all_ports.size() == 0);
        for(auto i = first_pos; i<=last_pos; i++){
            assert(!env.check_assigned_to_core(i, engine().cpu_id()));
            auto& ports = env.get_ports(i);
            _all_ports.push_back(&ports.local_obj());
            env.set_port_on_core(i, engine().cpu_id());
        }
    }

    void configure_receive_fn(uint16_t port_id,
                           std::function<future<> (net::packet)> receive_fn){
        (*_all_ports.at(port_id)).receive(std::move(receive_fn));
    }

    void configure_receive_fn_for_all_ports(){
        if(_receive_fn_configured)
        for(auto port : _all_ports){
            auto port_id = (*port).port_id();
            (*port).receive([this, port_id](net::packet pkt){
                return receive_from_port(port_id, std::move(pkt));
            });
        }
    }

    inline void send_from_port(uint16_t port_id, net::packet pkt){
        (*_all_ports.at(port_id)).send(std::move(pkt));
    }

    /*template<typename T>
    inline void forward_to(per_core_objs<T>* work_units){
        static_assert(std::is_base_of<work_unit, B>::value)
    }*/
};

} // namespace netstar

#endif
