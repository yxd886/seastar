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
#include "netstar/port.hh"
#include "netstar/extendable_buffer.hh"
#include "netstar/mica_client.hh"

using namespace seastar;
using namespace netstar;

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;
    mica_client::request_descriptor c(1, []{printf("timer called!\n");});

    return app.run_deprecated(ac, av, [&app, &all_ports, &c] {
        int i = 10;

        extendable_buffer b1(5);
        extendable_buffer b2(5);

        b1.fill_data(i);
        b2.fill_data(i);

        auto b1_len = b1.data_len();
        auto b2_len = b2.data_len();

        c.new_action(Operation::kSet, b1_len, b1.get_temp_buffer(), b2_len, b2.get_temp_buffer());
        c.obtain_future().then_wrapped([](auto&& f){
            try{
                f.get();
                printf("Get the response\n");
            }
            catch(kill_flow&){
                printf("Catch kill_flow exception\n");
            }
        });
        c.arm_timer();

        RequestHeader r;
        r.result = static_cast<uint8_t>(Result::kSuccess);
        r.opaque = static_cast<uint32_t>(1<<16|0);

        auto ret = c.match_response(r, net::packet(), net::packet());
        assert(ret == mica_client::action::recycle_rd);

        std::cout << "net::eth_hdr " << sizeof(net::eth_hdr) <<std::endl;
        std::cout << "net::ip_hdr " << sizeof(net::ip_hdr) <<std::endl;
        std::cout << "net::udp_hdr " << sizeof(net::udp_hdr) <<std::endl;
        std::cout << "eth_hdr " << sizeof(ether_hdr) <<std::endl;
        std::cout << "ip_hdr " << sizeof(ipv4_hdr) <<std::endl;
        std::cout << "udp_hdr " << sizeof(udp_hdr) <<std::endl;

        return make_ready_future<>().then([]{
            engine().exit(0);
        });
    });
}
