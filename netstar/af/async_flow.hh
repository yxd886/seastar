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
class async_flow;
template<typename Ppr>
class async_flow_manager;

enum class af_side : bool {
    client=true,
    server=false
};

enum class af_trigger : bool {
    send = true,
    recv = false
};

#define ENABLE_ASSERTION

void async_flow_assert(bool boolean_expr) {
#ifdef ENABLE_ASSERTION
    assert(boolean_expr);
#endif
}

namespace internal {

template<typename Ppr>
class async_flow_impl;

template<typename Ppr>
struct af_work_unit {
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;

    Ppr ppr;
    std::experimental::optional<promise<af_ev_context<Ppr>>> async_loop_pr;
    registered_events<EventEnumType> send_events;
    registered_events<EventEnumType> recv_events;
    circular_buffer<af_ev_context<Ppr>> buffer_q;
    std::experimental::optional<FlowKeyType> flow_key;
    uint8_t direction;
    bool loop_started;
    bool loop_has_context;
    bool ppr_close;

    af_work_unit(bool is_client_arg,
                 uint8_t direction_arg)
        : ppr(is_client_arg)
        , direction(direction_arg)
        , loop_started(false)
        , loop_has_context(false)
        , ppr_close(false) {
        buffer_q.reserve(5);
    }
};

template<typename Ppr>
class async_flow_impl{
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;
    static constexpr bool packet_recv = true;
    friend class async_flow<Ppr>;

    async_flow_manager<Ppr>& _manager;
    af_work_unit<Ppr> _client;
    af_work_unit<Ppr> _server;

private:
    // General helper utility function, useful for reducing the
    // boilerplates used in this class.

    af_work_unit<Ppr>& get_work_unit(bool is_client){
        return is_client ? _client : _server;
    }

    void close_ppr_and_remove_flow_key(af_work_unit<Ppr>& work_unit) {
        work_unit.ppr_close = true;
        if(work_unit.flow_key) {
            _manager.remove_mapping_on_flow_table(*(work_unit.flow_key));
            work_unit.flow_key = std::experimental::nullopt;
        }
    }

    void action_after_packet_handle(af_work_unit<Ppr>& work_unit,
                                    filtered_events<EventEnumType> fe,
                                    net::packet pkt,
                                    bool is_client, bool is_send) {
        if((work_unit.loop_started) &&
           (!work_unit.async_loop_pr || !fe.no_event())) {
            if(work_unit.async_loop_pr) {
                async_flow_assert(work_unit.loop_has_context == false);
                work_unit.loop_has_context = true;
                work_unit.async_loop_pr->set_value(af_ev_context<Ppr>{
                    std::move(pkt), fe,
                    is_client, is_send
                });
                work_unit.async_loop_pr = {};
            }
            else{
                work_unit.buffer_q.emplace_back(
                    std::move(pkt), fe,
                    is_client, is_send
                );
            }
        }
        else {
            if(is_send) {
                handle_packet_recv(std::move(pkt), ~is_client);
            }
            else{
                send_packet_out(std::move(pkt), is_client);
            }
        }
    }

    void forward_context(af_ev_context<Ppr>& context) {
        if(context.is_send()){
            handle_packet_recv(context.extract_packet(), ~context.is_client());
        }
        else{
            send_packet_out(context.extract_packet(), context.is_client());
        }
    }

    future<af_ev_context<Ppr>> build_no_event_ready_future(bool is_client){
        return make_ready_future<af_ev_context<Ppr>>({
            net::packet::make_null_packet(),
            filtered_events<EventEnumType>::make_close_event(),
            is_client,
            true
        });
    }

public:
    // Internal interfaces, exposed to async_flow and
    // async_flow manager.
    async_flow_impl(async_flow_manager<Ppr>& manager,
                    uint8_t client_direction,
                    FlowKeyType client_flow_key)
        : _manager(manager)
        , _client(true, client_direction)
        , _server(false, manager.get_reverse_direction(client_direction)) {
        _client.flow_key = client_flow_key;
    }

