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

#include "netstar/port.hh"
#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/print.hh"
#include "core/distributed.hh"

#include "netstar/per_core_objs.hh"
#include "netstar/mica_client.hh"
#include "netstar/extendable_buffer.hh"
#include "netstar/stack_port.hh"
#include "netstar/port_env.hh"
// #include "netstar/af/async_flow.hh"

#include "net/ip.hh"
#include "net/byteorder.hh"

/*using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

enum class dummy_udp_events : uint8_t{
    pkt_in=0
};

class dummy_udp_ppr{
private:
    bool _is_client;
public:
    using EventEnumType = dummy_udp_events;
    using FlowKeyType = net::l4connid<net::ipv4_traits>;
    using HashFunc = net::l4connid<net::ipv4_traits>::connid_hash;

    dummy_udp_ppr(bool is_client)
        : _is_client(is_client) {
    }

public:
    generated_events<EventEnumType> handle_packet_send(net::packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen<dummy_udp_events::pkt_in>();
        return ge;
    }

    generated_events<EventEnumType> handle_packet_recv(net::packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen<dummy_udp_events::pkt_in>();
        return ge;
    }

    FlowKeyType get_reverse_flow_key(net::packet& pkt){
        auto ip_hd_ptr = pkt.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
        auto udp_hd_ptr = pkt.get_header<net::udp_hdr>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr));
        return FlowKeyType{net::ntoh(ip_hd_ptr->src_ip),
                           net::ntoh(ip_hd_ptr->dst_ip),
                           net::ntoh(udp_hd_ptr->src_port),
                           net::ntoh(udp_hd_ptr->dst_port)};
    }

public:
    struct async_flow_config {
        static constexpr int max_event_context_queue_size = 5;
        static constexpr int new_flow_queue_size = 100;
        static constexpr int max_flow_table_size = 10000;
    };
};


int main(int ac, char** av) {
    app_template app;
    timer<steady_clock_type> to;
    async_flow_manager<dummy_udp_ppr> manager;
    async_flow_manager<dummy_udp_ppr>::external_io_direction ingress(0);
    async_flow_manager<dummy_udp_ppr>::external_io_direction egress(1);

    return app.run_deprecated(ac, av, [&app, &to, &manager, &ingress, &egress]{
        ingress.register_to_manager(manager, [](net::packet pkt){return make_ready_future();}, egress);
        egress.register_to_manager(manager, [](net::packet pkt){return make_ready_future();}, ingress);
    });
}*/

struct wtf {
    std::unique_ptr<stream<int>> s;
    int i;
    wtf(int i_arg) : i(i_arg), s(std::make_unique<stream<int>>()) {}
};

int main(int ac, char** av) {
    app_template app;
    vector<wtf> v;
    return app.run_deprecated(ac, av, [&app, &v]{
        v.emplace_back(std::piecewise_construct, 1);
    });
}
