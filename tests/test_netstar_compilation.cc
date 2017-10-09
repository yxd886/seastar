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
    app_template app;

    return app.run_deprecated(ac, av, [&app] {
       auto& opts = app.configuration();
       printf("Thread %d: In the reactor loop\n", engine().cpu_id());

       auto fst_dev_ptr = netstar::create_netstar_dpdk_net_device(0, smp::count);
       printf("Thread %d: netstar_dpdk_device is created\n", engine().cpu_id());

       auto sem = std::make_shared<semaphore>(0);
       std::shared_ptr<net::device> sdev(fst_dev_ptr.release());

       for (unsigned i = 0; i < smp::count; i++) {
           smp::submit_to(i, [opts, sdev] {
               uint16_t qid = engine().cpu_id();
               if (qid < sdev->hw_queues_count()) {
                   auto qp = sdev->init_local_queue(opts, qid);
                   std::map<unsigned, float> cpu_weights;
                   for (unsigned i = sdev->hw_queues_count() + qid % sdev->hw_queues_count(); i < smp::count; i+= sdev->hw_queues_count()) {
                       cpu_weights[i] = 1;
                   }
                   cpu_weights[qid] = opts["hw-queue-weight"].as<float>();
                   qp->configure_proxies(cpu_weights);
                   sdev->set_local_queue(std::move(qp));
               } else {
                   abort();
               }
           }).then([sem] {
               sem->signal();
           });
       }

       sem->wait(smp::count).then([sdev] {
           sdev->link_ready().then([sdev] {
               printf("Finish constructing all the qp\n");
               engine().exit(0);
           });
       });


       // auto snd_dev_ptr = netstar::create_netstar_dpdk_net_device(1, 4);
       // printf("Thread %d: netstar_dpdk_device is created\n", engine().cpu_id());
    });
}