    void handle_packet_send(net::packet pkt, uint8_t direction) {
        bool is_client = (direction == _client.direction);
        auto& send_unit = get_work_unit(is_client);

        if( (send_unit.buffer_q.size() >=
             Ppr::async_flow_config::max_event_context_queue_size) ||
             send_unit.ppr_close) {
            // drop the packet due to buffer overflow.
            return;
        }

        generated_events<EventEnumType> ge = send_unit.ppr.handle_packet_send(pkt);
        filtered_events<EventEnumType> fe = send_unit.send_events.filter(ge);
        if(fe.on_close_event()) {
            close_ppr_and_remove_flow_key(send_unit);
        }

        action_after_packet_handle(send_unit, fe, is_client, true);
    }

    void handle_packet_recv(net::packet pkt, bool is_client){
        auto& recv_unit = get_work_unit(is_client);

        if( (recv_unit.buffer_q.size() >=
             Ppr::async_flow_config::max_event_context_queue_size) ||
             recv_unit.ppr_close) {
            // drop the packet due to buffer overflow.
            return;
        }

        generated_events<EventEnumType> ge = recv_unit.ppr.handle_packet_recv(pkt);
        filtered_events<EventEnumType> fe = recv_unit.recv_events.filter(ge);
        if(fe.on_close_event()) {
            close_ppr_and_remove_flow_key(recv_unit);
        }

        action_after_packet_handle(recv_unit, fe, is_client, false);
    }

    void send_packet_out(net::packet pkt, bool is_client){
        auto& working_unit = get_work_unit(is_client);
        _manager.send(std::move(pkt), working_unit.direction);
    }

    void destroy_event_context(af_ev_context<Ppr> context) {
        auto& working_unit = get_work_unit(context.is_client());
        working_unit.loop_has_context = false;
    }

    void forward_event_context(af_ev_context<Ppr> context) {
        bool is_client = context.is_client();
        auto& working_unit = get_work_unit(is_client);
        working_unit.loop_has_conetxt = false;
        forward_context(context);
    }

    future<af_ev_context<Ppr>> on_new_events(bool is_client) {
        auto& working_unit = get_work_unit(is_client);
        async_flow_assert((working_unit.loop_has_context == false) &&
                          (!working_unit.async_loop_pr));

        if(working_unit.loop_started == false) {
            working_unit.loop_started = true;
        }

        if(!working_unit.buffer_q.empty()) {
            while(!working_unit.buffer_q.empty()) {
                auto& next_context = working_unit.buffer_q.front();
                if(next_context.events().no_event()) {
                    forward_context(next_context);
                    working_unit.buffer_q.pop_front();
                }
                else {
                    working_unit.buffer_q.pop_front();
                    working_unit.loop_has_context = true;
                    auto future = make_ready_future<af_ev_context<Ppr>>(
                        std::move(working_unit.buffer_q.front())
                    );
                    return future;
                }
            }
        }

        if(working_unit.ppr_close == true) {
            working_unit.loop_has_context = true;
            return build_no_event_ready_future(is_client);
        }
        else{
            working_unit.async_loop_pr = promise<af_ev_context<Ppr>>();
            return working_unit.async_loop_pr->get_future();
        }
    }

    template<EventEnumType EvT> void event_registration (bool is_client, bool is_send) {
        auto& working_unit = is_client ? _client : _server;
        auto& events = is_send ? working_unit.send_events : working_unit.recv_events;
        events.register_event<EvT>(1);
    }

    void close_async_loop (bool is_client) {
        auto& working_unit = get_work_unit(is_client);

        async_flow_assert(working_unit.loop_has_context == false &&
                          !working_unit.async_loop_pr &&
                          working_unit.loop_started == true);

        working_unit.loop_started = false;
        while(!working_unit.buffer_q.empty()) {
            auto& next_context = working_unit.buffer_q.front();
            forward_context(next_context);
            working_unit.buffer_q.pop_front();
        }
    }

    void ppr_passive_close(bool is_client){
        auto& working_unit = get_work_unit(is_client);
        close_ppr_and_remove_flow_key(working_unit);

        if(working_unit.loop_started && working_unit.async_loop_pr) {
            working_unit.loop_has_context = true;
            working_unit.async_loop_pr->set_value(
                af_ev_context<Ppr>({
                      net::packet::make_null_packet(),
                      filtered_events<EventEnumType>::make_close_event(),
                      is_client,
                      true
                })
            );
            working_unit.async_loop_pr = {};
        }
    }
};

} // namespace internal

