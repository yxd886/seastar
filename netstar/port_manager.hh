#ifndef _PORT_MANAGER_HH
#define _PORT_MANAGER_HH

#include "core/distributed.hh"
#include "core/reactor.hh"

#include "net/dpdk.hh"

#include "netstar/device/standard_device.hh"
#include "netstar/device/fdir_device.hh"

#include "netstar/port.hh"
#include "netstar/shard_container.hh"

namespace netstar{

enum class port_type {
    standard,
    fdir
};

class stack_manager;

class port_manager {
    std::vector<port_type> _port_types;
    std::vector<std::unique_ptr<seastar::net::device>> _devs;
    std::vector<uint16_t> _port_ids;
    std::vector<std::vector<port*>> _ports;

    port_manager() {
    }

public:
    static port_manager& get() {
        static port_manager pm;
        return pm;
    }

    seastar::future<> add_port(boost::program_options::variables_map& opts,
                               uint16_t port_id,
                               port_type pt){
        assert(port_check(opts, port_id));
        unsigned which_one = _ports.size();

        _port_types.push_back(pt);
        _port_ids.push_back(port_id);
        _ports.push_back(std::vector<port*>(seastar::smp::count, nullptr));

        switch(pt) {
        case(port_type::standard) : {
            auto dev = create_standard_device(port_id, seastar::smp::count);
            _devs.push_back(std::move(dev));
            break;
        }
        case(port_type::fdir) : {
            auto dev = create_fdir_device(port_id, seastar::smp::count);
            _devs.push_back(std::move(dev));
            break;
        }
        default : {
            break;
        }
        }

        auto dev  = _devs.back().get();
        auto shard_sptr = std::make_shared<seastar::distributed<internal::shard_container<port>>>();
        return shard_sptr->start(opts, dev, port_id).then([dev]{
            return dev->link_ready();
        }).then([this, which_one, shard_sptr]{
             return shard_sptr->invoke_on_all(&internal::shard_container<port>::save_container_ptr,
                                              &(_ports.at(which_one)));
        }).then([this, shard_sptr]{
            return shard_sptr->stop();
        }).then([shard_sptr]{
        });
    }

    port& pOrt(unsigned i) {
        return _ports.at(i).at(seastar::engine().cpu_id());
    }

    port_type type(unsigned i) {
        return _port_types.at(i);
    }

    uint16_t dpdk_dev_idx(unsigned i) {
        return _port_ids.at(i);
    }

    unsigned num_ports() {
        return _ports.size();
    }

private:
    bool port_check(boost::program_options::variables_map& opts, uint16_t port_id){
        if(opts.count("network-stack") &&
           opts["network-stack"].as<std::string>() == "native"){
            return false;
        }

        for(auto id : _port_ids){
            if(id == port_id){
                return false;
            }
        }
        return true;
    }
    friend class stack_manager;
    seastar::net::device* dev(unsigned id) {
        return _devs.at(id).get();
    }
};

} // namespace netstar

#endif // _PORT_MANAGER_HH
