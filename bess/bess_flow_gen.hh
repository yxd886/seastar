#ifndef _BESS_FLOW_GEN_HH
#define _BESS_FLOW_GEN_HH

#include "bess/time.hh"

#include "net/ip.hh"
#include "net/packet.hh"

#include "core/temporary_buffer.hh"
#include "core/print.hh"

#include <deque>
#include <queue>
#include <vector>
#include <memory>
#include <random>


// A port of BESS flow gen module.

namespace bess {

using namespace seastar;

struct flow {
    unsigned remaining_pkts;
    bool first_pkt;
    uint32_t src_ip; // host address
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
};

using flow_ptr_t = flow*;

using pkt_event_t = std::pair<uint64_t, flow_ptr_t>;

auto comp_func = [](const pkt_event_t &a, const pkt_event_t &b) {
    return a.first < b.first;
};

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
    double _total_pps;
    double _flow_rate;     /* in flows/s */
    double _flow_duration; /* in seconds */

    /* derived variables */
    double _concurrent_flows; /* expected # of flows */
    double _flow_pps;         /* packets/s/flow */
    double _flow_pkts;        /* flow_pps * flow_duration */
    double _flow_gap_ns;      /* == 10^9 / flow_rate */
    double _flow_pkt_gap;  /* = 10^9 / _flow_pps */

    int _active_flows;
    int _total_generated_flows;

    net::packet _pkt_template;
    event_heap_t _heap;
    flow_queue_t _q;

    // random device
    std::random_device _rd;
    std::default_random_engine _e;
    int _rand_max = 1000000;
    std::uniform_int_distribution<int> _dist{0, 1000000};

public:
    dynamic_udp_flow_gen(ipv4_addr ipv4_src_addr,  ipv4_addr ipv4_dst_addr,
                         double total_pps, double flow_rate, double flow_duration,
                         int pkt_len, net::ethernet_address eth_src, net::ethernet_address eth_dst)
        : _ip_src_base(ipv4_src_addr.ip)
        , _ip_dst_base(ipv4_dst_addr.ip)
        , _port_src_base(ipv4_src_addr.port)
        , _port_dst_base(ipv4_dst_addr.port)
        , _ip_src_range(0)
        , _total_pps(total_pps)
        , _flow_rate(flow_rate)
        , _flow_duration(flow_duration)
        , _concurrent_flows(flow_duration*flow_rate)
        , _flow_pps(_total_pps/_concurrent_flows)
        , _flow_pkts(_flow_pps*flow_duration)
        , _flow_gap_ns(static_cast<double>(1e9)/flow_rate)
        , _flow_pkt_gap(static_cast<double>(1e9)/_flow_pps)
        , _active_flows(0)
        , _total_generated_flows(0)
        , _heap([](const pkt_event_t &a, const pkt_event_t &b){ return a.first < b.first;})
        , _e(_rd()) {
        int pay_load_len = pkt_len - sizeof(net::eth_hdr) - sizeof(net::ip_hdr) - sizeof(net::udp_hdr);
        assert(pay_load_len >= 0);

        temporary_buffer<char> buf(pay_load_len);
        net::offload_info oi;
        net::packet pkt(std::move(buf));

        auto hdr = pkt.prepend_header<net::udp_hdr>();
        hdr->len = pkt.len();
        *hdr = net::hton(*hdr);
        oi.needs_csum = true;
        oi.protocol = net::ip_protocol_num::udp;

        auto iph = pkt.prepend_header<net::ip_hdr>();
        iph->ihl = sizeof(*iph) / 4;
        iph->ver = 4;
        iph->dscp = 0;
        iph->ecn = 0;
        iph->len = pkt.len();
        iph->id = 0;
        iph->frag = 0;
        iph->ttl = 64;
        iph->ip_proto = (uint8_t)net::ip_protocol_num::udp;
        iph->csum = 0;
        *iph = net::hton(*iph);
        oi.needs_ip_csum = true;

        auto eh = pkt.prepend_header<net::eth_hdr>();
        eh->dst_mac = eth_dst;
        eh->src_mac = eth_src;
        eh->eth_proto = uint16_t(net::eth_protocol_num::ipv4);
        *eh = net::hton(*eh);

        pkt.set_offload_info(oi);

        pkt.linearize();
        _pkt_template = std::move(pkt);
        assert(_pkt_template.nr_frags() == 1);
    }

