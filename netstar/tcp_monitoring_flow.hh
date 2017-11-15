#ifndef _TCP_MONITORING_FLOW_HH
#define _TCP_MONITORING_FLOW_HH

#include "net/packet.hh"

#include "core/shared_ptr.hh"
#include "core/future.hh"
#include "core/reactor.hh"
#include "core/timer.hh"
#include "core/queue.hh"

#include <deque>
#include <experimental/optional>
#include <unordered_map>
#include <chrono>

using namespace seastar;

namespace netstar{

class tcp_monitor;

enum class direction{
    INGRESS,
    EGRESS
};

struct directed_pkt{
    net::packet pkt;
    const direction dir;
};

namespace internal{

class tcp_monitor_impl;

class tcp_monitor_impl {
    friend class tcp_monitor;

    circular_buffer<directed_pkt> _receiveq;
    bool _end;
    std::experimental::optional<promise<>> _new_pkt_promise;
public:
    tcp_monitor_impl()
        : _end(false) {
    }
    void receive_pkt(net::packet pkt, direction dir){
        _receiveq.push_back(directed_pkt{std::move(pkt), dir});
        if(_new_pkt_promise){
            _new_pkt_promise->set_value();
        }
    }
private:
    future<> on_new_packet(){
        assert(!_new_pkt_promise);
        if(_receiveq.size()>0){
            return make_ready_future<>();
        }
        _new_pkt_promise = promise<>();
        return _new_pkt_promise->get_future();
    }
    directed_pkt read_packet(){
        directed_pkt p(std::move(_receiveq.front()));
        _receiveq.pop_front();
        return p;
    }
    void set_end(){
        _end = true;
    }
    bool get_end(){
        return _end;
    }
};

} // namespace internal

class tcp_monitor{
    friend class internal::tcp_monitor_impl;

    lw_shared_ptr<internal::tcp_monitor_impl> _impl;
public:
    tcp_monitor(lw_shared_ptr<internal::tcp_monitor_impl> impl)
        : _impl(impl) {
    }
    future<> on_new_packet(){
        return _impl->on_new_packet();
    }
    directed_pkt read_packet(){
        return _impl->read_packet();
    }
    bool is_impl_ended(){
        return _impl->get_end();
    }
};

} // namespace netstar

#endif
