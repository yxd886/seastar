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
using namespace netstar;
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

class traffic_gen {
    bess::dynamic_udp_flow_gen _pkt_gen;

public:
    template<typename... Args>
    traffic_gen(Args&&... args)
        : _pkt_gen(std::forward<Args>(args)...) {

    }

};

namespace bpo = boost::program_options;

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;
    app.add_options()
            ("conn", bpo::value<unsigned>()->default_value(16), "nr connections per cpu")
            ("time", bpo::value<unsigned>()->default_value(60), "total transmission time")
            ;

    return app.run_deprecated(ac, av, [&app, &all_ports] {
        auto& opts = app.configuration();
        return all_ports.add_port(opts, 0, smp::count, port_type::netstar_dpdk).then([&opts, &all_ports]{
            return all_ports.add_port(opts, 1, smp::count, port_type::netstar_dpdk);
        });
    });
}
