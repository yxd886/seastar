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


};

} // namespace netstar

#endif
