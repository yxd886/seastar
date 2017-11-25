#ifndef _ASYNC_FLOW_HH
#define _ASYNC_FLOW_HH

#include "net/packet.hh"

#include "core/shared_ptr.hh"
#include "core/future.hh"
#include "core/reactor.hh"
#include "core/timer.hh"
#include "core/queue.hh"

#include "netstar/af/async_flow_event.hh"

#include <deque>
#include <experimental/optional>
#include <unordered_map>
#include <chrono>

using namespace seastar;

namespace netstar {

/*
 * Some terminology.
 *  1. Direction: An integer that is used to record from which port
 *  does the packet comes in. This can be used to distinguish client
 *  side from server side. For instance, suppose that client side
 *  direction is 0 and server side direction is 1. Then packets with
 *  direction 0 are sent by the client side, and they should be processed
 *  by the client side preprocessor first. Packets with direction 1 are
 *  sent by the server side, and they should be processed by the server
 *  side preprocessor first.
 *  2. Preprocessor: I plan to name the template argument passed to
 *  async_flow as preprocessor (PPR). Preprocessor makes a lot of sense.
 *  Both client side and server side tracking module only monitors
 *  the action of the flow. They look like preprocessors. The actual decision
 *  making will be offloaded to the two asynchronous loops.
 *  3. Asynchronous loop: The core of async_flow is an asynchronous loop.
 *  The asynchronous loop checks for registered events, if interested events
 *  happen, the asynchronous loop processes the events in a purely asynchronous
 *  fashion. So that we can enable different kinds of queries and checkings
 *  in our source code.
 *  4. On demand server side creation: the server side preprocessor is only
 *  created if the client side asynchronous loop would like to the packet
 *  to be processed by the server side preprocessor.
 *  5.
 */
/*
 * Ppr interface:
 * (processed_pkt, event_bit_set) = ppr->handle_packet_sent(pkt)
 * (processed_pkt, event_bit_set) = ppr->handle_packet_recv(pkt, event_bit_set)
 *
 * When async_flow gets (processed_pkt, event_bit_set) tuple, it appends
 * direction and send_recv_flag to make the tuple into
 * (processed_pkt, event_bit_set, direction, send_recv_flag, ppr_side) and put it
 * into the queue.
 *
 * If the queues are empty and the async loop associated with the Ppr is
 * waiting for new event, we should check whether the event_bit_set is
 * empty, if so, no interested event happen and we can directly pass the
 * event according to the direction and Ppr. (If direction comes from the ppr side
 * , pass it to the other ppr, otherwise send the packet out.)
 * Otherwise, we enqueue the packet. When the async loop is resumed, it will
 * try to empty the queue before finding out packets that trigger interested
 * events.
 *
 * A flag indicating whether the async loop is still running. The flag is
 * initialized to false, and set to true when the async loop tries to run.
 * If the user would like to quit the async loop, it should do so by explicitly
 * setting the flag back to false before quitting. Otherwise, packets will
 * be halted on the pipeline without moving forward.
 *
 */

enum side : uint8_t {
    client,
    server
};

enum send_recv : uint8_t {
    send,
    recv
}; // or use a bool is_send

template<typename Ppr>
struct side_instance {
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;

    Ppr ppr;
    std::experimental::optional<promise<>> async_loop_pr;
    registered_events<EventEnumType> send_events;
    registered_events<EventEnumType> recv_events;
    circular_buffer<net::packet> _buffer_q;
    FlowKeyType flow_key;
    uint16_t direction;
    side local_side;
    bool loop_started;
    bool loop_has_context;
};

template<typename Ppr>
class asyn_flow_impl;
template<typename Ppr>
class async_flow;
template<typename Ppr>
class async_flow_manager;

template<typename Ppr>
class async_flow_impl{

};

template<typename Ppr>
class async_flow{
};

template<typename Ppr>
class async_flow_manager{

};

} // namespace netstar

#endif
