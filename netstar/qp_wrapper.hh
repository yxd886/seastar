#ifndef _QP_WRAPPER_HH
#define _QP_WRAPPER_HH

#include <memory>

#include "net/net.hh"
#include "net/proxy.hh"

#include "netstar/rte_packet.hh"

namespace netstar{

class qp_wrapper{
    uint16_t _qid;
    seastar::net::device* _dev;
    std::unique_ptr<seastar::net::qp> _qp;

public:
    explicit qp_wrapper(boost::program_options::variables_map opts,
                         seastar::net::device* dev,
                         uint16_t qid) :
                         _qid(qid), _dev(dev){
        // Each core must has a hardware queue. We enforce this!
        assert(_qid < _dev->hw_queues_count());

        // The default qp initialization taking from native stack
        _qp = _dev->init_local_queue(opts, _qid);
        std::map<unsigned, float> cpu_weights;
        for (unsigned i = _dev->hw_queues_count() + _qid % _dev->hw_queues_count();
             i < seastar::smp::count;
             i+= _dev->hw_queues_count()) {
            cpu_weights[i] = 1;
        }
        cpu_weights[_qid] = opts["hw-queue-weight"].as<float>();
        _qp->configure_proxies(cpu_weights);
        _dev->update_local_queue(_qp.get());
    }

    // Register packet provider for the qp
    void register_packet_provider(std::function<std::experimental::optional<rte_packet>()> fn){
        _qp->register_rte_packet_provider(std::move(fn));
    }

    // Start the rx_stream of qp
    seastar::subscription<rte_packet>
    receive(std::function<seastar::future<> (rte_packet)> next_packet) {
        return _dev->receive_rte_packet(std::move(next_packet));
    }

public:
    seastar::net::ethernet_address get_eth_addr(){
        return _dev->hw_address();
    }

    const seastar::rss_key_type& get_rss_key(){
        return _dev->rss_key();
    }

    unsigned hash2cpu(uint32_t hash){
        return _dev->hash2qid(hash);
    }

    unsigned get_qid(){
        return _qid;
    }

    uint16_t get_hw_queues_count(){
        return _dev->hw_queues_count();
    }

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
