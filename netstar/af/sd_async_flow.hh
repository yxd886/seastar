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

template<typename Ppr>
class sd_async_flow;
template<typename Ppr>
class sd_af_initial_context;
template<typename Ppr>
class sd_async_flow_manager;

namespace internal {

template<typename Ppr>
class sd_async_flow_impl;

template<typename Ppr>
class sd_async_flow_impl : public enable_lw_shared_from_this<sd_async_flow_impl<Ppr>>{
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;
    friend class sd_async_flow<Ppr>;

    sd_async_flow_manager<Ppr>& _manager;
    af_work_unit<Ppr> _client;
    unsigned _pkts_in_pipeline; // records number of the packets injected into the pipeline.
    bool _initial_context_destroyed;

private:

    void close_ppr_and_remove_flow_key() {
        _client.ppr_close = true;
        if(_client.flow_key) {
            _manager.remove_mapping_on_flow_table(*(_client.flow_key));
            _client.flow_key = std::experimental::nullopt;
        }
    }

    filtered_events<EventEnumType> preprocess_packet(net::packet& pkt) {
        generated_events<EventEnumType> ge = _client.ppr.handle_packet_send(pkt);
        filtered_events<EventEnumType> fe = _client.send_events.filter(ge);

        if(fe.on_close_event()) {
            async_flow_debug("sd_async_flow_impl: Close preprocessor and remove flow key.\n");
            close_ppr_and_remove_flow_key();
        }
        return fe;
    }

    void preprocess_and_forward(net::packet pkt) {
        auto ge = _client.ppr.handle_packet_send(pkt);
        if(ge.on_close_event()){
            async_flow_debug("sd_async_flow_impl: Close preprocessor and remove flow key.\n");
            close_ppr_and_remove_flow_key();
        }
        internal_packet_forward(std::move(pkt));
    }

    void internal_packet_forward(net::packet pkt) {
        send_packet_out(std::move(pkt), _manager.get_reverse_direction(_client.direction));
        _pkts_in_pipeline -= 1;
    }

private:
    // Critical functions for controlling the loop
    void close_async_loop() {
        _client.loop_fn = nullptr;
        while(!_client.buffer_q.empty()) {
            auto& next_pkt = _client.buffer_q.front();
            preprocess_and_forward(std::move(next_pkt.pkt));
            _client.buffer_q.pop_front();
        }
        _client.async_loop_quit_pr->set_value();
        _client.async_loop_quit_pr = {};
    }

    void loop_fn_post_handler(af_action action) {
        auto& context = _client.cur_context.value();

        if(action == af_action::forward || action == af_action::close_forward) {
            internal_packet_forward(std::move(context.pkt));
        }

        if(action == af_action::drop || action == af_action::close_drop) {
            _pkts_in_pipeline -= 1;
        }

        _client.cur_context = {};

        if(action == af_action::close_drop || action == af_action::close_forward) {
            close_async_loop();
            return;
        }

        while(!_client.buffer_q.empty()) {
            auto& next_pkt = _client.buffer_q.front();
            auto fe = preprocess_packet(next_pkt.pkt);
            if(fe.no_event()) {
                internal_packet_forward(std::move(next_pkt.pkt));
                _client.buffer_q.pop_front();
            }
            else{
                _client.cur_context.emplace(std::move(next_pkt.pkt),
                                            fe,
                                            next_pkt.is_send);
                _client.buffer_q.pop_front();
                invoke_async_loop();
                return;
            }
        }

        if(_client.ppr_close == true) {
            _pkts_in_pipeline += 1;
            _client.cur_context.emplace(net::packet::make_null_packet(),
                                        filtered_events<EventEnumType>::make_close_event(),
                                        true);
            invoke_async_loop();
        }
    }

