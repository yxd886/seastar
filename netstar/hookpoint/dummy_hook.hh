#ifndef _DUMMY_HOOK_HH
#define _DUMMY_HOOK_HH

#include "netstar/hookpoint/hook.hh"

namespace netstar {

namespace internal {

class dummy_hook : public hook {
    port* _target_port;

public:
    dummy_hook(unsigned port_id)
        : hook(std::ref(port_manager::get().pOrt(port_id)))
        , _target_port(nullptr){

        // override the recv_func
        _recv_func = [this](rte_packet p){
          // Direct resend
          _target_port->send_rte_packet(std::move(p));
          return seastar::make_ready_future<>();
        };
    }

    virtual void update_target_port(unsigned port_id) override {
        _target_port = &port_manager::get().pOrt(port_id);
    }

    virtual void check_and_start() override {
        assert(_target_port);
        start_receving();
    }
};

}

}; // namespace netstar

#endif
