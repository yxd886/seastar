#ifndef _SD_ASYNC_FLOW
#define _SD_ASYNC_FLOW

#include "net/packet.hh"

#include "core/shared_ptr.hh"
#include "core/future.hh"
#include "core/reactor.hh"
#include "core/timer.hh"
#include "core/queue.hh"

#include "netstar/af/async_flow_util.hh"

#include <deque>
#include <experimental/optional>
#include <unordered_map>
#include <chrono>
#include <array>

using namespace seastar;

namespace netstar {

template<typename Ppr, af_side Side>
class sd_async_flow;
template<typename Ppr>
class sd_af_initial_context;
template<typename Ppr>
class sd_async_flow_manager;

template<typename Ppr>
using client_sd_async_flow = sd_async_flow<Ppr, af_side::client>;

namespace internal {

template<typename Ppr>
class sd_async_flow_impl;

template<typename Ppr>
class sd_async_flow_impl : public enable_lw_shared_from_this<sd_async_flow_impl<Ppr>>{
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;
    friend class sd_async_flow<Ppr, af_side::client>;

    sd_async_flow_manager<Ppr>& _manager;
    af_work_unit<Ppr> _client;
    af_work_unit<Ppr> _server;
    unsigned _pkts_in_pipeline; // records number of the packets injected into the pipeline.
    bool _initial_context_destroyed;

private:
    // General helper utility function, useful for reducing the
    // boilerplates used in this class.
    af_work_unit<Ppr>& get_work_unit(bool is_client){
        return _client;
    }

    void close_ppr_and_remove_flow_key(af_work_unit<Ppr>& work_unit) {
        work_unit.ppr_close = true;
        if(work_unit.flow_key) {
            _manager.remove_mapping_on_flow_table(*(work_unit.flow_key));
            work_unit.flow_key = std::experimental::nullopt;
        }
    }

    filtered_events<EventEnumType> preprocess_packet(
            af_work_unit<Ppr>& working_unit, net::packet& pkt, bool is_send) {
        generated_events<EventEnumType> ge =
                is_send ?
                working_unit.ppr.handle_packet_send(pkt) :
                working_unit.ppr.handle_packet_recv(pkt);
        filtered_events<EventEnumType> fe =
                is_send ?
                working_unit.send_events.filter(ge) :
                working_unit.recv_events.filter(ge);

        if(fe.on_close_event()) {
            async_flow_debug("async_flow_impl: Close preprocessor and remove flow key.\n");
            close_ppr_and_remove_flow_key(working_unit);
        }
        return fe;
    }

    void preprocess_and_forward(net::packet pkt, bool is_client, bool is_send) {
        auto& working_unit = get_work_unit(is_client);
        auto ge = is_send ? working_unit.ppr.handle_packet_send(pkt) :
                            working_unit.ppr.handle_packet_recv(pkt);
        if(ge.on_close_event()){
            async_flow_debug("async_flow_impl: Close preprocessor and remove flow key.\n");
            close_ppr_and_remove_flow_key(working_unit);
        }
        if(is_send) {
            handle_packet_recv(std::move(pkt), !is_client);
        }
        else{
            send_packet_out(std::move(pkt), is_client);
            _pkts_in_pipeline -= 1;
        }
    }

    void internal_packet_forward(net::packet pkt, bool is_client, bool is_send) {
        if(is_send) {
            handle_packet_recv(std::move(pkt), !is_client);
        }
        else{
            send_packet_out(std::move(pkt), is_client);
            _pkts_in_pipeline -= 1;
        }
    }

    // invoke_async_loop can be as simple as this:
    // working_unit.loop_fn().then([this, is_client](af_action action){
    //    loop_fn_post_handler(is_client, action);
    // })
    // However, this will not handle exceptions thrown when executing
    // loop_fn. In order to catch the exception, we replace the
    // implementation with this.
    void invoke_async_loop(af_work_unit<Ppr>& working_unit, bool is_client) {
        working_unit.loop_fn().then_wrapped([this, is_client](auto&& f){
            try {
                auto action = f.get0();
                this->loop_fn_post_handler(is_client, action);
            }
            catch(...){
                this->_initial_context_destroyed = false;
                auto& working_unit = this->get_work_unit(is_client);
                working_unit.cur_context = {};
                working_unit.loop_fn = nullptr;
                while(!working_unit.buffer_q.empty()) {
                    working_unit.buffer_q.pop_front();
                }
                working_unit.async_loop_quit_pr->set_exception(std::current_exception());
                working_unit.async_loop_quit_pr = {};
            }
        });
    }

