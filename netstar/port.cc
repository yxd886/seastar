#include "port.hh"

using namespace seastar;

namespace netstar{

future<> create_ports(per_core_objs<netstar_port>* ports,
                      boost::program_options::variables_map& opts,
                      net::device* dev,
                      uint16_t dev_port_id){
    return ports->start(opts, dev, dev_port_id).then([ports, dev]{
        return dev->link_ready();
    });
}

} // namespace netstar
