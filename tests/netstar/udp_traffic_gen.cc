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
using namespace std::chrono_literals;

// A description of udp_traffic_gen.
// Generate udp packet from p0 and receive the generated udp packet from p1.
// Each generated udp packet has a fixed destination IP address 10.10.0.3.
// Each generated udp packet may contain a different source IP address,
// starting from a specified value. For simplicity, we will use the same
// source port and destination port for all generated udp flows.

// We will define a class called udp_generator. udp_generator generates
// flow packetes of a single flow, as fast as possible. Since there is a
// semaphore protecting the send port, we can simulate fair sharing.
//

class traffic_gen;
distributed<traffic_gen> traffic_gens;

ipv4_addr ipv4_src_addr("10.10.0.1:1000");
ipv4_addr ipv4_dst_addr("10.10.0.3:2000");
net::ethernet_address eth_src{0x3c, 0xfd, 0xfe, 0x06, 0x08, 0x00};
net::ethernet_address eth_dst{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x60};

class traffic_gen {
    bess::dynamic_udp_flow_gen _pkt_gen;
    netstar::port* _p;

public:
    traffic_gen(double total_pps, double flow_rate, double flow_duration, int pkt_len, netstar::ports_env& all_ports)
        : _pkt_gen(ipv4_src_addr, ipv4_dst_addr,
                   total_pps, flow_rate, flow_duration,
                   pkt_len, eth_src, eth_dst)
        , _p(&(all_ports.local_port(0))){

    }

    future<> stop(){
        return make_ready_future<>();
    }

    void prepare_initial_flows(int) {
        _pkt_gen.launch(tsc_to_ns(rdtsc()));
    }

    void run(int) {
        keep_doing();
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
            ;

    return app.run_deprecated(ac, av, [&app, &all_ports] {
        auto& opts = app.configuration();
        auto total_pps = opts["total-pps"].as<double>();
        auto flow_rate = opts["flow-rate"].as<double>();
        auto flow_duration = opts["flow-duration"].as<double>();
        auto pkt_len = opts["pkt-len"].as<int>();

        return all_ports.add_port(opts, 0, smp::count, port_type::netstar_dpdk).then([&opts, &all_ports]{
            return all_ports.add_port(opts, 1, smp::count, port_type::netstar_dpdk);
        }).then([total_pps, flow_rate, flow_duration, pkt_len, &all_ports]{
            return traffic_gens.start(total_pps, flow_rate, flow_duration, pkt_len, all_ports);
        }).then([]{
            return traffic_gens.invoke_on_all(&traffic_gen::prepare_initial_flows, 1);
        })
        .then([]{
            printf("udp traffic gen is launched.\n");
            return traffic_gens.stop();
        }).then([]{
            engine().exit(0);
        });
    });
}