    ~dynamic_udp_flow_gen () {
        while(!_q.empty()) {
            delete _q.front();
            _q.pop_front();
        }
        while(!_heap.empty()) {
            flow_ptr_t fptr = _heap.top().second;
            delete fptr;
            _heap.pop();
        }
    }

    void launch(uint64_t now_ns) {
        fill_in_initial_flows(now_ns);
    }

    net::packet get_next_pkt (uint64_t now_ns) {
        if(now_ns < _heap.top().first) {
            return net::packet::make_null_packet();
        }
        else {
             flow_ptr_t fptr = _heap.top().second;
             _heap.pop();
             net::packet new_pkt = build_packet_for_flow(*fptr);

             if(fptr->first_pkt) {
                 fptr->first_pkt = false;

                 // schedule a new flow to run.
                 auto new_fptr = build_new_flow();
                 _heap.push(
                     std::pair<uint64_t, flow_ptr_t>(
                         now_ns + static_cast<uint64_t>(_flow_gap_ns), new_fptr));

             }

             if(fptr->remaining_pkts == 0) {
                 _q.push_back(fptr);
                 _active_flows -= 1;
             }
             else {
                 _heap.push(
                     std::pair<uint64_t, flow_ptr_t>(
                         now_ns + static_cast<uint64_t>(_flow_pkt_gap), fptr));
             }
             return new_pkt;
        }
    }

private:
    net::packet build_packet_for_flow(flow& f) {
        net::packet new_pkt(_pkt_template.frag(0));

        // This is a udp packet, with pre-initialized header,
        // we only need to fill in ip and port field.
        auto ip_h = new_pkt.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
        ip_h->src_ip.ip.raw = net::hton(f.src_ip);
        ip_h->dst_ip.ip.raw = net::hton(f.dst_ip);

        auto udp_h = new_pkt.get_header<net::udp_hdr>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr));
        udp_h->src_port.raw = net::hton(f.src_port);
        udp_h->dst_port.raw = net::hton(f.dst_port);

        f.remaining_pkts -= 1;

        return new_pkt;
    }

    flow_ptr_t build_new_flow () {
        _active_flows += 1;
        _total_generated_flows += 1;
        if(_q.empty()) {
            auto new_fptr =
                 new flow{static_cast<unsigned>(_flow_pkts),
                         true,
                         (_ip_src_base+_ip_src_range),
                         _ip_dst_base,
                         _port_src_base,
                         _port_dst_base};
            _ip_src_range += 1;
            return new_fptr;
        }
        else{
            _q.front()->remaining_pkts = static_cast<unsigned>(_flow_pkts);
            _q.front()->first_pkt = true;
            _q.front()->src_ip = (_ip_src_base+_ip_src_range);
            _ip_src_range += 1;
            auto new_fptr = _q.front();
            _q.pop_front();
            return new_fptr;
        }
    }

    void fill_in_initial_flows(uint64_t now_ns) {

        auto new_fptr = build_new_flow ();
        _heap.push(
             std::pair<uint64_t, flow_ptr_t>(
                 now_ns + static_cast<uint64_t>(_flow_gap_ns), new_fptr));


        /* emulate pre-existing flows at the beginning */
        double past_origin = _flow_pkts / _flow_pps; /* in secs */
        double step = 1.0 / _flow_rate;

        for (double past = step; past < past_origin; past += step) {
            double pre_consumed_pkts = _flow_pps * past;
            double flow_pkts = _flow_pkts;

            if (flow_pkts > pre_consumed_pkts) {
                uint64_t jitter = static_cast<uint64_t>((static_cast<double>(1e9) * get_rand_num() / _flow_pps));

                // struct flow *f = ScheduleFlow(now_ns + jitter);
                auto new_fptr = build_new_flow ();
                new_fptr->first_pkt = false;
                new_fptr->remaining_pkts = static_cast<unsigned>(flow_pkts - pre_consumed_pkts);
                _heap.push(
                    std::pair<uint64_t, flow_ptr_t>(
                        now_ns + jitter, new_fptr));
            }
        }
    }

    double get_rand_num () {
        int val = _dist(_e);
        return static_cast<double>(val) / static_cast<double>(_rand_max);
    }

};

} // namespace netstar

#endif