    void action_after_packet_handle(af_work_unit<Ppr>& working_unit,
                                    net::packet pkt,
                                    bool is_client, bool is_send) {
        if(working_unit.loop_fn != nullptr) {
            if(!working_unit.cur_context) {
                async_flow_assert(working_unit.buffer_q.empty());
                auto fe = preprocess_packet(working_unit, pkt, is_send);
                if(fe.no_event()) {
                    internal_packet_forward(std::move(pkt), is_client, is_send);
                }
                else{
                    working_unit.cur_context.emplace(std::move(pkt), fe, is_send);
                    invoke_async_loop(working_unit, is_client);
                }
            }
            else{
                working_unit.buffer_q.emplace_back(std::move(pkt), is_send);
            }
        }
        else{
            preprocess_and_forward(std::move(pkt), is_client, is_send);
        }
    }

private:
    // Critical functions for controlling the loop
    void close_async_loop(bool is_client) {
        auto& working_unit = get_work_unit(is_client);
        working_unit.loop_fn = nullptr;
        while(!working_unit.buffer_q.empty()) {
            auto& next_pkt = working_unit.buffer_q.front();
            preprocess_and_forward(std::move(next_pkt.pkt), is_client, next_pkt.is_send);
            working_unit.buffer_q.pop_front();
        }
        working_unit.async_loop_quit_pr->set_value();
        working_unit.async_loop_quit_pr = {};
    }

    void loop_fn_post_handler(bool is_client, af_action action) {
        auto& working_unit = get_work_unit(is_client);
        auto& context = working_unit.cur_context.value();

        if(action == af_action::forward || action == af_action::close_forward) {
            internal_packet_forward(std::move(context.pkt), is_client, context.is_send);
        }

        if(action == af_action::drop || action == af_action::close_drop) {
            _pkts_in_pipeline -= 1;
        }

        working_unit.cur_context = {};

        if(action == af_action::close_drop || action == af_action::close_forward) {
            close_async_loop(is_client);
            return;
        }

        while(!working_unit.buffer_q.empty()) {
            auto& next_pkt = working_unit.buffer_q.front();
            auto fe = preprocess_packet(working_unit,
                    next_pkt.pkt, next_pkt.is_send);
            if(fe.no_event()) {
                internal_packet_forward(std::move(next_pkt.pkt),
                                        is_client,
                                        next_pkt.is_send);
                working_unit.buffer_q.pop_front();
            }
            else{
                working_unit.cur_context.emplace(std::move(next_pkt.pkt),
                                                 fe,
                                                 next_pkt.is_send);
                working_unit.buffer_q.pop_front();
                invoke_async_loop(working_unit, is_client);
                return;
            }
        }

        if(working_unit.ppr_close == true) {
            _pkts_in_pipeline += 1;
            working_unit.cur_context.emplace(net::packet::make_null_packet(),
                                             filtered_events<EventEnumType>::make_close_event(),
                                             true);
            invoke_async_loop(working_unit, is_client);
        }
    }

public:
    // Internal interfaces, exposed to async_flow and
    // async_flow manager.
    sd_async_flow_impl(sd_async_flow_manager<Ppr>& manager,
                       uint8_t client_direction,
                       FlowKeyType* client_flow_key)
        : _manager(manager)
        , _client(true, client_direction)
        , _server(false, manager.get_reverse_direction(client_direction))
        , _pkts_in_pipeline(0)
        , _initial_context_destroyed(false) {
        _client.flow_key = *client_flow_key;
    }

    ~sd_async_flow_impl() {
        async_flow_debug("async_flow_impl: deconstruction.\n");
        async_flow_assert(!_client.cur_context);
        async_flow_assert(!_server.cur_context);
        async_flow_assert(_pkts_in_pipeline == 0);
    }

    void destroy_initial_context() {
        _initial_context_destroyed = true;
    }

    void handle_packet_send(net::packet pkt, uint8_t direction) {
        async_flow_debug("async_flow_impl: handle_packet_send is called\n");

        bool is_client = (direction == _client.direction);
        auto& working_unit = get_work_unit(is_client);

        if( _pkts_in_pipeline >= Ppr::async_flow_config::max_event_context_queue_size ||
             working_unit.ppr_close ||
             !_initial_context_destroyed) {
            // Unconditionally drop the packet.
            return;
        }

        _pkts_in_pipeline += 1;

        action_after_packet_handle(working_unit, std::move(pkt),
                                   is_client, true);
    }

    void handle_packet_recv(net::packet pkt, bool is_client){
        async_flow_debug("async_flow_impl: handle_packet_recv is called\n");

        auto& working_unit = get_work_unit(is_client);

        if(working_unit.ppr_close || !pkt) {
            // Unconditionally drop the packet.
            _pkts_in_pipeline -= 1;
            return;
        }

        if(!working_unit.flow_key) {
            async_flow_assert(!is_client);
            FlowKeyType flow_key = working_unit.ppr.get_reverse_flow_key(pkt);
            _manager.add_new_mapping_to_flow_table(flow_key, this->shared_from_this());
        }

        action_after_packet_handle(working_unit, std::move(pkt),
                                   is_client, false);
    }

