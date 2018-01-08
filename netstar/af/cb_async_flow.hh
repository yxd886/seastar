#ifndef _CB_ASYNC_FLOW
#define _CB_ASYNC_FLOW

#include "net/packet.hh"

#include "core/shared_ptr.hh"
#include "core/future.hh"
#include "core/reactor.hh"
#include "core/timer.hh"
#include "core/queue.hh"

#include "netstar/af/async_flow_util.hh"
#include "netstar/extendable_buffer.hh"

#include "mica/util/hash.h"

#include <deque>
#include <experimental/optional>
#include <unordered_map>
#include <chrono>
#include <array>

using namespace seastar;

namespace netstar {

template<typename Ppr>
class cb_async_flow;
template<typename Ppr>
class cb_af_initial_context;
template<typename Ppr>
class cb_async_flow_manager;

namespace internal {

template<typename Ppr>
class cb_async_flow_impl;

template<typename Ppr>
class cb_async_flow_impl : public enable_lw_shared_from_this<cb_async_flow_impl<Ppr>>{
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;
    friend class cb_async_flow<Ppr>;

    cb_async_flow_manager<Ppr>& _manager;
    af_work_unit<Ppr> _client;
    unsigned _pkts_in_pipeline; // records number of the packets injected into the pipeline.
    bool _initial_context_destroyed;
    std::function<void()> _pkt_cb;
    bool _pkt_cb_registered;
    std::function<void()> _close_cb;
    uint64_t _flow_key_hash;
    uint32_t _flow_rss;

public:
    // Internal interfaces, exposed to async_flow and
    // async_flow manager.
    cb_async_flow_impl(cb_async_flow_manager<Ppr>& manager,
                       uint8_t client_direction,
                       FlowKeyType* client_flow_key)
        : _manager(manager)
        , _client(true, client_direction, [this](bool){this->ppr_passive_close();})
        , _pkts_in_pipeline(0)
        , _initial_context_destroyed(false)
        , _pkt_cb(nullptr)
        , _pkt_cb_registered(false)
        , _close_cb(nullptr) {
        _client.flow_key = *client_flow_key;
        _flow_key_hash = mica::util::hash(reinterpret_cast<char*>(client_flow_key), sizeof(FlowKeyType));
        _flow_rss = 0;
    }

    ~cb_async_flow_impl() {
        async_flow_debug("cb_asyn_flow_impl: deconstruction.\n");
    }

    void destroy_initial_context() {
        _initial_context_destroyed = true;
    }

