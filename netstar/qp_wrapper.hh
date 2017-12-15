#ifndef _QP_WRAPPER_HH
#define _QP_WRAPPER_HH

#include <memory>

#include "net/net.hh"
#include "net/proxy.hh"

using namespace seastar;

namespace netstar{

// This is a wrapper class to net::qp.
// The most important functionality of this class
// is to store a unique_ptr to net::qp.
// It also provides several public interfaces for
// accessing net::qp.
class qp_wrapper{
    uint16_t _qid;
    net::device* _dev;
    std::unique_ptr<net::qp> _qp;
public:
    // The default constructor.
    explicit qp_wrapper(boost::program_options::variables_map opts,
                         net::device* dev,
                         uint16_t qid) :
                         _qid(qid), _dev(dev){
        if(_qid < _dev->hw_queues_count()){
            _qp = _dev->init_local_queue(opts, _qid);

            std::map<unsigned, float> cpu_weights;
            for (unsigned i = _dev->hw_queues_count() + _qid % _dev->hw_queues_count();
                 i < smp::count;
                 i+= _dev->hw_queues_count()) {
                cpu_weights[i] = 1;
            }
            cpu_weights[_qid] = opts["hw-queue-weight"].as<float>();
            _qp->configure_proxies(cpu_weights);

            _dev->update_local_queue(_qp.get());
        }
        else{
            auto master = _qid % _dev->hw_queues_count();
            _qp = create_proxy_net_device(master, _dev);

            _dev->update_local_queue(_qp.get());
        }
    }

    // Register packet provider for the qp
    void register_packet_provider(std::function<std::experimental::optional<net::packet>()> fn){
        _qp->register_packet_provider(std::move(fn));
    }

    // Start the rx_stream of qp
    subscription<net::packet>
    receive(std::function<future<> (net::packet)> next_packet) {
        return _dev->receive(std::move(next_packet));
    }

public:
    // Utility functions exported by port_wrapper:

    // Obtain the default ethernet address of the underlying NIC
    net::ethernet_address get_eth_addr(){
        return _dev->hw_address();
    }
    // Obtain the configured RS key.
    const rss_key_type& get_rss_key(){
        return _dev->rss_key();
    }
    // Giving an RSS key, return which CPU is this RSS key
    // mapped to.
    unsigned hash2cpu(uint32_t hash){
        return _dev->hash2qid(hash);
    }
    // Obtain the queue id of the qp
    unsigned get_qid(){
        return _qid;
    }
    // Obtain the number of hardware queues configured
    // for the underlying DPDK device
    uint16_t get_hw_queues_count(){
        return _dev->hw_queues_count();
    }
    // Obtain the stats of the qp.
    uint64_t rx_bytes() {
        return _qp->rx_bytes();
    }
    uint64_t rx_pkts() {
        return _qp->rx_pkts();
    }
    uint64_t tx_bytes() {
        return _qp->tx_bytes();
    }
    uint64_t tx_pkts() {
        return _qp->tx_pkts();
    }
};

} // namespace netstar

#endif // _QP_WRAPPER_HH
