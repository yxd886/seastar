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
#include "netstar/work_unit.hh"

using namespace seastar;
using namespace netstar;

class simple_send_work_unit : public work_unit<simple_send_work_unit>{
public:
    explicit simple_send_work_unit(per_core_objs<simple_send_work_unit>* all_objs) :
        work_unit<simple_send_work_unit>(all_objs) {}
};

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;

    return app.run_deprecated(ac, av, [&app, &all_ports] {
        auto& opts = app.configuration();
        return all_ports.add_port(opts, 0, smp::count,
            [](uint16_t port_id, uint16_t queue_num){
                return create_netstar_dpdk_net_device(port_id, queue_num);
        }).then([&opts, &all_ports]{
            return all_ports.add_port(opts, 1, smp::count,
                [](uint16_t port_id, uint16_t queue_num){
                    return create_netstar_dpdk_net_device(port_id, queue_num);
            });
        }).then([]{
            printf("All the devices are successfully created\n");
            engine().exit(0);
        });
    });
}
