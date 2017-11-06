#ifndef _PORT_ENV_HH
#define _PORT_ENV_HH

#include "netstar/stack_port.hh"
#include "netstar/port_refactor.hh"

#include "net/dpdk.hh"

namespace netstar{

namespace refactor{

// port_type defines three kinds of ports
// that we can create for netstar.
// original: The original port defined in net/dpdk.cc.
//           The default offloading functionalities, especially
//           the TCP tso offload is enabled in this mode.
//           This port_type can only be used by stack_port, which
//           exposes the seastar tcp/ip stack.
// netstar_dpdk: The port defined in netstar/netstar_dpdk_device.cc
//               For this port, the TCP tso offloading is turned off.
//               This is used to carry out basic network funciton processing.
// fdir: The port defined in netstar/fdir_device.cc.
//       This is used to interface with mica client.
//       The checksum offloading is turned off in this mode.
enum class port_type{
    original,
    netstar_dpdk,
    fdir
};

class ports_env{
    std::vector<per_core_objs<refactor::port>> _ports;
    std::vector<per_core_objs<refactor::stack_port>> _stack_ports;

    std::vector<std::shared_ptr<net::device>> _devs;
    std::vector<uint16_t> _port_ids;

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
                      port_type pt){
        assert(port_check(opts, port_id));
        _ports.emplace_back();
        switch(pt) {
        case(port_type::netstar_dpdk) : {
            auto dev = create_netstar_dpdk_net_device(port_id, queue_num);
            auto dev_ptr = dev.release();
            std::shared_ptr<net::device> dev_shared_ptr;
            dev_shared_ptr.reset(dev_ptr);
            _devs.push_back(std::move(dev_shared_ptr));
            break;
        }
        case(port_type::fdir) : {
            auto dev = create_fdir_device(port_id, queue_num);
            auto dev_ptr = dev.release();
            std::shared_ptr<net::device> dev_shared_ptr;
            dev_shared_ptr.reset(dev_ptr);
            _devs.push_back(std::move(dev_shared_ptr));
            break;
        }
        default : {
            break;
        }
        }
        _port_ids.push_back(port_id);

        auto& ports = _ports.back();
        auto dev  = _devs.back().get();

        return ports.start(opts, dev, port_id).then([dev]{
            return dev->link_ready();
        });
    }

    future<> add_stack_port(boost::program_options::variables_map& opts,
                      uint16_t port_id,
                      uint16_t queue_num,
                      std::unordered_map<std::string, net::ipv4_address> addr_map){
        assert(port_check(opts, port_id));
        _stack_ports.emplace_back();

        // Create an original port.
        auto dev = seastar::create_dpdk_net_device(port_id, queue_num);
        auto dev_ptr = dev.release();
        std::shared_ptr<net::device> dev_shared_ptr;
        dev_shared_ptr.reset(dev_ptr);
        _devs.push_back(dev_shared_ptr);

        _port_ids.push_back(port_id);

        auto& new_stack_ports = _stack_ports.back();
        return new_stack_ports.start(opts, dev_ptr, port_id).then([dev_ptr]{
            return dev_ptr->link_ready();
        }).then([&new_stack_ports, &opts, dev_shared_ptr, addr_map]{
            return make_ready_future<>();
        });
    }

private:
    // Ensure that we do not re-use port, and that the native-network-stack
    // of seastar is not enabled by default.
    bool port_check(boost::program_options::variables_map& opts, uint16_t port_id){
        if(opts.count("network-stack") &&
           opts["network-stack"].as<std::string>() == "native"){
            // netstar applications should not use the default network stack of seastar.
            return false;
        }

        for(auto id : _port_ids){
            if(id == port_id){
                return false;
            }
        }
        return true;
    }
};

} // namespace refactor

} // namespace netstar

#endif // _PORT_ENV_HH