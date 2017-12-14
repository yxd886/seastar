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
    std::experimental::optional<subscription<net::packet>> _ingress_sub;
    std::experimental::optional<subscription<net::packet>> _egress_sub;
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

        _ingress_sub.emplace(ingress_port.receive([&egress_port](net::packet pkt){
            fprint(std::cout, "ingress receives packet.\n");

            auto eth_h = pkt.get_header<net::eth_hdr>(0);
            if(!eth_h) {
                return make_ready_future<>();
            }

            if(eth_h->eth_proto == net::eth_protocol_num::arp) {
                auto ah = pkt.get_header(sizeof(net::eth_hdr), net::arp_for::arp_hdr::size());
                if (!ah) {
                    return make_ready_future<>();
                }
                auto h = net::arp_for::arp_hdr::read(ah);
                switch (h.oper) {
                    case 1:
                        std::cout<<"Receive arp request, send_hwaddr="<<h.sender_hwaddr<<", send_paddr="<<h.sender_paddr
                                 <<", target_hwaddr="<<h.target_hwaddr<<", target_paddr="<<h.target_paddr<<std::endl;
                        return handle_request(&h);
                    case 2: {
                        std::cout<<"Receive arp reply, send_hwaddr="<<h.sender_hwaddr<<", send_paddr="<<h.sender_paddr
                                 <<", target_hwaddr="<<h.target_hwaddr<<", target_paddr="<<h.target_paddr<<std::endl;
                        return make_ready_future<>();
                    }
                    default:
                        return make_ready_future<>();
                    }
            }

            egress_port.send(std::move(pkt));
            return make_ready_future<>();
        }));

        _egress_sub.emplace(egress_port.receive([&ingress_port](net::packet pkt){
            fprint(std::cout, "egress receives packet.\n");
            ingress_port.send(std::move(pkt));
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
