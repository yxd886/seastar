#ifndef _ASYNC_FLOW_HH
#define _ASYNC_FLOW_HH

#include "net/packet.hh"

#include "core/shared_ptr.hh"
#include "core/future.hh"
#include "core/reactor.hh"
#include "core/timer.hh"
#include "core/queue.hh"

#include "netstar/af/async_flow_util.hh"
#include "netstar/af/async_flow_impl_base.hh"

#include <deque>
#include <experimental/optional>
#include <unordered_map>
#include <chrono>
#include <array>

using namespace seastar;

namespace netstar {

template<typename Ppr, af_side Side>
class async_flow;
template<typename Ppr>
class af_initial_context;
template<typename Ppr>
class async_flow_manager;
template<typename Ppr>
using client_async_flow = async_flow<Ppr, af_side::client>;
template<typename Ppr>
using server_async_flow = async_flow<Ppr, af_side::server>;

namespace internal {

template<typename Ppr>
class async_flow_impl;

template<typename Ppr>
class async_flow_impl : public enable_lw_shared_from_this<async_flow_impl<Ppr>>
                      , public async_flow_impl_base<Ppr> {
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;
    friend class async_flow<Ppr, af_side::client>;
    friend class async_flow<Ppr, af_side::server>;
    friend class async_flow_manager<Ppr>;
    friend class af_initial_context<Ppr>;
    friend class lw_shared_ptr<async_flow_impl>;

    async_flow_manager<Ppr>& _manager;
    af_work_unit<Ppr> _client;
    af_work_unit<Ppr> _server;

protected:
    // General helper utility function, useful for reducing the
    // boilerplates used in this class.
    af_work_unit<Ppr>& get_work_unit(bool is_client) override {
        return is_client ? _client : _server;
    }

    void internal_packet_forward(net::packet pkt, bool is_client, bool is_send) override {
        if(is_send) {
            handle_packet_recv(std::move(pkt), !is_client);
        }
        else{
            send_packet_out(std::move(pkt), is_client);
            this->_pkts_in_pipeline -= 1;
        }
    }

    void close_ppr_and_remove_flow_key(af_work_unit<Ppr>& work_unit) override{
        work_unit.ppr_close = true;
        if(work_unit.flow_key) {
            _manager.remove_mapping_on_flow_table(*(work_unit.flow_key));
            work_unit.flow_key = std::experimental::nullopt;
        }
    }

    void send_packet_out(net::packet pkt, bool is_client) override {
        if(pkt) {
            auto& working_unit = get_work_unit(is_client);
            _manager.send(std::move(pkt), working_unit.direction);
            async_flow_debug("async_flow_impl: send packet out from direction %d.\n", working_unit.direction);
        }
    }

    void handle_packet_recv(net::packet pkt, bool is_client) override {
        async_flow_debug("async_flow_impl: handle_packet_recv is called\n");

        auto& working_unit = get_work_unit(is_client);

        if(working_unit.ppr_close || !pkt) {
            // Unconditionally drop the packet.
            this->_pkts_in_pipeline -= 1;
            return;
        }

        if(!working_unit.flow_key) {
            async_flow_assert(!is_client);
            FlowKeyType flow_key = working_unit.ppr.get_reverse_flow_key(pkt);
            _manager.add_new_mapping_to_flow_table(flow_key, this->shared_from_this());
        }

        this->action_after_packet_handle(working_unit, std::move(pkt),
                                         is_client, false);
    }

    bool check_is_client(uint8_t direction) override{
        return _client.direction == direction;
    }

private:
    // Internal interfaces, exposed to async_flow and
    // async_flow manager.
    async_flow_impl(async_flow_manager<Ppr>& manager,
                    uint8_t client_direction,
                    FlowKeyType* client_flow_key)
        : _manager(manager)
        , _client(true, client_direction)
        , _server(false, manager.get_reverse_direction(client_direction)) {
        _client.flow_key = *client_flow_key;
        this->_pkts_in_pipeline = 0;
        this->_initial_context_destroyed = false;
    }

public:
    ~async_flow_impl() {
        async_flow_debug("async_flow_impl: deconstruction.\n");
        async_flow_assert(!_client.cur_context);
        async_flow_assert(!_server.cur_context);
        async_flow_assert(this->_pkts_in_pipeline == 0);
    }
};

} // namespace internal

template<typename Ppr, af_side Side>
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

    void register_events(af_send_recv sr, EventEnumType ev) {
        _impl->event_registration(static_cast<bool>(Side), static_cast<bool>(sr), ev);
    }

    void unregister_events(af_send_recv sr, EventEnumType ev) {
        _impl->event_unregistration(static_cast<bool>(Side), static_cast<bool>(sr), ev);
    }
    future<> run_async_loop(std::function<future<af_action>()> fn) {
        return _impl->run_async_loop(static_cast<bool>(Side), std::move(fn));
    }
};

template<typename Ppr>
class af_initial_context {
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    friend class async_flow_manager<Ppr>;

    impl_type _impl_ptr;
    net::packet _pkt;
    uint8_t _direction;
private:
    explicit af_initial_context(net::packet pkt, uint8_t direction,
                                impl_type impl_ptr)
        : _impl_ptr(std::move(impl_ptr))
        , _pkt(std::move(pkt))
        , _direction(direction) {
    }
public:
    af_initial_context(af_initial_context&& other) noexcept
        : _impl_ptr(std::move(other._impl_ptr))
        , _pkt(std::move(other._pkt))
        , _direction(other._direction) {
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
            _impl_ptr->destroy_initial_context();
            _impl_ptr->handle_packet_send(std::move(_pkt), _direction);
        }
    }
    client_async_flow<Ppr> get_client_async_flow() {
        return client_async_flow<Ppr>(_impl_ptr);
    }
    server_async_flow<Ppr> get_server_async_flow() {
        return server_async_flow<Ppr>(_impl_ptr);
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
