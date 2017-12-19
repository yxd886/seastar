#ifndef _PORT_HH
#define _PORT_HH

#include <memory>
#include <experimental/optional>

#include "core/future.hh"
#include "core/stream.hh"
#include "core/circular_buffer.hh"
#include "core/semaphore.hh"
#include "core/queue.hh"

#include "netstar/qp_wrapper.hh"

using namespace seastar;

namespace netstar{

// A regular port. Applications can directly fetch
// and send packets from this port using the exposed
// public methods.
class port{
    uint16_t _port_id;
    qp_wrapper _qp_wrapper;
    circular_buffer<net::packet> _sendq;
    unsigned _failed_send_count;
    bool _receive_configured;
    std::experimental::optional<subscription<net::packet>> _sub;
    seastar::queue<net::packet> _receiveq;
    static constexpr size_t port_sendq_size = 500;
public:
    explicit port(boost::program_options::variables_map opts,
                          net::device* dev,
                          uint16_t port_id) :
        _port_id(port_id),
        _qp_wrapper(opts, dev, engine().cpu_id()),
        _failed_send_count(0),
        _receive_configured(false),
        _receiveq(100){

        if(_qp_wrapper.get_qid() < _qp_wrapper.get_hw_queues_count()){
            _qp_wrapper.register_packet_provider([this](){
                std::experimental::optional<net::packet> p;
                if (!_sendq.empty()) {
                    p = std::move(_sendq.front());
                    _sendq.pop_front();
                }
                return p;
            });
        }

        // 180 is the default attempt of a single tx_poll by dpdk_qp;
        // _queue_space = std::make_unique<semaphore>(180);
        _sendq.reserve(port_sendq_size);
    }

    ~port(){
    }

    // port can only be constructed by per_core_objs,
    // so all the copy and move constructors/assignments are deleted
    port(const port& other) = delete;
    port(port&& other)  = delete;
    port& operator=(const port& other) = delete;
    port& operator=(port&& other) = delete;

    // stop() has to be added so that port can be
    // constructed by per_core_objs.
    future<> stop(){
        return make_ready_future<>();
    }
public:
    // Send the packet out.
    // Assert that we are sending out from correct qp type.
    // Need to wait for enough space in the _queue_space.
    inline future<> send(net::packet p){
        if(_sendq.size() < port_sendq_size) {
            _sendq.push_back(std::move(p));
        }
        else{
            _failed_send_count += 1;
        }
        return make_ready_future<>();
    }

    // Lineraize the packet and then send the packet out.
    // This is primarily used by mica_client.
    inline future<> linearize_and_send(net::packet p){
        if(_sendq.size() < port_sendq_size) {
            p.linearize();
            _sendq.push_back(std::move(p));
        }
        else{
            _failed_send_count += 1;
        }
        return make_ready_future<>();
    }

    inline size_t peek_sendq_size() {
        return _sendq.size();
    }

    inline unsigned peek_failed_send_cout () {
        return _failed_send_count;
    }

    // Provide a customized receive function for the underlying qp.
    subscription<net::packet>
    receive(std::function<future<> (net::packet)> next_packet) {
        assert(!_receive_configured);
        _receive_configured = true;
        return _qp_wrapper.receive(std::move(next_packet));
    }

    // Expose qp_wrapper. Some functionality in netstar
    // requires to access the public methods exposed by qp_wrapper
    qp_wrapper& get_qp_wrapper(){
        return _qp_wrapper;
    }
public:
    // Enable on_new_pkt() function. Once this is called
    // on_new_pkt() will return a future that is going to
    // be eventually resolved. Otherwise, the future returned
    // by on_new_pkt will never be resolved.
    void enable_on_new_pkt(){
        assert(!_receive_configured);
        _receive_configured = true;

        _sub.emplace(
            _qp_wrapper.receive([this](net::packet pkt){
                _receiveq.push(std::move(pkt));
                return make_ready_future<>();
            })
        );
    }
    future<net::packet> on_new_pkt(){
        return _receiveq.pop_eventually();
    }
};

} // namespace netstar

#endif // _PORT_REFACTOR_HH
