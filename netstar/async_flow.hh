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

// Forward declaration of public class interface.
class async_flow;

template<typename FlowKeyType>
class async_flow_manager;

namespace internal {

template<typename FlowKeyType>
class async_flow_impl;

// Possible state experienced by bidirection_async_flow.
enum class af_state {
    ACTIVE,         // The flow is active.
    IDLE_TIMEOUT,   // The flow timeouts due to idleness.
    ABORT           // The flow is aborted, primarily by user
};

template<typename FlowKeyType>
class async_flow_impl{
    // Size of the receive queue
    static constexpr unsigned max_receiveq_size = 5;
    // 5s timeout interval
    static constexpr unsigned timeout_interval = 5;
private:
    async_flow_manager<FlowKeyType>& _manager;

public:
    async_flow_impl(async_flow_manager<FlowKeyType>& manager)
        : _manager(manager) {
    }
};

} // namespace internal

class async_flow{

};

template<typename FlowKeyType>
class async_flow_manager{
    std::unordered_map<FlowKeyType, lw_shared_ptr<internal::async_flow_impl<FlowKeyType>>> _flow_table;
    std::experimental::optional<subscription<net::packet>> _ingress_input_sub;
    stream<net::packet> _egress_output_stream;

public:
    // Register a sending stream to inject ingress packets
    // to the async_flow_manager.
    void register_ingress_input(stream<net::packet> istream){
        _ingress_input_sub.emplace(istream.listen([this](net::packet pkt){
            return make_ready_future<>();
        }));
    }
    // Register output send function for the egress stream.
    subscription<net::packet> register_egress_output(std::function<future<>(net::packet)> fn){
        return _egress_output_stream.listen(std::move(fn));
    }

};

} // namespace netstar

#endif
