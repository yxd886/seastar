#ifndef _ASYNC_FLOW_IO_HH
#define _ASYNC_FLOW_IO_HH

#include "core/stream.hh"

#include "netstar/rte_packet.hh"
#include "netstar/hookpoint/hook_manager.hh"

namespace netstar {

namespace internal {

template<typename Ppr>
class async_flow_io {
    using FlowKeyType = typename Ppr::FlowKeyType;

    // This is used as a subscription for receiving packets from the hookpoint.
    std::experimental::optional<seastar::subscription<rte_packet, FlowKeyType*>> _receive_sub;
    unsigned _receive_sub_hook_id;

    // This is used as a stream to send packet to hookpoint.
    std::experimental::optional<seastar::stream<rte_packet>> _send_stream;
    unsigned _send_stream_hook_id;

    // either 0 or 1. If 0, then it's ingress.
    // If 1, then it's egress. This silly representation
    // is to keep up with the old implementation.
    uint8_t _direction;

public:
    async_flow_io(uint8_t direction)
        : _receive_sub_hook_id(0)
        , _send_stream_hook_id(0)
        , _direction(direction){
        assert(direction ==0 || direction == 1);
    }

    void receive_from_hookpoint(unsigned hook_id,
                                seastar::stream<rte_packet, FlowKeyType*>& hook_input_stream,
                                std::function<seastar::future<>(rte_packet, FlowKeyType*)> recv_fn) {
        assert(!_receive_sub);
        _receive_sub.emplace(hook_input_stream.listen(std::move(recv_fn)));
        _receive_sub_hook_id = hook_id;

        if(_send_stream) {
            // If _send_stream is configured, then we must pass the following check.
            assert(_receive_sub_hook_id == _send_stream_hook_id);
        }
    }

    seastar::subscription<rte_packet>
    send_to_hookpoint(unsigned hook_id, std::function<seastar::future<>(rte_packet)> send_fn) {
        assert(!_send_stream);

        _send_stream = seastar::stream<rte_packet>();
        auto sub = _send_stream->listen(std::move(send_fn));

        if(_receive_sub) {
            // If _receive_sub is configured, then we must pass the following check.
            assert(_receive_sub_hook_id == _send_stream_hook_id);
        }

        return std::move(sub);
    }

    unsigned get_hookpoint_id() {
        assert(!_send_stream || !_receive_sub);
        unsigned hookpoint_id = 0;
        if(_send_stream) {
            hookpoint_id = _send_stream_hook_id;
        }
        if(_receive_sub) {
            hookpoint_id = _receive_sub_hook_id;
        }
        return hookpoint_id;
    }

    uint8_t get_direction() {
        return _direction;
    }

    uint8_t get_reverse_direction() {
        if(_direction == 0) {
            return 1;
        }
        else {
            return 0;
        }
    }

    seastar::stream<rte_packet>& get_send_stream() {
        return *_send_stream;
    }
};

} // namespace internal

} // namespace netstar

#endif // namespace _ASYNC_FLOW_IO_HH
