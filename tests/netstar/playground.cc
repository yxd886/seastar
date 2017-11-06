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
            /*std::unordered_map<std::string, net::ipv4_address> p1_addr_map;
            p1_addr_map["host-ipv4-addr"] = net::ipv4_address("10.29.1.13");
            p1_addr_map["gw-ipv4-addr"] = net::ipv4_address("10.29.1.1");
            p1_addr_map["netmask-ipv4-addr"] = net::ipv4_address("255.255.255.255");

            return all_ports.add_stack_port(opts, 1, smp::count, std::move(p1_addr_map));*/
            return make_ready_future<>();
        }).then_wrapped([](auto&& f){
            try{
                f.get();
                printf("Finish creating two stack ports\n");
            }
            catch(...){
                printf("Error creating two stack ports\n");
            }

            engine().exit(0);
        });
    });
}
