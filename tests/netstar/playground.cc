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

using namespace seastar;
using namespace netstar;

class l2_processing{
    port& _in_port;
    port& _out_port;

    stream<net::packet> _arp_recv_stream;
    stream<net::packet> _ipv4_recv_stream;
public:
    void enable_l2_in(){
        _in_port.receive([this](net::packet pkt){
            auto eh = pkt.get_header<net::eth_hdr>();
            if(eh){
                auto eh_proto = net::ntoh(eh->eth_proto);

                switch(static_cast<net::eth_protocol_num>(eh_proto)){
                case net::eth_protocol_num::arp: {
                    _arp_recv_stream.produce(std::move(pkt));
                    return make_ready_future<>();
                }
                case net::eth_protocol_num::ipv4 : {
                    _ipv4_recv_stream.produce(std::move(pkt));
                    return make_ready_future<>();
                }
                default :{
                    return make_ready_future<>();
                }
                }
            }
            return make_ready_future<>();
        });
    }

    future<> l2_out(net::packet pkt){
        return _out_port.send(std::move(pkt));
    }

    const stream<net::packet>& get_arp_recv_stream(){
        return _arp_recv_stream;
    }

    const stream<net::packet>& get_ipv4_recv_stream(){
        return _ipv4_recv_stream;
    }

    template<typename... Items>
    subscription<Items...> set_stream_recv_fn(const stream<Items...>& which_stream,
                                              std::function<future<>(Items...)> fn){
        return which_stream.listen(std::move(fn));
    }

};

class l3_arp_processing {
    subscription<net::packet> _arp_pkt_sub;
    l2_processing& _l2;

public:
    void enable_l3_arp_in(){
        _arp_pkt_sub = _l2.set_stream_recv_fn(_l2.get_arp_recv_stream(), [this](net::packet){
           return make_ready_future<>();
        });
    }
};

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;

    return app.run_deprecated(ac, av, [&app, &all_ports]{
        auto& opts = app.configuration();

        std::unordered_map<std::string, net::ipv4_address> p0_addr_map;
        p0_addr_map["host-ipv4-addr"] = net::ipv4_address("10.28.1.13");
        p0_addr_map["gw-ipv4-addr"] = net::ipv4_address("10.28.1.1");
        p0_addr_map["netmask-ipv4-addr"] = net::ipv4_address("255.255.255.255");

        return all_ports.add_stack_port(opts, 0, smp::count, std::move(p0_addr_map)).then([&opts, &all_ports]{
            std::unordered_map<std::string, net::ipv4_address> p1_addr_map;
            p1_addr_map["host-ipv4-addr"] = net::ipv4_address("10.29.1.13");
            p1_addr_map["gw-ipv4-addr"] = net::ipv4_address("10.29.1.1");
            p1_addr_map["netmask-ipv4-addr"] = net::ipv4_address("255.255.255.255");

            return all_ports.add_stack_port(opts, 1, smp::count, std::move(p1_addr_map));
        }).then_wrapped([](auto&& f){
            try{
                f.get();
                printf("Finish creating two stack ports\n");
            }
            catch(...){
                printf("Error creating two stack ports\n");
            }
        });
    });
}
