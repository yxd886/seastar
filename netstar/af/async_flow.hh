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

template<typename Ppr>
struct work_unit {
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;

    Ppr ppr;
    std::experimental::optional<promise<>> async_loop_pr;
    registered_events<EventEnumType> send_events;
    registered_events<EventEnumType> recv_events;
    circular_buffer<net::packet> _buffer_q;
    std::experimental::optional<FlowKeyType> flow_key;
    uint16_t direction;
    bool is_client;
    bool loop_started;
    bool loop_has_context;

    work_unit(bool is_client_arg,
              uint16_t direction_arg)
        : ppr(is_client_arg)
        , direction(direction_arg)
        , is_client(is_client_arg)
        , loop_started(false)
        , loop_has_context(false) {
        _buffer_q.reserve(5);
    }
};

template<typename Ppr>
class asyn_flow_impl;
template<typename Ppr>
class async_flow;
template<typename Ppr>
class async_flow_manager;

template<typename Ppr>
class async_flow_impl{
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;

    work_unit<Ppr> _client;
    work_unit<Ppr> _server;
public:
    async_flow_impl(uint16_t client_direction,
                    FlowKeyType client_flow_key)
        : _client(true, client_direction)
        , _server(false, get_reverse_direction(client_direction)){
        _client.flow_key = client_flow_key;
    }

    void handle_packet_send(net::packet pkt, uint16_t direction){
        bool client_work = (direction == _client.direction);

        work_unit<Ppr>& send_unit = client_work ? _client : _server;
        work_unit<Ppr>& recv_unit = client_work ? _server : _client;

        generated_events<EventEnumType> ge = send_unit.ppr.handle_packet_send(pkt);
        filtered_events<EventEnumType> fe = send_unit.send_events.filter(ge);

        if(send_unit.loop_started){
            if(send_unit.async_loop_pr && fe.no_event()){
                // unconditionally forward the packet to receive side.
            }
        }
    }

    void handle_packet_recv(net::packet pkt, bool client_work){
        work_unit<Ppr>& recv_unit = client_work? _client : _server;

        generated_events<EventEnumType> ge = recv_unit.ppr.handle_packet_recv(pkt);
        filtered_events<EventEnumType> fe = recv_unit.recv_events.filter(ge);

        if(recv_unit.loop_started){

        }
    }

private:
    uint16_t get_reverse_direction(const uint16_t direction){
        return direction;
    }
};

template<typename Ppr>
class async_flow{
};

template<typename Ppr>
class async_flow_manager{

};

} // namespace netstar

#endif
