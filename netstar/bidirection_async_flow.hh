#ifndef _bidirection_async_flow_HH
#define _bidirection_async_flow_HH

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

// An exception raised by bidirection_async_flow.
class asyn_flow_abort : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "abort";
    }
};

// Direction of the packet.
enum class direction{
    INGRESS,
    EGRESS
};

// Internal packet representation.
struct directed_packet{
    net::packet pkt;
    const direction dir;
};

template<typename FKTrait>
class bidirection_bidirection_async_flow_manager{
    // The type of the corresponding flow key.
    using FlowKeyType = typename FKTrait::FlowKeyType;
    // The meta data passed in together with the packet.
    using PacketMetaData = typename FKTrait::PacketMetaData;
    // Possible state experienced by bidirection_async_flow.
    enum class af_impl_state {
        ACTIVE,         // The flow is active.
        IDLE_TIMEOUT,   // The flow timeouts due to idleness.
        ABORT           // The flow is aborted, primarily by user
    };

    class bidirection_async_flow_impl;
private:
    std::unordered_map<FlowKeyType, lw_shared_ptr<bidirection_async_flow_impl>> _flow_table;

public:


    class bidirection_async_flow_impl : public enable_lw_shared_from_this<bidirection_async_flow_impl>{
        // Size of the receive queue
        static constexpr unsigned max_receiveq_size = 5;
        // 5s timeout interval
        static constexpr unsigned timeout_interval = 5;

        // The bidirection_async_flow_manager
        bidirection_async_flow_manager& _manager;
        // The flow key
        FlowKeyType _flow_key;
        // The default direction when the first packet of
        // this bidirection_async_flow arrives
        direction _default_dir;
        // The status of the master flow
        af_impl_state _status;
        // The receive queue.
        circular_buffer<directed_packet> _receiveq;
        // The promise to notify new packet arrival.
        std::experimental::optional<promise<>> _new_pkt_promise;
        // The timer to timeout the master flow and the
        // packet counters.
        timer<steady_clock_type> _to;
        unsigned _pkt_counter;
        unsigned _previous_pkt_counter;

        // If this boolean flag is set, the bidirection_async_flow will
        // acquire flow packets in the other direction.
        bool _interested_in_reverse_dir;
    public:
        explicit bidirection_async_flow_impl(bidirection_async_flow_manager& manager,
                                             FlowKeyType flow_key,
                                             direction default_dir)
            : _manager(manager)
            , _flow_key(flow_key)
            , _default_dir(default_dir)
            , _status(af_impl_state::ACTIVE)
            , _pkt_counter(0)
            , _previous_pkt_counter(0)
            , _interested_in_reverse_dir(false) {
            _to.set_callback([this]{timeout();});
            _to.arm(std::chrono::seconds(timeout_interval));
        }
        void remove_from_flow_table(){
            _manager._flow_table.erase(_flow_key);
            if(_interested_in_reverse_dir){
                /*
                 * submit_to(dst_core, [reverse_flow_key, dst_core_manager]{
                 *  dst_core_manager.remove_flow(reverse_flow_key);
                 * })
                 *
                 */

                _manager._flow_table.erase(_flow_key.build_reverse_key());
            }
        }
    public:
        void received(directed_packet pkt) override{
            _pkt_counter += 1;
            if(_receiveq.size() < max_receiveq_size && _status == af_impl_state::ACTIVE) {
                _receiveq.push_back(std::move(pkt));
                if(_new_pkt_promise){
                    _new_pkt_promise->set_value();
                    _new_pkt_promise = {};
                }
            }
        }
    public:
        future<> wait_for_new_pkt(){
            if(!_receiveq.empty() || _status == af_impl_state::IDLE_TIMEOUT){
                return make_ready_future<>();
            }
            _new_pkt_promise = promise<>();
            return _new_pkt_promise->get_future();
        }
        directed_packet get_pkt(){
            if(_status == af_impl_state::IDLE_TIMEOUT){
                return directed_packet{net::packet::make_null_packet(), direction::INGRESS};
            }
            auto pkt = _receiveq.front();
            _receiveq.pop_front();
            return pkt;
        }
        future<> send_pkt(directed_packet pkt){
            return _manager.send(std::move(pkt));
        }
        void abort(){
            assert(_status == af_impl_state::ACTIVE);
            while(!_receiveq.empty()){
                _receiveq.pop_front();
            }
            if(_new_pkt_promise){
                _new_pkt_promise->set_exception(asyn_flow_abort());
                _new_pkt_promise = {};
            }
            _status = af_impl_state::ABORT;
        }
        void timeout(){
            if(_previous_pkt_counter == _pkt_counter){
                _status == af_impl_state::IDLE_TIMEOUT;
                if(_new_pkt_promise){
                    _new_pkt_promise->set_value();
                    _new_pkt_promise = {};
                }
            }
            _previous_pkt_counter = _pkt_counter;
        }
    };

public:
    future<> send(directed_packet pkt){
        return make_ready_future<>();
    }
};





}

#endif
