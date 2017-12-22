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

// For an NFV application, it needs to poll packet no matter what happened.
// In a run-to-completion model, we can always poll fixed amount of packet,
// process them and send them out. If the run-to-completion system overloads,
// it can still maintain a desired throughput.
// However, for applications built in a fully asynchronous fashion, this presents as
// a problem. We still need to poll packets for timely processing. However, the problem
// is that, after the packets are polled, we will not know when they are released
// out of the system. Therefore, when a system is overloaded. We will see a tremendous performance
// drop.
// Here, I'm adding a counter to the port device, which serves as the front line for the
// entire netstar application. The counter is increased whenever a valid new packet is
// going to be received by the the upper appliaction, and decreased only when the packet is
// deconstructed.

// A regular port. Applications can directly fetch
// and send packets from this port using the exposed
// public methods.
class port{
    static constexpr size_t port_sendq_size = 500;

    uint16_t _port_id;
    qp_wrapper _qp_wrapper;
    unsigned _failed_send_count;
    bool _receive_configured;
    unsigned* _port_counter;
    circular_buffer<net::packet> _sendq;
    std::experimental::optional<subscription<net::packet>> _sub;
public:
    explicit port(boost::program_options::variables_map opts,
                  net::device* dev,
                  uint16_t port_id,
                  std::vector<unsigned>* vec) :
        _port_id(port_id),
        _qp_wrapper(opts, dev, engine().cpu_id()),
        _failed_send_count(0),
        _receive_configured(false),
        _port_counter(&((*vec)[engine().cpu_id()])){

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
};

} // namespace netstar

#endif // _PORT_REFACTOR_HH