    void handle_packet_send(net::packet pkt, uint8_t direction) {
        async_flow_debug("sd_async_flow_impl: handle_packet_send is called\n");
        async_flow_assert(direction == _client.direction);

        // a patch
        if(_flow_rss == 0 && pkt.rss_hash()){
            _flow_rss = pkt.rss_hash().value();
        }

        if( _pkts_in_pipeline >= Ppr::async_flow_config::max_event_context_queue_size ||
             _client.ppr_close ||
             !_initial_context_destroyed) {
            // Unconditionally drop the packet.
            return;
        }

        _pkts_in_pipeline += 1;

        if(_pkt_cb!=nullptr){
            if(!_client.cur_context) {
                async_flow_assert(_client.buffer_q.empty());
                auto fe = preprocess_packet(pkt);
                if(fe.no_event()) {
                    internal_packet_forward(std::move(pkt));
                }
                else{
                    _client.cur_context.emplace(std::move(pkt), fe, true);
                    _pkt_cb();
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

private:
    // Async loop initialization sequences after acquring the
    // initial packet context.
    void event_registration (EventEnumType ev) {
        _client.send_events.register_event(ev);
    }

    void event_unregistration (EventEnumType ev) {
        _client.send_events.unregister_event(ev);
    }

    void register_cbs(std::function<void()> pkt_cb,
                      std::function<void()> close_cb) {
        _pkt_cb = std::move(pkt_cb);
        _pkt_cb_registered = true;
        _close_cb = std::move(close_cb);
    }

    void unregister_packet_cb() {
        _pkt_cb_registered = false;
        while(!_client.buffer_q.empty()) {
            auto& next_pkt = _client.buffer_q.front();
            preprocess_and_forward(std::move(next_pkt.pkt));
            _client.buffer_q.pop_front();
        }
        _close_cb();
    }

    void forward_cur_packet() {
        internal_packet_forward(std::move(_client.cur_context->pkt));
        _client.cur_context = {};
        forward_drop_post_handler();
    }

    void drop_cur_packet() {
        _pkts_in_pipeline -= 1;
        _client.cur_context = {};
        forward_drop_post_handler();
    }

private:
    void forward_drop_post_handler() {
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
                _pkt_cb();
                return;
            }
        }

        if(_client.ppr_close == true) {
            _pkts_in_pipeline += 1;
            _client.cur_context.emplace(net::packet::make_null_packet(),
                                        filtered_events<EventEnumType>::make_close_event(),
                                        true);
            _pkt_cb();
        }
    }

    void send_packet_out(net::packet pkt, uint8_t direction){
        if(pkt) {
            _manager.send(std::move(pkt), direction);
            async_flow_debug("sd_async_flow_impl: send packet out from direction %d.\n", direction);
        }
    }

    void internal_packet_forward(net::packet pkt) {
        send_packet_out(std::move(pkt), _manager.get_reverse_direction(_client.direction));
        _pkts_in_pipeline -= 1;
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

    void close_ppr_and_remove_flow_key() {
        _client.ppr_close = true;
        if(_client.flow_key) {
            _manager.remove_mapping_on_flow_table(*(_client.flow_key));
            _client.flow_key = std::experimental::nullopt;
        }
    }

    void ppr_passive_close(){
        close_ppr_and_remove_flow_key();

        if(_pkt_cb_registered && !_client.cur_context) {
            _pkts_in_pipeline += 1;
            _client.cur_context.emplace(net::packet::make_null_packet(),
                                        filtered_events<EventEnumType>::make_close_event(),
                                        true);
            _pkt_cb();
        }
    }
};

} // namespace internal

template<typename Ppr>
class cb_async_flow{
    using impl_type = lw_shared_ptr<internal::cb_async_flow_impl<Ppr>>;
    using EventEnumType = typename Ppr::EventEnumType;
    impl_type _impl;
public:
    explicit cb_async_flow(impl_type impl)
        : _impl(std::move(impl)) {
    }
    ~cb_async_flow(){
    }
    cb_async_flow(const cb_async_flow& other) = delete;
    cb_async_flow(cb_async_flow&& other) noexcept
        : _impl(std::move(other._impl)) {
    }
    cb_async_flow& operator=(const cb_async_flow& other) = delete;
    cb_async_flow& operator=(cb_async_flow&& other) {
        if(&other != this){
            this->~cb_async_flow();
            new (this) cb_async_flow(std::move(other));
        }
        return *this;
    }

    void register_events(EventEnumType ev) {
        _impl->event_registration(ev);
    }

    void unregister_events(EventEnumType ev) {
        _impl->event_unregistration(ev);
    }

    net::packet& cur_packet() {
        return _impl->_client.cur_context.value().pkt;
    }

    filtered_events<EventEnumType> cur_event() {
        return _impl->_client.cur_context.value().fe;
    }

    uint32_t get_flow_rss () {
        return _impl->_flow_rss;
    }

    uint64_t get_flow_key_hash () {
        return _impl->_flow_key_hash;
    }

    // One shot interface, continuous call without shutting down
    // the current async loop will abort the program
    void register_cbs(std::function<void()> pkt_cb,
                      std::function<void()> close_cb) {
        _impl->register_cbs(std::move(pkt_cb), std::move(close_cb));
    }

    void unregister_packet_cb() {
        _impl->unregister_packet_cb();
    }

    void forward_cur_packet() {
        _impl->forward_cur_packet();
    }

    void drop_cur_packet(){
        _impl->drop_cur_packet();
    }
};

template<typename Ppr>
class cb_af_initial_context {
    using impl_type = lw_shared_ptr<internal::cb_async_flow_impl<Ppr>>;
    friend class cb_async_flow_manager<Ppr>;

    impl_type _impl_ptr;
    net::packet _pkt;
    uint8_t _direction;
private:
    explicit cb_af_initial_context(net::packet pkt, uint8_t direction,
                                   impl_type impl_ptr)
        : _impl_ptr(std::move(impl_ptr))
        , _pkt(std::move(pkt))
        , _direction(direction) {
    }
public:
    cb_af_initial_context(cb_af_initial_context&& other) noexcept
        : _impl_ptr(std::move(other._impl_ptr))
        , _pkt(std::move(other._pkt))
        , _direction(other._direction) {
    }
    cb_af_initial_context& operator=(cb_af_initial_context&& other) noexcept {
        if(&other != this) {
            this->~cb_af_initial_context();
            new (this) cb_af_initial_context(std::move(other));
        }
        return *this;
    }
    ~cb_af_initial_context(){
        if(_impl_ptr) {
            _impl_ptr->destroy_initial_context();
            _impl_ptr->handle_packet_send(std::move(_pkt), _direction);
        }
    }
    cb_async_flow<Ppr> get_cb_async_flow() {
        return cb_async_flow<Ppr>(_impl_ptr);
    }
};

template<typename Ppr>
class cb_async_flow_manager {
    using FlowKeyType = typename Ppr::FlowKeyType;
    using HashFunc = typename Ppr::HashFunc;
    using impl_type = lw_shared_ptr<internal::cb_async_flow_impl<Ppr>>;
    friend class internal::cb_async_flow_impl<Ppr>;

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

    std::unordered_map<FlowKeyType, lw_shared_ptr<internal::cb_async_flow_impl<Ppr>>, HashFunc> _flow_table;
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
        void register_to_manager(cb_async_flow_manager<Ppr>& manager,
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
    cb_af_initial_context<Ppr> get_initial_context() {
        async_flow_assert(!_new_ic_q.empty());
        auto qitem = _new_ic_q.pop();
        return cb_af_initial_context<Ppr>(std::move(qitem.pkt), qitem.direction, std::move(qitem.impl_ptr));
    }
    size_t peek_active_flow_num() {
        return _flow_table.size();
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
                            make_lw_shared<internal::cb_async_flow_impl<Ppr>>(
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
                                       lw_shared_ptr<internal::cb_async_flow_impl<Ppr>> impl_lw_ptr){
        assert(_flow_table.insert({flow_key, impl_lw_ptr}).second);
    }
    void remove_mapping_on_flow_table(FlowKeyType& flow_key) {
        _flow_table.erase(flow_key);
    }
};

} // namespace netstar

#endif
