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
#include "netstar/netstar_dpdk_device.hh"
#include "netstar/fdir_device.hh"
#include "netstar/port.hh"
#include "netstar/work_unit.hh"
#include "net/udp.hh"
#include "net/ip_checksum.hh"
#include "net/ip.hh"
#include "net/net.hh"
#include "net/packet.hh"
#include "net/byteorder.hh"

using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

class simple_send_work_unit : public work_unit<simple_send_work_unit>{
public:
    explicit simple_send_work_unit(per_core_objs<simple_send_work_unit>* all_objs) :
        work_unit<simple_send_work_unit>(all_objs) {}

    simple_send_work_unit(const simple_send_work_unit& other) = delete;
    simple_send_work_unit(simple_send_work_unit&& other)  = delete;
    simple_send_work_unit& operator=(const simple_send_work_unit& other) = delete;
    simple_send_work_unit& operator=(simple_send_work_unit&& other) = delete;

    future<> stop(){
        return make_ready_future<>();
    }

    void send_from_port_0(){
        _stats_timer.set_callback([this] {
            std::cout << "Thread: " << engine().cpu_id() << ", \t";
            std::cout << "Out: " << _n_sent << " pps, \t";
            std::cout << "Err: " << _n_failed << " pps, \t";
            std::cout << "In: " << _n_received << " pps" << std::endl;
            _n_sent = 0;
            _n_received = 0;
            _n_failed = 0;
        });
        _stats_timer.arm_periodic(1s);
        _pkt = build_pkt(
                "aaaaaaaaaaaaaaaaaaaaa");

        keep_doing([this](){
           net::packet pkt(_pkt.frag(0));
           _n_sent+=1;
           return this->send_from_port(0, std::move(pkt));
        });
    }

    void send_from_port_1(){
        _stats_timer.set_callback([this] {
            std::cout << "Thread: " << engine().cpu_id() << ", \t";
            std::cout << "Out: " << _n_sent << " pps, \t";
            std::cout << "Err: " << _n_failed << " pps, \t";
            std::cout << "In: " << _n_received << " pps" << std::endl;
            _n_sent = 0;
            _n_received = 0;
            _n_failed = 0;
        });
        _stats_timer.arm_periodic(1s);
        _pkt = build_pkt(
                "aaaaaaaaaaaaaaaaaaaaa");

        keep_doing([this](){
           net::packet pkt(_pkt.frag(0));
           _n_sent+=1;
           return this->send_from_port(1, std::move(pkt));
        });
    }

private:
    uint64_t _n_sent {};
    uint64_t _n_received {};
    uint64_t _n_failed {};
    timer<> _stats_timer;
    net::packet _pkt;
    net::packet build_pkt(const char* buf){
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
        std::cout<< "pkt.nr_framgs() = " << pkt.nr_frags() <<std::endl;
        std::cout<<"pkt.len() = "<< pkt.len() <<std::endl;
        assert(pkt.nr_frags() == 1);
        return pkt;
    }
};

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;
    per_core_objs<simple_send_work_unit> all_objs;

    return app.run_deprecated(ac, av, [&app, &all_ports, &all_objs] {
        auto& opts = app.configuration();
        return all_ports.add_port(opts, 0, smp::count,
            [](uint16_t port_id, uint16_t queue_num){
                return create_netstar_dpdk_net_device(port_id, queue_num);
        }).then([&opts, &all_ports]{
            return all_ports.add_port(opts, 1, smp::count,
                [](uint16_t port_id, uint16_t queue_num){
                    return create_fdir_device(port_id, queue_num);
            });
        }).then([&all_objs]{
            return all_objs.start(&all_objs);
        }).then([&all_ports, &all_objs]{
            return all_objs.invoke_on_all([&all_ports](simple_send_work_unit& wu){
                wu.configure_ports(all_ports, 0, 1);
            });
        })/*.then([&all_objs]{
            return all_objs.invoke_on(1, [](simple_send_work_unit& wu){
                wu.send_from_port_0();
            });
        }).then([&all_objs]{
            return all_objs.invoke_on(2, [](simple_send_work_unit& wu){
                wu.send_from_port_0();
            });
        }).then([&all_objs]{
            return all_objs.invoke_on(3, [](simple_send_work_unit& wu){
                wu.send_from_port_0();
            });
        }).then([&all_objs]{
            return all_objs.invoke_on(4, [](simple_send_work_unit& wu){
                wu.send_from_port_1();
            });
        }).then([&all_objs]{
            return all_objs.invoke_on(5, [](simple_send_work_unit& wu){
                wu.send_from_port_1();
            });
        })*/.then([&all_objs]{
            return all_objs.invoke_on(6, [](simple_send_work_unit& wu){
                wu.send_from_port_1();
            });
        });
    });
}
