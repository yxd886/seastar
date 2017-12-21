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
#include "core/gate.hh"

#include "netstar/per_core_objs.hh"
#include "netstar/mica_client.hh"
#include "netstar/extendable_buffer.hh"
#include "netstar/stack_port.hh"
#include "netstar/port_env.hh"
#include "netstar/af/async_flow.hh"

#include "net/ip.hh"
#include "net/byteorder.hh"

#include <array>

using namespace seastar;
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
        ge.event_happen(dummy_udp_events::pkt_in);
        ge.close_event_happen();
        return ge;
    }

    generated_events<EventEnumType> handle_packet_recv(net::packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen(dummy_udp_events::pkt_in);
        ge.close_event_happen();
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
        static constexpr int max_directions = 2;

        static FlowKeyType get_flow_key(net::packet& pkt){
            auto ip_hd_ptr = pkt.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
            auto udp_hd_ptr = pkt.get_header<net::udp_hdr>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr));
            return FlowKeyType{net::ntoh(ip_hd_ptr->dst_ip),
                               net::ntoh(ip_hd_ptr->src_ip),
                               net::ntoh(udp_hd_ptr->dst_port),
                               net::ntoh(udp_hd_ptr->src_port)};
        }

        static net::packet build_pkt(const char* buf){
            ipv4_addr ipv4_src_addr("10.1.2.4:666");
            ipv4_addr ipv4_dst_addr("10.1.2.5:666");
            auto pkt = net::packet::from_static_data(buf, strlen(buf));

            auto hdr = pkt.prepend_header<net::udp_hdr>();
            hdr->src_port = ipv4_src_addr.port;
            hdr->dst_port = ipv4_dst_addr.port;
            hdr->len = pkt.len();
            *hdr = net::hton(*hdr);

            net::checksummer csum;
            net::ipv4_traits::udp_pseudo_header_checksum(csum, ipv4_src_addr, ipv4_dst_addr, pkt.len());
            csum.sum(pkt);
            hdr->cksum = csum.get();

            net::offload_info oi;
            oi.needs_csum = false;
            oi.protocol = net::ip_protocol_num::udp;
            pkt.set_offload_info(oi);

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
            iph->src_ip = net::ipv4_address(ipv4_src_addr.ip);
            iph->dst_ip = net::ipv4_address(ipv4_dst_addr.ip);
            *iph = net::hton(*iph);
            net::checksummer ip_csum;
            ip_csum.sum(reinterpret_cast<char*>(iph), sizeof(*iph));
            iph->csum = csum.get();

            auto eh = pkt.prepend_header<net::eth_hdr>();
            net::ethernet_address eth_src{0x3c, 0xfd, 0xfe, 0x06, 0x07, 0x82};
            net::ethernet_address eth_dst{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x62};
            eh->dst_mac = eth_dst;
            eh->src_mac = eth_src;
            eh->eth_proto = uint16_t(net::eth_protocol_num::ipv4);
            *eh = net::hton(*eh);

            pkt.linearize();
            assert(pkt.nr_frags() == 1);
            return pkt;
        }
    };
};

class async_flow_loop {
    client_async_flow<dummy_udp_ppr> _client;
    // int i;
    // string x;

    server_async_flow<dummy_udp_ppr> _server;
    seastar::gate _g;
public:
    async_flow_loop(client_async_flow<dummy_udp_ppr> client,
                    server_async_flow<dummy_udp_ppr> server)
        : _client(std::move(client))
        , _server(std::move(server)){
    }

    void configure() {
        _client.register_events(af_send_recv::send, dummy_udp_events::pkt_in);
        _server.register_events(af_send_recv::recv, dummy_udp_events::pkt_in);
    }

    future<> run() {
        _g.enter();
        _client.run_async_loop([this](){
            return make_ready_future<af_action>(af_action::close_forward);
        }).then([this](){
            _g.leave();
        });

        _g.enter();
        _server.run_async_loop([this](){
            return make_ready_future<af_action>(af_action::close_forward);
        }).then([this](){
            _g.leave();
        });

        return _g.close();
    }
};

int main(int ac, char** av) {
    app_template app;
    timer<steady_clock_type> to;
    async_flow_manager<dummy_udp_ppr> manager;
    async_flow_manager<dummy_udp_ppr>::external_io_direction ingress(0);
    async_flow_manager<dummy_udp_ppr>::external_io_direction egress(1);
    net::packet the_pkt = dummy_udp_ppr::async_flow_config::build_pkt("abcdefg");

    return app.run_deprecated(ac, av, [&app, &to, &manager, &ingress, &egress, &the_pkt]{
        ingress.register_to_manager(manager, [](net::packet pkt){return make_ready_future();}, egress);
        egress.register_to_manager(manager, [](net::packet pkt){return make_ready_future();}, ingress);
        net::packet pkt(the_pkt.frag(0));
        dummy_udp_ppr::FlowKeyType fk = dummy_udp_ppr::async_flow_config::get_flow_key(pkt);
        ingress.get_send_stream().produce(std::move(pkt), &fk);

        return manager.on_new_initial_context().then([&manager]() mutable {
            auto ic = manager.get_initial_context();

            do_with(async_flow_loop(ic.get_client_async_flow(), ic.get_server_async_flow()), [](async_flow_loop& l){
               l.configure();
               return l.run();
            }).then([](){
                printf("async_flow_loop close.\n");
            });
        }).then([](){
            engine().exit(0);
        });
    });
}
