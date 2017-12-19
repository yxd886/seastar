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
#include "netstar/af/sd_async_flow.hh"

#include "bess/bess_flow_gen.hh"

using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

enum class dummy_udp_events : uint8_t{
    pkt_in=0
};

class dummy_udp_ppr{
private:
    bool _is_client;
    timer<lowres_clock> _t;
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
        return ge;
    }

    generated_events<EventEnumType> handle_packet_recv(net::packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen(dummy_udp_events::pkt_in);
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
    };
};

class forwarder;
distributed<forwarder> forwarders;

class forwarder {
    port& _ingress_port;
    std::experimental::optional<subscription<net::packet>> _ingress_port_sub;

    port& _egress_port;
    std::experimental::optional<subscription<net::packet>> _egress_port_sub;


    sd_async_flow_manager<dummy_udp_ppr> _udp_manager;
    sd_async_flow_manager<dummy_udp_ppr>::external_io_direction _udp_manager_ingress;
    sd_async_flow_manager<dummy_udp_ppr>::external_io_direction _udp_manager_egress;

    unsigned ingress_received = 0;
    unsigned ingress_snapshot = 0;
    unsigned egress_received = 0;
    unsigned egress_snapshot = 0;
    timer<lowres_clock> reporter;
public:
    forwarder (ports_env& all_ports)
        : _ingress_port(std::ref(all_ports.local_port(0)))
        , _egress_port(std::ref(all_ports.local_port(1)))
        , _udp_manager_ingress(0)
        , _udp_manager_egress(1){
    }

    future<> stop(){
        return make_ready_future<>();
    }

    void configure(int i) {
        reporter.set_callback([this]() {
            fprint(std::cout, "ingress_receive=%d. ", _ingress_port.get_qp_wrapper().rx_pkts() - this->ingress_snapshot);
            fprint(std::cout, "egress_send=%d. ", _egress_port.get_qp_wrapper().tx_pkts()-this->egress_snapshot);
            fprint(std::cout, "egress_failed_send_count=%d. \n", _egress_port.peek_failed_send_cout());
            this->ingress_snapshot =  _ingress_port.get_qp_wrapper().rx_pkts();
            this->egress_snapshot = _egress_port.get_qp_wrapper().tx_pkts();
        });

        reporter.arm_periodic(1s);

        auto udp_manager_ingress_output_fn = [this](net::packet pkt) {
            fprint(std::cout, "udp_manager_ingress_output_fn receives.\n");
            auto eth_h = pkt.get_header<net::eth_hdr>(0);
            eth_h->src_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x62};
            eth_h->dst_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x07, 0x82};
            _egress_port.send(std::move(pkt));
            return make_ready_future<>();
        };

        auto udp_manager_egress_output_fn = [this](net::packet pkt) {
            fprint(std::cout, "udp_manager_egress_output_fn receives.\n");
            auto eth_h = pkt.get_header<net::eth_hdr>(0);
            eth_h->src_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x60};
            eth_h->dst_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x08, 0x00};
            _ingress_port.send(std::move(pkt));
            return make_ready_future<>();
        };

        _udp_manager_ingress.register_to_manager(_udp_manager,
                                                 std::move(udp_manager_ingress_output_fn),
                                                 _udp_manager_egress);

        _udp_manager_egress.register_to_manager(_udp_manager,
                                                std::move(udp_manager_egress_output_fn),
                                                _udp_manager_ingress);

        _ingress_port_sub.emplace(_ingress_port.receive([this](net::packet pkt){
            ingress_received += 1;

            auto eth_h = pkt.get_header<net::eth_hdr>(0);
            if(!eth_h) {
                return make_ready_future<>();
            }

            if(net::ntoh(eth_h->eth_proto) == static_cast<uint16_t>(net::eth_protocol_num::ipv4)) {
                auto ip_h = pkt.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
                if(!ip_h) {
                    return make_ready_future<>();
                }

                if(ip_h->ip_proto == static_cast<uint8_t>(net::ip_protocol_num::udp)) {
                    auto udp_h = pkt.get_header<net::udp_hdr>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr));
                    if(!udp_h) {
                        return make_ready_future<>();
                    }

                    dummy_udp_ppr::FlowKeyType fk{net::ntoh(ip_h->dst_ip),
                                                  net::ntoh(ip_h->src_ip),
                                                  net::ntoh(udp_h->dst_port),
                                                  net::ntoh(udp_h->src_port)};
                    _udp_manager_ingress.get_send_stream().produce(std::move(pkt), &fk);
                    return make_ready_future<>();

                }
                else{
                    return make_ready_future<>();
                }
            }
            else{
                return make_ready_future<>();
            }
        }));
    }

    void run_udp_manager(int) {
        repeat([this]{
            return _udp_manager.on_new_initial_context().then([this]() mutable {
                auto ic = _udp_manager.get_initial_context();

                do_with(ic.get_sd_async_flow(), [](sd_async_flow<dummy_udp_ppr>& ac){
                    ac.register_events(dummy_udp_events::pkt_in);
                    return ac.run_async_loop([&ac](){
                        printf("client async loop runs!\n");
                        return make_ready_future<af_action>(af_action::forward);
                    });
                }).then([](){
                    printf("client async flow is closed.\n");
                });

                return stop_iteration::no;
            });
        });
    }
};

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;

    return app.run_deprecated(ac, av, [&app, &all_ports] {
        auto& opts = app.configuration();
        return all_ports.add_port(opts, 0, smp::count, port_type::original).then([&opts, &all_ports]{
            return all_ports.add_port(opts, 1, smp::count, port_type::original);
        }).then([&all_ports]{
            return forwarders.start(std::ref(all_ports));
        }).then([]{
            return forwarders.invoke_on_all(&forwarder::configure, 1);
        }).then([]{
            return forwarders.invoke_on_all(&forwarder::run_udp_manager, 1);
        }).then([]{
            fprint(std::cout, "forwarder runs!\n");
        });
    });
}
