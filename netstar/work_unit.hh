#ifndef _WORK_UNIT_HH
#define _WORK_UNIT_HH

#include "port.hh"

namespace netstar{

class work_unit{
    std::vector<port*> _all_ports;
protected:
    virtual future<> receive_from_port(uint16_t port_id, net::packet pkt) = 0;
public:
    void add_port(per_core_objs<port>& ports){
        auto local_port = ports.local_obj();
        _all_ports.push_back(&local_port);
        auto port_id = _all_ports.size()-1;

        local_port.receive([this, port_id](net::packet pkt){
            return receive_from_port(port_id, std::move(pkt))();
        });
        // ports->local_obj()->receive()
    }
};

} // namespace netstar

#endif
