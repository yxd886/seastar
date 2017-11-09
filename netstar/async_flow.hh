#ifndef _ASYNC_FLOW_HH
#define _ASYNC_FLOW_HH

#include "net/packet.hh"

#include "core/shared_ptr.hh"
#include "core/future.hh"
#include "core/reactor.hh"
#include "core/timer.hh"

#include <deque>
#include <experimental/optional>
#include <unordered_map>
#include <chrono>

using namespace seastar;

namespace netstar{

namespace internal{

// Possible state experienced by bidirection_async_flow.
enum class af_state {
    ACTIVE,         // The flow is active.
    IDLE_TIMEOUT,   // The flow timeouts due to idleness.
    ABORT           // The flow is aborted, primarily by user
};

}



template<typename FlowKeyType>
class async_flow_manager{
    class async_flow_impl;

    std::unordered_map<FlowKeyType, lw_shared_ptr<async_flow_impl>> _flow_table;
    std::experimental::optional<subscription<net::packet>> _ingress_input_sub;

public:
    // Register a sending stream to inject ingress packets
    // to the async_flow_manager.
    void register_ingress_input(stream<net::packet> istream){
        _ingress_input_sub.emplace(istream.listen([this](net::packet pkt){
            return make_ready_future<>();
        }));
    }
};

}

#endif
