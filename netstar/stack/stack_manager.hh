#ifndef _STACK_MANAGER_HH
#define _STACK_MANAGER_HH

#include "netstar/stack/dummy_device.hh"
#include "netstar/port_manager.hh"

#include "core/distributed.hh"
#include "core/print.hh"

namespace netstar {

class stack_manager {
    std::vector<unsigned> _port_ids;
    std::vector<std::shared_ptr<seastar::net::device>> _dummy_devices;
    std::vector<std::vector<internal::multi_stack*>> _stacks;
    std::vector<std::string> _ipv4_addrs;

    using stack_shard = shard_container_trait<internal::multi_stack>;

public:
    seastar::future<> add_stack(unsigned port_id, std::string ipv4_addr,
                   std::string gw_addr, std::string netmask) {
        assert(stack_check(port_id, ipv4_addr));
        unsigned which_one = _stacks.size();

        _port_ids.push_back(port_id);
        _ipv4_addrs.push_back(ipv4_addr);
        _dummy_devices.push_back(std::make_shared<internal::dummy_device>(port_manager::get().dev(port_id)));
        _stacks.emplace_back();
        auto sptr = _dummy_devices.at(which_one);

        auto stack_shard_sptr = std::make_shared<stack_shard::shard_t>();
        return stack_shard_sptr->start(sptr, &(port_manager::get().pOrt(port_id)),
                                       ipv4_addr, gw_addr, netmask).then([stack_shard_sptr, which_one, this]{
            stack_shard_sptr->invoke_on_all(&stack_shard::instance_t::save_container_ptr,
                                            &_stacks.at(which_one));
        }).then([stack_shard_sptr]{
            seastar::fprint(std::cout, "stack creation succeed.\n");
            return stack_shard_sptr->stop();
        }).then([stack_shard_sptr]{
            seastar::fprint(std::cout, "stack stop succeed.\n");
        });
    }

    static stack_manager& get() {
        static stack_manager sm;
        return sm;
    }

    unsigned port_id(unsigned stack_id) {
        return _port_ids.at(stack_id);
    }

    std::string ipv4_addr(unsigned stack_id) {
        return _ipv4_addrs.at(stack_id);
    }

    seastar::net::network_stack& stack(unsigned stack_id) {
        return *(_stacks.at(stack_id).at(seastar::engine().cpu_id())->get_stack());
    }

private:
    bool stack_check(unsigned port_id, std::string ipv4_addr) {
        for(auto id : _port_ids) {
            if (id == port_id) {
                seastar::fprint(std::cout, "stack_manager ERROR: Duplicated port ID %d.\n", port_id);
                return false;
            }
        }

        for(auto& addr : _ipv4_addrs) {
            if(addr == ipv4_addr) {
                seastar::fprint(std::cout, "stack_manager ERROR: Dupliacted IPv4 address %s.\n", addr);
                return false;
            }
        }

        return true;
    }
};

} // namespace netstar

#endif
