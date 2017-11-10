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

template<typename FlowKeyType>
class async_flow;
template<typename FlowKeyType>
class async_flow_manager;

class asyn_flow_abort : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "abort";
    }
};

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
        _to.set_callback([this]{timeout();});
        _to.arm(std::chrono::seconds(timeout_interval));
    }
    void remote_from_flow_table(){
        if(_to.armed()){
            _to.cancel();
        }
        _manager._flow_table.erase(_flow_key);
    }
public:
    void received(net::packet pkt) {
        _pkt_counter += 1;
        if(_receiveq.size() < max_receiveq_size &&
           _status == af_state::ACTIVE) {
            _receiveq.push_back(std::move(pkt));
            if(_new_pkt_promise){
                _new_pkt_promise->set_value();
                _new_pkt_promise = {};
            }
        }
    }
public:
    future<> wait_for_new_pkt(){
        if(!_receiveq.empty() || _status != af_state::ACTIVE){
            return make_ready_future<>();
        }
        _new_pkt_promise = promise<>();
        return _new_pkt_promise->get_future();
    }
    net::packet get_new_pkt(){
        if(_status != af_state::ACTIVE){
            return net::packet::make_null_packet();
        }
        auto pkt = _receiveq.front();
        _receiveq.pop_front();
        return pkt;
    }
    future<> send_pkt(net::packet pkt){
        if(_status == af_state::ACTIVE){
            return _manager.send(std::move(pkt));
        }
        else{
            return make_ready_future<>();
        }
    }
    void abort(){
        if(_status!=af_state::ABORT){
            while(!_receiveq.empty()){
                _receiveq.pop_front();
            }
            if(_new_pkt_promise){
                _new_pkt_promise->set_exception(asyn_flow_abort());
                _new_pkt_promise = {};
            }
            _status = af_state::ABORT;
        }
    }
private:
    void timeout(){
        if(_previous_pkt_counter == _pkt_counter){
            _status == af_state::IDLE_TIMEOUT;
            if(_new_pkt_promise){
                _new_pkt_promise->set_value();
                _new_pkt_promise = {};
            }
            while(!_receiveq.empty()){
                _receiveq.pop_front();
            }
            remote_from_flow_table();
        }
        _previous_pkt_counter = _pkt_counter;
        _to.arm(std::chrono::seconds(timeout_interval));
    }
};

} // namespace internal

template<typename FlowKeyType>
class async_flow{
    using impl_type = lw_shared_ptr<internal::async_flow_impl<FlowKeyType>>;
    impl_type _impl;
public:
    explicit async_flow(impl_type impl)
        : _impl(std::move(impl)) {
    }
    ~async_flow(){
        _impl->abort();
    }
    async_flow(const async_flow& other) = delete;
    async_flow(async_flow&& other)
        : _impl(std::move(other._impl)) {
    }
    async_flow& operator=(const async_flow& other) = delete;
    async_flow& operator=(async_flow&& other) {
        if(&other != this){
            this->~async_flow();
            new (this) async_flow(std::move(other));
        }
        return *this;
    }
    future<> on_new_packet(){
        return _impl->wait_for_new_pkt();
    }

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
public:
    future<> send(net::packet pkt){
        return _egress_output_stream.produce(std::move(pkt));
    }

};

} // namespace netstar

#endif