    void send_packet_out(net::packet pkt, bool is_client){
        if(pkt) {
            auto& working_unit = get_work_unit(is_client);
            _manager.send(std::move(pkt), working_unit.direction);
            async_flow_debug("async_flow_impl: send packet out from direction %d.\n", working_unit.direction);
        }
    }

    void ppr_passive_close(bool is_client){
        auto& working_unit = get_work_unit(is_client);
        close_ppr_and_remove_flow_key(working_unit);

        if(working_unit.loop_fn != nullptr && !working_unit.cur_context) {
            _pkts_in_pipeline += 1;
            working_unit.cur_context.emplace(net::packet::make_null_packet(),
                                             filtered_events<EventEnumType>::make_close_event(),
                                             true);
            invoke_async_loop(working_unit, is_client);
        }
    }

private:
    // Async loop initialization sequences after acquring the
    // initial packet context.
    void event_registration (bool is_client, bool is_send, EventEnumType ev) {
        auto& working_unit = get_work_unit(is_client);
        auto& events = is_send ? working_unit.send_events : working_unit.recv_events;
        events.register_event(ev);
    }

    void event_unregistration (bool is_client, bool is_send, EventEnumType ev) {
        auto& working_unit = get_work_unit(is_client);
        auto& events = is_send ? working_unit.send_events : working_unit.recv_events;
        events.unregister_event(ev);
    }

    future<> run_async_loop(bool is_client, std::function<future<af_action>()> fn) {
        auto& working_unit = get_work_unit(is_client);

        async_flow_assert(working_unit.loop_fn==nullptr && !working_unit.cur_context && !working_unit.async_loop_quit_pr);

        working_unit.loop_fn = std::move(fn);
        working_unit.async_loop_quit_pr = promise<>();
        return working_unit.async_loop_quit_pr->get_future();
    }
};

} // namespace internal

template<typename Ppr>
class sd_async_flow<Ppr, af_side::client>{
    using impl_type = lw_shared_ptr<internal::sd_async_flow_impl<Ppr>>;
    using EventEnumType = typename Ppr::EventEnumType;
    impl_type _impl;
public:
    explicit sd_async_flow(impl_type impl)
        : _impl(std::move(impl)) {
    }
    ~sd_async_flow(){
    }
    sd_async_flow(const sd_async_flow& other) = delete;
    sd_async_flow(sd_async_flow&& other) noexcept
        : _impl(std::move(other._impl)) {
    }
    sd_async_flow& operator=(const sd_async_flow& other) = delete;
    sd_async_flow& operator=(sd_async_flow&& other) {
        if(&other != this){
            this->~sd_async_flow();
            new (this) sd_async_flow(std::move(other));
        }
        return *this;
    }

    void register_events(EventEnumType ev) {
        _impl->event_registration(true, true, ev);
    }

    void unregister_events(EventEnumType ev) {
        _impl->event_unregistration(true, true, ev);
    }
    future<> run_async_loop(std::function<future<af_action>()> fn) {
        return _impl->run_async_loop(true, std::move(fn));
    }
};

template<typename Ppr>
class sd_af_initial_context {
    using impl_type = lw_shared_ptr<internal::sd_async_flow_impl<Ppr>>;
    friend class sd_async_flow_manager<Ppr>;

    impl_type _impl_ptr;
    net::packet _pkt;
    uint8_t _direction;
private:
    explicit sd_af_initial_context(net::packet pkt, uint8_t direction,
                                   impl_type impl_ptr)
        : _impl_ptr(std::move(impl_ptr))
        , _pkt(std::move(pkt))
        , _direction(direction) {
    }
public:
    sd_af_initial_context(sd_af_initial_context&& other) noexcept
        : _impl_ptr(std::move(other._impl_ptr))
        , _pkt(std::move(other._pkt))
        , _direction(other._direction) {
    }
    sd_af_initial_context& operator=(sd_af_initial_context&& other) noexcept {
        if(&other != this) {
            this->~sd_af_initial_context();
            new (this) sd_af_initial_context(std::move(other));
        }
        return *this;
    }
    ~sd_af_initial_context(){
        if(_impl_ptr) {
            _impl_ptr->destroy_initial_context();
            _impl_ptr->handle_packet_send(std::move(_pkt), _direction);
        }
    }
};

template<typename Ppr>
class sd_async_flow_manager {
    using FlowKeyType = typename Ppr::FlowKeyType;
    using HashFunc = typename Ppr::HashFunc;
    using impl_type = lw_shared_ptr<internal::sd_async_flow_impl<Ppr>>;
    friend class internal::sd_async_flow_impl<Ppr>;

