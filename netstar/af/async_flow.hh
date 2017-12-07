#ifndef _ASYNC_FLOW_HH
#define _ASYNC_FLOW_HH

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

#define ASYNC_FLOW_DEBUG

template <typename... Args>
void async_flow_debug(const char* fmt, Args&&... args) {
#ifdef ASYNC_FLOW_DEBUG
    print(fmt, std::forward<Args>(args)...);
#endif
}

template<typename Ppr>
class async_flow;
template<typename Ppr>
class af_initial_context;
template<typename Ppr>
class async_flow_manager;

namespace internal {

template<typename Ppr>
class async_flow_impl;

template<typename Ppr>
class async_flow_impl : public enable_lw_shared_from_this<async_flow_impl<Ppr>>{
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;

public:
    // Internal interfaces, exposed to async_flow and
    // async_flow manager.
    async_flow_impl(async_flow_manager<Ppr>& manager,
                    uint8_t client_direction,
                    FlowKeyType* client_flow_key) {

    }

    void handle_packet_send(net::packet pkt, uint8_t direction) {
        async_flow_debug("async_flow_impl: handle_packet_send is called\n");
    }
};

} // namespace internal

template<typename Ppr>
class async_flow{
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    using EventEnumType = typename Ppr::EventEnumType;
    impl_type _impl;
public:
    explicit async_flow(impl_type impl)
        : _impl(std::move(impl)) {
    }
    ~async_flow(){
    }
    async_flow(const async_flow& other) = delete;
    async_flow(async_flow&& other) noexcept
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

    future<> on_client_side_events() {
        // return _impl->on_new_events(true);
        return make_ready_future<>();
    }

    future<> on_server_side_events() {
        // return _impl->on_new_events(false);
        return make_ready_future<>();
    }

    void register_client_events(af_send_recv sr, EventEnumType ev) {
        // _impl->event_registration(true, static_cast<bool>(sr), ev);
    }

    void register_server_events(af_send_recv sr, EventEnumType ev) {
        // _impl->event_registration(false, static_cast<bool>(sr), ev);
    }
};

template<typename Ppr>
class af_initial_context {
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    friend class async_flow_manager<Ppr>;

    impl_type _impl_ptr;
    net::packet _pkt;
    uint8_t _direction;
    bool _extract_async_flow;
    int _move_construct_count;
private:
    explicit af_initial_context(net::packet pkt, uint8_t direction,
                                impl_type impl_ptr)
        : _impl_ptr(std::move(impl_ptr))
        , _pkt(std::move(pkt))
        , _direction(direction)
        , _extract_async_flow(false)
        , _move_construct_count(0)
        {
    }
public:
    af_initial_context(af_initial_context&& other) noexcept
        : _impl_ptr(std::move(other._impl_ptr))
        , _pkt(std::move(other._pkt))
        , _direction(other._direction)
        , _extract_async_flow(other._extract_async_flow)
        , _move_construct_count(other._move_construct_count) {
        _move_construct_count += 1;
    }
    af_initial_context& operator=(af_initial_context&& other) noexcept {
        if(&other != this) {
            this->~af_initial_context();
            new (this) af_initial_context(std::move(other));
        }
        return *this;
    }
    ~af_initial_context(){
        if(_impl_ptr) {
            async_flow_assert(_move_construct_count == 0);
            // _impl_ptr->destroy_initial_context();
            _impl_ptr->handle_packet_send(std::move(_pkt), _direction);
        }
    }
    async_flow<Ppr> get_async_flow() {
        async_flow_assert(!_extract_async_flow);
        _extract_async_flow = true;
        return async_flow<Ppr>(_impl_ptr);
    }
};

template<typename Ppr>
class async_flow_manager {
    using FlowKeyType = typename Ppr::FlowKeyType;
    using HashFunc = typename Ppr::HashFunc;
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    friend class internal::async_flow_impl<Ppr>;

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


    std::unordered_map<FlowKeyType, lw_shared_ptr<internal::async_flow_impl<Ppr>>, HashFunc> _flow_table;
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
        void register_to_manager(async_flow_manager<Ppr>& manager,
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
    af_initial_context<Ppr> get_initial_context() {
        async_flow_assert(!_new_ic_q.empty());
        auto qitem = _new_ic_q.pop();
        return af_initial_context<Ppr>(std::move(qitem.pkt), qitem.direction, std::move(qitem.impl_ptr));
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
                                       lw_shared_ptr<internal::async_flow_impl<Ppr>> impl_lw_ptr){
        assert(_flow_table.insert({flow_key, impl_lw_ptr}).second);
    }
    void remove_mapping_on_flow_table(FlowKeyType& flow_key) {
        _flow_table.erase(flow_key);
    }
};

} // namespace netstar

#endif
