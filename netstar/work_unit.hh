#ifndef _WORK_UNIT_HH
#define _WORK_UNIT_HH

#include "port.hh"

namespace netstar{

class work_unit{
    std::vector<per_core_objs<port>*> _controlled_ports;
public:
    void receive_from(per_core_objs<port>* ports){

        // ports->local_obj()->receive()
    }
};

} // namespace netstar

#endif
