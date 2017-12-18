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

class forwarder;
distributed<forwarder> forwarders;

class forwarder {
    std::vector<port*> _all_ports;
    std::experimental::optional<subscription<net::packet>> _ingress_sub;
    std::experimental::optional<subscription<net::packet>> _egress_sub;

    unsigned ingress_received = 0;
    unsigned ingress_snapshot = 0;
    unsigned egress_received = 0;
    unsigned egress_snapshot = 0;
    timer<lowres_clock> reporter;
public:
    forwarder (ports_env& all_ports) {
        _all_ports.push_back(&(all_ports.local_port(0)));
        _all_ports.push_back(&(all_ports.local_port(1)));
    }

    future<> stop(){
        return make_ready_future<>();
    }

    void configure(int i) {
        auto& ingress_port = *_all_ports[0];
        auto& egress_port = *_all_ports[1];

        reporter.set_callback([this]() {
            fprint(std::cout, "ingress_receive=%d, egress_receive=%d.\n",
                   this->ingress_received-this->ingress_snapshot, this->egress_received-this->egress_snapshot);
            this->ingress_snapshot = this->ingress_received;
            this->egress_snapshot = this->egress_received;
        });

        reporter.arm_periodic(1s);

        _ingress_sub.emplace(ingress_port.receive([&egress_port, this](net::packet pkt){
            // fprint(std::cout, "ingress receives packet.\n");
            ingress_received += 1;
            auto eth_h = pkt.get_header<net::eth_hdr>(0);
            if(!eth_h) {
                return make_ready_future<>();
            }

            if(net::ntoh(eth_h->eth_proto) == static_cast<uint16_t>(net::eth_protocol_num::ipv4)) {
                // Perform the address translation.
                // For IP/TCP packets received from port 0, it should replace the
                // source mac to 3c:fd:fe:06:09:62 and destination mac to 3c:fd:fe:06:07:82
                eth_h->src_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x62};
                eth_h->dst_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x07, 0x82};
            }

            // egress_port.send(std::move(pkt));
            return make_ready_future<>();
        }));

        _egress_sub.emplace(egress_port.receive([&ingress_port, this](net::packet pkt){
            // fprint(std::cout, "egress receives packet.\n");
            egress_received += 1;
            auto eth_h = pkt.get_header<net::eth_hdr>(0);
            if(!eth_h) {
                return make_ready_future<>();
            }

            if(net::ntoh(eth_h->eth_proto) == static_cast<uint16_t>(net::eth_protocol_num::ipv4)) {
                // Perform the address translation.
                // For IP/TCP packets received from port 1, it should replace the
                // source mac to 3c:fd:fe:06:09:60 and destination mac to 3c:fd:fe:06:08:00
                eth_h->src_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x60};
                eth_h->dst_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x08, 0x00};
            }

            // ingress_port.send(std::move(pkt));
            return make_ready_future<>();
        }));
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
