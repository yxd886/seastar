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
    static constexpr unsigned max_receiveq_size = 5;
    static constexpr unsigned timeout_interval = 5;
private:
    async_flow_manager<FlowKeyType>& _manager;
    FlowKeyType _flow_key;
    af_state _status;
    circular_buffer<net::packet> _receiveq;
    std::experimental::optional<promise<>> _new_pkt_promise;
    timer<steady_clock_type> _to;
    unsigned _pkt_counter;
    unsigned _previous_pkt_counter;
public:
    async_flow_impl(async_flow_manager<FlowKeyType>& manager,
                    FlowKeyType& flow_key)
        : _manager(manager)
        , _flow_key(flow_key)
        , _status(af_state::ACTIVE)
        , _pkt_counter(0)
        , _previous_pkt_counter(0) {
        // _to.set_callback([this]{timeout();});
        // _to.arm(std::chrono::seconds(timeout_interval));
    }
    void remote_from_flow_table(){
        _manager._flow_table.erase(_flow_key);
    }
public:
    void received(net::packet pkt) {
        _pkt_counter += 1;
        if(_receiveq.size() < max_receiveq_size && _status == af_state::ACTIVE) {
            _receiveq.push_back(std::move(pkt));
            if(_new_pkt_promise){
                _new_pkt_promise->set_value();
                _new_pkt_promise = {};
            }
        }
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
    friend class internal::async_flow_impl<FlowKeyType>;
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
