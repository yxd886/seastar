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

#include "netstar/netstar_dpdk_device.hh"

using namespace seastar;

int main(int ac, char** av) {
    return app_template().run_deprecated(ac, av, [] {
       printf("Thread %d: In the reactor loop\n", engine().cpu_id());
       auto ptr = netstar::create_netstar_dpdk_net_device();
       printf("Thread %d: netstar_dpdk_device is created\n", engine().cpu_id());
       engine().exit(0);
    });
}
