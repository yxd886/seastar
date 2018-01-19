#ifndef _DUMMY_HOOK_HH
#define _DUMMY_HOOK_HH

#include "netstar/hookpoint/hook.hh"

namespace netstar {

namespace internal {

class dummy_hook : public hook {
    bool _recv_func_configured;

public:
    dummy_hook(port* p)
        : hook(p)
        , _recv_func_configured(false) {}

    virtual void update_port_recv_func(std::function<seastar::future<> (rte_packet)> new_func) override {
        _recv_func_configured = true;
        _recv_func = std::move(new_func);
    }

    virtual bool check_and_start() override {
        if(!_recv_func_configured) {
            return false;
        }

        start_receving();
        return true;
    }
};

}

}; // namespace netstar

#endif