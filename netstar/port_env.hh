#ifndef _PORT_ENV_HH
#define _PORT_ENV_HH

#include "netstar/stack_port.hh"
#include "netstar/per_core_objs.hh"
#include "netstar/netstar_dpdk_device.hh"
#include "netstar/fdir_device.hh"
#include "netstar/port.hh"

#include "net/dpdk.hh"

namespace netstar{

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
    std::vector<per_core_objs<port>> _ports;
    std::vector<port_type> _port_types;
    std::vector<per_core_objs<stack_port>> _stack_ports;
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
        _port_types.push_back(pt);
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
            auto sem = std::make_shared<semaphore>(0);
            auto vec = std::make_shared<std::vector<net::arp_for<net::ipv4>*>>(smp::count);
            for(unsigned i=0; i<smp::count; i++){
                smp::submit_to(i, [&new_stack_ports, &opts, dev_shared_ptr, addr_map]{
                    return new_stack_ports.local_obj().
                            initialize_network_stack(opts,
                                                     dev_shared_ptr,
                                                     addr_map);
                }).then([vec, i](net::arp_for<net::ipv4>* arp_instance){
                    vec->at(i) = arp_instance;
                }).then_wrapped([sem, i](auto&& f){
                    try{
                        f.get();
                        sem->signal();
                    }
                    catch(const std::exception & ex ){
                        std::cout<<ex.what()<<std::endl;
                        std::string err_msg("Fail to initialize network stack");
                        err_msg += std::to_string(i);
                        sem->broken(std::runtime_error(err_msg));
                    }
                });
            }
            return sem->wait(smp::count).then([vec, &new_stack_ports]{
                return new_stack_ports.invoke_on_all([vec](stack_port& sp){
                    sp.set_arp_for(*vec);
                });
            });
        });
    }

    port& local_port(unsigned env_index){
        return _ports.at(env_index).local_obj();
    }

    port_type what_port_type(unsigned env_index){
        return _port_types.at(env_index);
    }

    stack_port& local_stack_port(unsigned env_index){
        return _stack_ports.at(env_index).local_obj();
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

} // namespace netstar

#endif // _PORT_ENV_HH