    struct internal_io_direction {
        std::experimental::optional<subscription<net::packet, FlowKeyType*>> input_sub;
        stream<net::packet> output_stream;
        uint8_t reverse_direction;
        internal_io_direction()
            : reverse_direction(0) {
        }
    };
    struct queue_item {
        impl_type impl_ptr;
        net::packet pkt;
        uint8_t direction;

        queue_item(impl_type impl_ptr_arg,
                   net::packet pkt_arg,
                   uint8_t direction_arg)
            : impl_ptr(std::move(impl_ptr_arg))
            , pkt(std::move(pkt_arg))
            , direction(direction_arg) {
        }
    };

    std::unordered_map<FlowKeyType, lw_shared_ptr<internal::sd_async_flow_impl<Ppr>>, HashFunc> _flow_table;
    std::array<internal_io_direction, Ppr::async_flow_config::max_directions> _directions;
    seastar::queue<queue_item> _new_ic_q{Ppr::async_flow_config::new_flow_queue_size};

public:
    class external_io_direction {
        std::experimental::optional<subscription<net::packet>> _receive_sub;
        stream<net::packet, FlowKeyType*> _send_stream;
        uint8_t _direction;
        bool _is_registered;
    public:
        external_io_direction(uint8_t direction)
            : _direction(direction)
            , _is_registered(false) {
        }
        void register_to_manager(sd_async_flow_manager<Ppr>& manager,
                                 std::function<future<>(net::packet)> receive_fn,
                                 external_io_direction& reverse_io) {
            async_flow_assert(!_is_registered);
            _is_registered = true;
            _receive_sub.emplace(
                    manager.direction_registration(_direction, reverse_io.get_direction(),
                                                   _send_stream, std::move(receive_fn))
            );
        }
        uint8_t get_direction() {
            return _direction;
        }
        stream<net::packet, FlowKeyType*>& get_send_stream(){
            return _send_stream;
        }
    };
    future<> on_new_initial_context() {
        return _new_ic_q.not_empty();
    }
    sd_af_initial_context<Ppr> get_initial_context() {
        async_flow_assert(!_new_ic_q.empty());
        auto qitem = _new_ic_q.pop();
        return sd_af_initial_context<Ppr>(std::move(qitem.pkt), qitem.direction, std::move(qitem.impl_ptr));
    }

private:
    subscription<net::packet> direction_registration(uint8_t direction,
                                                     uint8_t reverse_direction,
                                                     stream<net::packet, FlowKeyType*>& istream,
                                                     std::function<future<>(net::packet)> fn) {
        async_flow_assert(direction < _directions.size());
        async_flow_assert(!_directions[direction].input_sub);

        _directions[direction].input_sub.emplace(
                istream.listen([this, direction](net::packet pkt, FlowKeyType* key) {
            async_flow_debug("Receive a new packet from direction %d\n", direction);
            auto afi = _flow_table.find(*key);
            if(afi == _flow_table.end()) {
                if(!_new_ic_q.full() &&
                   (_flow_table.size() <
                    Ppr::async_flow_config::max_flow_table_size) ){
                    auto impl_lw_ptr =
                            make_lw_shared<internal::async_flow_impl<Ppr>>(
                                (*this), direction, key
                            );
                    auto succeed = _flow_table.insert({*key, impl_lw_ptr}).second;
                    assert(succeed);
                    _new_ic_q.push(queue_item(std::move(impl_lw_ptr), std::move(pkt), direction));
                }
            }
            else {
                afi->second->handle_packet_send(std::move(pkt), direction);
            }

            return make_ready_future<>();
        }));
        auto sub = _directions[direction].output_stream.listen(std::move(fn));
        _directions[direction].reverse_direction = reverse_direction;
        return sub;
    }
    future<> send(net::packet pkt, uint8_t direction) {
        return _directions[direction].output_stream.produce(std::move(pkt));
    }
    uint8_t get_reverse_direction(uint8_t direction) {
        return _directions[direction].reverse_direction;
    }
    void add_new_mapping_to_flow_table(FlowKeyType& flow_key,
                                       lw_shared_ptr<internal::sd_async_flow_impl<Ppr>> impl_lw_ptr){
        assert(_flow_table.insert({flow_key, impl_lw_ptr}).second);
    }
    void remove_mapping_on_flow_table(FlowKeyType& flow_key) {
        _flow_table.erase(flow_key);
    }
};

} // namespace netstar

#endif