template<typename Ppr>
class af_ev_context{
    using EventEnumType = typename Ppr::EventEnumType;

    net::packet _pkt;
    filtered_events<EventEnumType> _fe;
    bool _is_client;
    bool _is_send;

    friend class internal::async_flow_impl<Ppr>;

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

    const filtered_events<EventEnumType>& events() {
        return _fe;
    }
    bool is_client() {
        return _is_client;
    }
    bool is_send() {
        return _is_send;
    }
    bool is_null_pkt() {
        return _pkt;
    }
private:
    net::packet extract_packet() {
        return std::move(_pkt);
    }
};

template<typename Ppr>
class async_flow{
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    impl_type _impl;
public:
    explicit async_flow(impl_type impl)
        : _impl(std::move(impl)) {
    }
    ~async_flow(){
        auto& client_ref = _impl->_client;
        auto& server_ref = _impl->_server;
        async_flow_assert(!client_ref.async_loop_pr &&
                          !client_ref.loop_has_context &&
                          !client_ref.loop_started);
        async_flow_assert(!server_ref.async_loop_pr &&
                          !server_ref.loop_has_context &&
                          !server_ref.loop_started);
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

    future<af_ev_context<Ppr>> on_client_side_events() {
        return _impl->on_new_events(true);
    }
    future<af_ev_context<Ppr>> on_server_side_events() {
        return _impl->on_new_events(true);
    }
};

template<typename Ppr>
class async_flow_manager {
    using FlowKeyType = typename Ppr::FlowKeyType;
    struct io_direction {
        std::experimental::optional<subscription<net::packet, FlowKeyType&>> input_sub;
        stream<net::packet> output_stream;
        uint8_t reverse_direction;
    };

    std::unordered_map<FlowKeyType, lw_shared_ptr<internal::async_flow_impl<Ppr>>> _flow_table;
    std::vector<io_direction> _directions;
    seastar::queue<async_flow<Ppr>> _new_flow_q{Ppr::async_flow_config::new_flow_queue_size};
    friend class internal::async_flow_impl<Ppr>;
public:
    subscription<net::packet> direction_registration(uint8_t direction, uint8_t reverse_direction,
                                                     stream<net::packet, FlowKeyType&>& istream,
                                                     std::function<future<>(net::packet)> fn) {
        if(direction < _directions.size()) {
            async_flow_assert(!_directions[direction].input_sub);
        }
        else {
            _directions.resize(direction+1);
        }
        _directions[direction].input_sub.emplace(
                istream.listen([this, direction](net::packet pkt, FlowKeyType& key) {
            auto afi = _flow_table.find(key);
            if(afi == _flow_table.end()) {
                if(!_new_flow_q.full() &&
                   (_flow_table.size() <
                    Ppr::async_flow_config::max_flow_table_size) ){
                    auto impl_lw_ptr =
                            make_lw_shared<internal::async_flow_impl<Ppr>>>(
                                (*this), direction, key
                            );
                    _flow_table.insert({key, impl_lw_ptr});
                    _new_flow_q.push(new_async_flow(std::move(impl_lw_ptr)));
                }
            }
            else {
                afi->second->handle_packet_send(std::move(pkt), direction);
            }

            return make_ready_future<>();
        }));
        auto sub = _directions[direction].output_stream.listen(std::move(fn));
        return sub;
    }

    future<async_flow<Ppr>> on_new_flow() {
        return _new_flow_q.not_empty().then([this]{
           return make_ready_future<async_flow<Ppr>>(_new_flow_q.pop());
        });
    }

    future<> send(net::packet pkt, uint8_t direction) {
        return _directions[direction].output_stream.produce(std::move(pkt));
    }

    uint8_t get_reverse_direction(uint8_t direction) {
        return _directions[direction].reverse_direction;
    }

    void add_new_mapping_to_flow_table(FlowKeyType& flow_key,
                                       lw_shared_ptr<internal::async_flow_impl<Ppr>> impl_lw_ptr){
        _flow_table.insert({flow_key, impl_lw_ptr});
    }

    void remove_mapping_on_flow_table(FlowKeyType& flow_key) {
        _flow_table.erase(flow_key);
    }
};

} // namespace netstar

#endif
