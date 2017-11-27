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
class af_ev_context;
template<typename Ppr>
class async_flow_impl;
template<typename Ppr>
class async_flow;
template<typename Ppr>
class async_flow_manager;

template<typename Ppr>
class af_ev_context{
    using EventEnumType = typename Ppr::EventEnumType;

    net::packet _pkt;
    const filtered_events<EventEnumType> _fe;
    const bool _is_client;
    const bool _is_send;

    friend class async_flow_impl<Ppr>;

public:
    af_ev_context(net::packet pkt,
                  filtered_events<EventEnumType> fe,
                  bool is_client,
                  bool is_send)
        : _pkt(std::move(pkt))
        , _fe(fe)
        , _is_client(is_client)
        , _is_send(is_send) {
    }

    const filtered_events<EventEnumType>& events(){
        return _fe;
    }
    bool is_client(){
        return _is_client;
    }
    bool is_send(){
        return _is_send;
    }
private:
    net::packet extract_packet() {
        return std::move(_pkt);
    }
};

template<typename Ppr>
struct af_work_unit {
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;

    Ppr ppr;
    std::experimental::optional<promise<>> async_loop_pr;
    registered_events<EventEnumType> send_events;
    registered_events<EventEnumType> recv_events;
    circular_buffer<af_ev_context<Ppr>> buffer_q;
    std::experimental::optional<FlowKeyType> flow_key;
    uint16_t direction;
    bool is_client;
    bool loop_started;
    bool loop_has_context;

    af_work_unit(bool is_client_arg,
                 uint16_t direction_arg)
        : ppr(is_client_arg)
        , direction(direction_arg)
        , is_client(is_client_arg)
        , loop_started(false)
        , loop_has_context(false) {
        buffer_q.reserve(5);
    }
};

template<typename Ppr>
class async_flow_impl{
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;
    static constexpr bool packet_recv = true;

    af_work_unit<Ppr> _client;
    af_work_unit<Ppr> _server;
public:
    async_flow_impl(uint16_t client_direction,
                    FlowKeyType client_flow_key)
        : _client(true, client_direction)
        , _server(false, get_reverse_direction(client_direction)){
        _client.flow_key = client_flow_key;
    }

    void handle_packet_send(net::packet pkt, uint16_t direction){
        if(_client.buffer_q.size()>5){
            // drop the packet due to buffer overflow.
            return;
        }

        bool is_client = (direction == _client.direction);
        af_work_unit<Ppr>& send_unit = is_client ? _client : _server;
        generated_events<EventEnumType> ge = send_unit.ppr.handle_packet_send(pkt);
        filtered_events<EventEnumType> fe = send_unit.send_events.filter(ge);

        if(send_unit.loop_started){
            if(send_unit.async_loop_pr && fe.no_event()){
                // unconditionally forward the packet to receive side.
                handle_packet_recv(std::move(pkt), ~is_client);
                return;
            }
            send_unit.buffer_q.emplace_back(std::move(pkt), fe, is_client, packet_recv);
            if(send_unit.async_loop_pr){
                assert(send_unit.loop_has_context == false);
                send_unit.async_loop_pr->set_value();
            }
        }
        else{
            handle_packet_recv(std::move(pkt), ~is_client);
        }
    }

    void handle_packet_recv(net::packet pkt, bool is_client){
        if(is_client == false){
            // try to fill in the correct server side flow key
            // and register the flow key into the flow table.
        }
        af_work_unit<Ppr>& recv_unit = is_client? _client : _server;
        generated_events<EventEnumType> ge = recv_unit.ppr.handle_packet_recv(pkt);
        filtered_events<EventEnumType> fe = recv_unit.recv_events.filter(ge);

        if(recv_unit.loop_started){
            if(recv_unit.async_loop_pr && fe.no_event()){
                send_packet_out(std::move(pkt), is_client);
                return;
            }
            recv_unit.buffer_q.emplace_back(std::move(pkt), fe, is_client, ~packet_recv);
            if(recv_unit.async_loop_pr){
                assert(recv_unit.loop_has_context == false);
                recv_unit.async_loop_pr->set_value();
            }
        }
        else{
            send_packet_out(std::move(pkt), is_client);
        }
    }

    void send_packet_out(net::packet pkt, bool is_client){
        if(is_client){
            // send the packet out from _client.direction
        }
        else{
            // send the packet out from _server.direction
        }
    }

    af_ev_context<Ppr> peek_event_context(bool is_client) {
        af_work_unit<Ppr>& working_unit = is_client ? _client : _server;

        // Use assertion to protect the async loop from incorrect
        // use of peek_event_context API.
        assert( (working_unit.loop_has_context == false) &&
                (!working_unit.buffer_q.empty()) );

        working_unit.loop_has_context = true;
        auto ret(std::move(working_unit.buffer_q.front()));
        working_unit.buffer_q.pop_front();
        return ret;
    }

    void destroy_event_context(af_ev_context<Ppr> context) {
        af_work_unit<Ppr>& working_unit = context.is_client() ?
                                          _client : _server;
        working_unit.loop_has_context = true;
    }

    void forward_event_context(af_ev_context<Ppr> context) {
        bool is_client = context.is_client();
        af_work_unit<Ppr>& working_unit = is_client ? _client : _server;
        working_unit.loop_has_conetxt = false;
        if(context.is_send()){
            handle_packet_recv(context.extract_packet(), ~is_client);
        }
        else{
            send_packet_out(context.extract_packet(), is_client);
        }
    }

    future<> on_new_events(bool is_client) {
        af_work_unit<Ppr>& working_unit = is_client ? _client : _server;
        assert((working_unit.loop_has_context == false) &&
                (!working_unit.async_loop_pr));

        if(!working_unit.buffer_q.empty()){
            while(!working_unit.buffer_q.empty()) {
                auto& next_context = working_unit.buffer_q.front();
                if(next_context.events().no_event()){
                    if(next_context.is_send()){
                        handle_packet_recv(next_context.extract_packet(), ~next_context.is_client());
                    }
                    else{
                        send_packet_out(next_context.extract_packet(), next_context.is_client());
                    }
                    working_unit.buffer_q.pop_front();
                }
                else{
                    return make_ready_future<>();
                }
            }
        }


        working_unit.async_loop_pr = promise<>();
        return working_unit.async_loop_pr->get_future();
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