    // invoke_async_loop can be as simple as this:
    // working_unit.loop_fn().then([this, is_client](af_action action){
    //    loop_fn_post_handler(is_client, action);
    // })
    // However, this will not handle exceptions thrown when executing
    // loop_fn. In order to catch the exception, we replace the
    // implementation with this.
    void invoke_async_loop() {
        _client.loop_fn().then_wrapped([this](auto&& f){
            try {
                auto action = f.get0();
                this->loop_fn_post_handler(action);
            }
            catch(...){
                this->_initial_context_destroyed = false;
                _client.cur_context = {};
                _client.loop_fn = nullptr;
                while(!_client.buffer_q.empty()) {
                    _client.buffer_q.pop_front();
                }
                _client.async_loop_quit_pr->set_exception(std::current_exception());
                _client.async_loop_quit_pr = {};
            }
        });
    }

public:
    // Internal interfaces, exposed to async_flow and
    // async_flow manager.
    sd_async_flow_impl(sd_async_flow_manager<Ppr>& manager,
                       uint8_t client_direction,
                       FlowKeyType* client_flow_key)
        : _manager(manager)
        , _client(true, client_direction)
        , _pkts_in_pipeline(0)
        , _initial_context_destroyed(false) {
        _client.flow_key = *client_flow_key;
    }

    ~sd_async_flow_impl() {
        async_flow_debug("sd_async_flow_impl: deconstruction.\n");
        async_flow_assert(!_client.cur_context);
        async_flow_assert(_pkts_in_pipeline == 0);
    }

    void destroy_initial_context() {
        _initial_context_destroyed = true;
    }

    void handle_packet_send(net::packet pkt, uint8_t direction) {
        async_flow_debug("sd_async_flow_impl: handle_packet_send is called\n");
        async_flow_assert(direction == _client.direction);

        if( _pkts_in_pipeline >= Ppr::async_flow_config::max_event_context_queue_size ||
             _client.ppr_close ||
             !_initial_context_destroyed) {
            // Unconditionally drop the packet.
            return;
        }

        _pkts_in_pipeline += 1;

        if(_client.loop_fn != nullptr) {
            if(!_client.cur_context) {
                async_flow_assert(_client.buffer_q.empty());
                auto fe = preprocess_packet(pkt);
                if(fe.no_event()) {
                    internal_packet_forward(std::move(pkt));
                }
                else{
                    _client.cur_context.emplace(std::move(pkt), fe, true);
                    invoke_async_loop();
                }
            }
            else{
                _client.buffer_q.emplace_back(std::move(pkt), true);
            }
        }
        else{
            preprocess_and_forward(std::move(pkt));
        }
    }

    void send_packet_out(net::packet pkt, uint8_t direction){
        if(pkt) {
            _manager.send(std::move(pkt), direction);
            async_flow_debug("sd_async_flow_impl: send packet out from direction %d.\n", direction);
        }
    }

    void ppr_passive_close(){
        close_ppr_and_remove_flow_key();

        if(_client.loop_fn != nullptr && !_client.cur_context) {
            _pkts_in_pipeline += 1;
            _client.cur_context.emplace(net::packet::make_null_packet(),
                                        filtered_events<EventEnumType>::make_close_event(),
                                        true);
            invoke_async_loop();
        }
    }

private:
    // Async loop initialization sequences after acquring the
    // initial packet context.
    void event_registration (EventEnumType ev) {
        _client.send_events.register_event(ev);
    }

    void event_unregistration (EventEnumType ev) {
        _client.send_events.unregister_event(ev);
    }

    future<> run_async_loop(std::function<future<af_action>()> fn) {
        async_flow_assert(_client.loop_fn==nullptr &&
                          !_client.cur_context &&
                          !_client.async_loop_quit_pr &&
                          !_client.ppr_close);

        _client.loop_fn = std::move(fn);
        _client.async_loop_quit_pr = promise<>();
        return _client.async_loop_quit_pr->get_future();
    }
};

} // namespace internal

template<typename Ppr>
class sd_async_flow{
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
        _impl->event_registration(ev);
    }

    void unregister_events(EventEnumType ev) {
        _impl->event_unregistration(ev);
    }
    future<> run_async_loop(std::function<future<af_action>()> fn) {
        return _impl->run_async_loop(std::move(fn));
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
    sd_async_flow<Ppr> get_client_sd_async_flow() {
        return sd_async_flow<Ppr>(_impl_ptr);
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
                            make_lw_shared<internal::sd_async_flow_impl<Ppr>>(
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
