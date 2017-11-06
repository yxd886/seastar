#ifndef _STACK_PORT_HH
#define _STACK_PORT_HH

#include <memory>
#include <unordered_map>
#include <string>

#include "netstar/qp_wrapper.hh"

#include "net/native-stack.hh"

using namespace seastar;

namespace netstar{

namespace refactor{

// This port contains a seastar native network stack.
// So that, no matter who gets a reference to this port,
// it can use the native network stack of seastar.
// Note that the network stack is not the native_network_stack
// of seastar. native_network_stack can not be accessed by any
// other classes.
// Here the network stack is called minimal_network_stack, which contains
// a interface and a ipv4. No dhcp is allowed and the IP addresses of the
// ipv4 is statically configured.
class stack_port{
    struct minimal_network_stack{
        net::interface netif;
        net::ipv4 inet;
        explicit minimal_network_stack(std::shared_ptr<net::device> dev) :
                netif(std::move(dev)),
                inet(&netif){}
    };
    uint16_t _port_id;
    qp_wrapper _qp_wrapper;
    std::unique_ptr<minimal_network_stack> _network_stack;
public:
    // default constructor, initialize the _qp_wrapper.
    // This triggers the start of the underlying NIC device.
    explicit stack_port(boost::program_options::variables_map& opts,
                        net::device* dev,
                        uint16_t port_id) :
        _port_id(port_id),
        _qp_wrapper(opts, dev, engine().cpu_id()){
    }

    // After the underlying NIC is successfully started, dev's link_ready
    // promise will be set. This should be called after link_ready promise
    // is set to initialize the network stack.
    // Note that the addr_map should contain three required field, otherwise
    // an exception will be thrown.
    future<> initialize_network_stack(
            boost::program_options::variables_map& opts,
            std::shared_ptr<net::device> dev,
            std::unordered_map<std::string, net::ipv4_address> addr_map){
        _network_stack = std::make_unique<minimal_network_stack>(std::move(dev));

        _network_stack->inet.get_udp().set_queue_size(opts["udpv4-queue-size"].as<int>());

        _network_stack->inet.set_host_address(addr_map.at("host-ipv4-addr"));
        _network_stack->inet.set_gw_address(addr_map.at("gw-ipv4-addr"));
        _network_stack->inet.set_netmask_address(addr_map.at("netmask-ipv4-addr"));

        return make_ready_future<>();
    }

    ~stack_port(){
        // Extend the life time of _network_stack.
        // By doing this, we can ensure that the _network_stack is destroyed
        // after the qp_wrapper.
        engine().at_destroy([network_stack = std::move(_network_stack)]{});
    }

    // stack_port can only be constructed by per_core_objs,
    // so all the copy and move constructors/assignments are deleted
    stack_port(const stack_port& other) = delete;
    stack_port(stack_port&& other)  = delete;
    stack_port& operator=(const stack_port& other) = delete;
    stack_port& operator=(stack_port&& other) = delete;

    // stop() has to be added so that stack_port can be
    // constructed by per_core_objs.
    future<> stop(){
        return make_ready_future<>();
    }

};

} // namespace refactor

} // namespace netstar

#endif // _STACK_PORT_HH
