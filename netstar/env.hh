#ifndef _ENV_HH
#define _ENV_HH

#include "port.hh"

using namespace seastar;

namespace netstar{

class env{
    static std::vector<std::unique_ptr<netstar_port>> ports;

public:
    static future<> create_netstar_port(std::unique_ptr<net::device> device,
                                        boost::program_options::variables_map& opts);
};

} // namespace netstar

#endif // _ENV_HH
