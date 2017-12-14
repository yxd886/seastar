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

using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

class forwarder;
distributed<forwarder> forwarders;

class forwarder {
    std::vector<port*> _all_ports;
public:
    forwarder (ports_env& all_ports) {
        _all_ports.push_back(&(all_ports.local_port(0)));
        _all_ports.push_back(&(all_ports.local_port(1)));
    }

    future<> stop(){
        return make_ready_future<>();
    }

    void configure(int ) {
        auto& ingress_port = *_all_ports[0];
        auto& egress_port = *_all_ports[1];

        ingress_port.receive([&egress_port](net::packet pkt){
            fprint(std::cout, "ingress receives packet.\n");
            egress_port.send(std::move(pkt));
            return make_ready_future<>();
        });

        egress_port.receive([&ingress_port](net::packet pkt){
            fprint(std::cout, "egress receives packet.\n");
            ingress_port.send(std::move(pkt));
            return make_ready_future<>();
        });
    }
};

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;

    return app.run_deprecated(ac, av, [&app, &all_ports] {
        auto& opts = app.configuration();
        return all_ports.add_port(opts, 0, smp::count, port_type::netstar_dpdk).then([&opts, &all_ports]{
            return all_ports.add_port(opts, 1, smp::count, port_type::netstar_dpdk);
        }).then([&all_ports]{
            return forwarders.start(std::ref(all_ports));
        }).then([]{
            return forwarders.invoke_on_all(&forwarder::configure, 1);
        }).then([]{
            fprint(std::cout, "forwarder runs!\n");
        });
    });
}
