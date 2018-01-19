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
    port* _p;
    std::function<seastar::future<> (rte_packet)> _recv_func;
    std::experimental::optional<seastar::subscription<rte_packet>> _sub;

    void start_receving() {
        // Move or copy?
        _sub.emplace(_p->receive(_recv_func));
    }

public:
    explicit hook(port* p)
        : _p(p)
        , _recv_func([](rte_packet p){return seastar::make_ready_future<>();}) {
        seastar::fprint(std::cout, "hook point is created on core %d.\n", seastar::engine().cpu_id());
    }

    virtual ~hook() {}

    // Each hook point has a default receive function to receive rte_packet
    // from the port. This interface method can be used to replace this receive
    // function.
    virtual void update_port_recv_func(std::function<seastar::future<> (rte_packet)> new_func) = 0;

    // Check whether the hook point has been correctly configured.
    // If so, the hook point is started by calling start_receving() and return true.
    // If not, the hook point is not started and return false.
    virtual void check_and_start() = 0;
};

} // namespace netstar

#endif // _HOOK_HH
