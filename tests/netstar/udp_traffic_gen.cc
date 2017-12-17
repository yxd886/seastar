/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/print.hh"
#include "core/distributed.hh"
#include "core/print.hh"
#include "core/sleep.hh"

#include "net/udp.hh"
#include "net/ip_checksum.hh"
#include "net/ip.hh"
#include "net/net.hh"
#include "net/packet.hh"
#include "net/byteorder.hh"

#include "netstar/work_unit.hh"
#include "netstar/port_env.hh"

#include "bess/bess_flow_gen.hh"

using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

class traffic_gen;
distributed<traffic_gen> traffic_gens;

ipv4_addr ipv4_src_addr("10.10.0.1:10240");
ipv4_addr ipv4_dst_addr("10.10.0.3:10241");
net::ethernet_address eth_src{0x3c, 0xfd, 0xfe, 0x06, 0x08, 0x00};
net::ethernet_address eth_dst{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x60};

struct rx_tx_stats {
    uint64_t rx_pkts;
    uint64_t tx_pkts;

    void operator+=(const rx_tx_stats& o) {
        rx_pkts += o.rx_pkts;
        tx_pkts += o.tx_pkts;
    }
};

class traffic_gen {
    bess::dynamic_udp_flow_gen _pkt_gen;
    netstar::port* _p;
    int _n;
    int _duration;
    uint64_t _prev_checkpoint;

    uint64_t _tx_pkts;
    uint64_t _rx_pkts;
public:
    traffic_gen(double total_pps, double flow_rate, double flow_duration, int pkt_len, int duration,
                netstar::ports_env& all_ports)
        : _pkt_gen(ipv4_src_addr, ipv4_dst_addr,
                   total_pps, flow_rate, flow_duration,
                   pkt_len, eth_src, eth_dst)
        , _p(&(all_ports.local_port(0)))
        , _n(0)
        , _duration(duration)
        , _prev_checkpoint(0)
        , _tx_pkts(0)
        , _rx_pkts(0){

    }

    future<> stop(){
        return make_ready_future<>();
    }

    void prepare_initial_flows(int) {
        _pkt_gen.launch(tsc_to_ns(rdtsc()));
    }

    void run(int) {
        _prev_checkpoint = tsc_to_ns(rdtsc());
        repeat([this](){
            uint64_t now_ns = tsc_to_ns(rdtsc());

            if(now_ns - _prev_checkpoint > 1e9) {
                _prev_checkpoint = now_ns;
                _n += 1;
                if(_n == _duration) {
                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                }
            }

            auto next_ns = _pkt_gen.get_next_active_time();

            while(next_ns <= now_ns) {
                auto pkt = _pkt_gen.get_next_pkt(now_ns);
                auto f = _p->send(std::move(pkt));
                if(!f.available()){
                    return f.then([]{
                        return stop_iteration::no;
                    });
                }
                next_ns = _pkt_gen.get_next_active_time();
            }

            return later().then([]{
                 return stop_iteration::no;
            });
        });
    }

    void collect_stats(int) {
        repeat([this]{
            return traffic_gens.map_reduce(adder<rx_tx_stats>(), &traffic_gen::tx_pkts).then([this](rx_tx_stats s){
                fprint(std::cout, "Tx pkts: %d pkts/s.\n", s.tx_pkts-_tx_pkts);
                _tx_pkts = s.tx_pkts;
            }).then([]{
                return sleep(1s).then([]{
                    return stop_iteration::no;
                });
            });
        });
    }

    future<rx_tx_stats> tx_pkts() {
        return make_ready_future<rx_tx_stats>(rx_tx_stats{_p->get_qp_wrapper().tx_pkts(), 0});
    }

};

namespace bpo = boost::program_options;

int main(int ac, char** av) {
    app_template app;
    netstar::ports_env all_ports;
    app.add_options()
            ("total-pps", bpo::value<double>()->default_value(1000000.0), "total-pps")
            ("flow-rate", bpo::value<double>()->default_value(10000.0), "flow-rate")
            ("flow-duration", bpo::value<double>()->default_value(10.0), "flow-duration")
            ("pkt-len", bpo::value<int>()->default_value(64), "pkt-len")
            ("duration", bpo::value<int>()->default_value(10), "duration")
            ;

    return app.run_deprecated(ac, av, [&app, &all_ports] {
        auto& opts = app.configuration();
        auto total_pps = opts["total-pps"].as<double>();
        auto flow_rate = opts["flow-rate"].as<double>();
        auto flow_duration = opts["flow-duration"].as<double>();
        auto pkt_len = opts["pkt-len"].as<int>();
        auto duration = opts["duration"].as<int>();

        return all_ports.add_port(opts, 0, smp::count, port_type::netstar_dpdk).then([&opts, &all_ports]{
            return all_ports.add_port(opts, 1, smp::count, port_type::netstar_dpdk);
        }).then([total_pps, flow_rate, flow_duration, pkt_len, duration, &all_ports]{
            return traffic_gens.start(total_pps, flow_rate, flow_duration, pkt_len, duration, std::ref(all_ports));
        }).then([]{
            return traffic_gens.invoke_on_all(&traffic_gen::prepare_initial_flows, 1);
        }).then([]{
            return traffic_gens.invoke_on_all(&traffic_gen::run, 1);
        }).then([]{
            return traffic_gens.invoke_on(0, &traffic_gen::collect_stats, 1);
        })
        ;
    });
}
