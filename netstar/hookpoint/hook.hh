#ifndef _HOOK_HH
#define _HOOK_HH

#include "netstar/port_manager.hh"
#include "netstar/stack/stack_manager.hh"

namespace netstar{

enum class hook_type {
    dummy
};

class hook {
protected:
    port& _p;
    std::function<seastar::future<> (rte_packet)> _recv_func;
    std::experimental::optional<seastar::subscription<rte_packet>> _sub;

    void start_receving() {
        // Move or copy?
        _sub.emplace(_p.receive(_recv_func));
    }

public:
    explicit hook(port& p)
        : _p(p)
        , _recv_func([](rte_packet p){return seastar::make_ready_future<>();}) {
        seastar::fprint(std::cout, "hook point is created on core %d.\n", seastar::engine().cpu_id());
    }

    virtual ~hook() {}

    virtual void update_target_port(unsigned port_id) {
        seastar::fprint(std::cout,"update_target_port is not defined for a hook.\n");
        abort();
    }

    virtual void attach_stack(unsigned stack_id) {
        seastar::fprint(std::cout,"attach_stack is not defined for a hook.\n");
        abort();
    }

    virtual void check_and_start() = 0;
};

} // namespace netstar

#endif // _HOOK_HH
