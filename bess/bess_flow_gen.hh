#ifndef _BESS_FLOW_GEN_HH
#define _BESS_FLOW_GEN_HH

#include "bess/time.hh"

#include "net/ip.hh"
#include "net/packet.hh"

#include <deque>
#include <queue>
#include <vector>
#include <memory>


// A port of BESS flow gen module.

namespace besss {

using namespace seastar;

struct flow {
    unsigned remaining_pkts;
    bool first_pkt;
    uint32_t src_ip; // host address
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
};

using flow_ptr_t = std::unique_ptr<flow>;

using pkt_event_t = std::pair<uint64_t, flow_ptr_t>;


using event_heap_t = std::priority_queue<pkt_event_t,
                                         std::vector<pkt_event_t>,
                                         std::function<bool(const pkt_event_t&, const pkt_event_t&)>>;

using flow_queue_t = std::deque<flow_ptr_t>;

class dynamic_udp_flow_gen {
    // base parameter
    uint32_t _ip_src_base;
    uint32_t _ip_dst_base;
    uint16_t _port_src_base;
    uint16_t _port_dst_base;

    // range parameter
    uint32_t _ip_src_range;

    /* load parameters */
    double _total_pps_;
    double _flow_rate_;     /* in flows/s */
    double _flow_duration_; /* in seconds */

    /* derived variables */
    double _concurrent_flows; /* expected # of flows */
    double _flow_pps;         /* packets/s/flow */
    double _flow_pkts;        /* flow_pps * flow_duration */
    double _flow_gap_ns;      /* == 10^9 / flow_rate */

    uint64_t _flow_pkt_gap;  /* = 10^9 / _flow_pps */
    // flow_next_pkt_time = cur_ns + _flow_pkt_gap;

    net::packet _pkt_template;
    event_heap_t _heap;
    flow_queue_t _q;

public:
    net::packet build_packet_for_flow(flow& f) {
        net::packet new_pkt(_pkt_template.frag(0));

        // This is a udp packet, with pre-initialized header,
        // we only need to fill in ip and port field.
        auto ip_h = new_pkt.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
        ip_h->src_ip = net::hton(f.src_ip);
        ip_h->dst_ip = net::hton(f.dst_ip);

        auto udp_h = new_pkt.get_header<net::udp_hdr>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr));
        udp_h->src_port = net::hton(f.src_port);
        udp_h->dst_port = net::hton(f.dst_port);

        f.remaining_pkts -= 1;

        return new_pkt;
    }

    flow_ptr_t build_new_flow () {
        if(_q.empty()) {
            auto new_fptr =
            std::make_unique<flow>({static_cast<unsigned>(_flow_pkts), true, (_ip_src_base+_ip_src_range), _ip_dst_base, _port_src_base, _port_dst_base});
            _ip_src_range += 1;
            return new_fptr;
        }
        else{
            _q.front()->remaining_pkts = static_cast<unsigned>(_flow_pkts);
            _q.front()->first_pkt = true;
            _q.front()->src_ip = (_ip_src_base+_ip_src_range);
            _ip_src_range += 1;
            auto new_fptr = std::move(_q.front());
            _q.pop_front();
            return new_fptr;
        }
    }

    void build_up_initial_heap () {

    }


};

} // namespace netstar

#endif
