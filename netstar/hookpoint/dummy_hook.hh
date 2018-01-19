#ifndef _DUMMY_HOOK_HH
#define _DUMMY_HOOK_HH

#include "netstar/hookpoint/hook.hh"

namespace netstar {

namespace internal {

class dummy_hook : public hook {
    bool _recv_func_configured;

public:
    dummy_hook(unsigned port_id)
        : hook(&(port_manager::get().pOrt(port_id)))
        , _recv_func_configured(false) {
    }

    virtual void update_port_recv_func(std::function<seastar::future<> (rte_packet)> new_func) override {
        _recv_func_configured = true;
        _recv_func = new_func;
    }

    virtual void check_and_start() override {
        assert(_recv_func_configured);

        start_receving();
    }
};

}

}; // namespace netstar

#endif
