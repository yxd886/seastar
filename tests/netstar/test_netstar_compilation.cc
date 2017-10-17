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

using namespace seastar;
using namespace std::chrono_literals;

struct stats_timer {
    uint64_t n_sent {};
    uint64_t n_received {};
    uint64_t n_failed {};
    void start(net::qp* qp) {
        _stats_timer.set_callback([this] {
            std::cout << "Out: " << n_sent << " pps, \t";
            std::cout << "Err: " << n_failed << " pps, \t";
            std::cout << "In: " << n_received << " pps" << std::endl;
            n_sent = 0;
            n_received = 0;
            n_failed = 0;
        });
        _stats_timer.arm_periodic(1s);

        keep_doing([this, qp](){
           const char* buf = "hello!";
           auto pkt = net::packet::from_static_data(buf, strlen(buf));
           qp->proxy_send(std::move(pkt));
           this->n_sent+=1;
           return make_ready_future<>();
        });
    }
private:
    timer<> _stats_timer;
};


int main(int ac, char** av) {
    app_template app;
    std::unique_ptr<net::device> life_holder;
    net::qp* fst_qp;
    stats_timer timer;

    return app.run_deprecated(ac, av, [&app, &life_holder, &fst_qp, &timer] {
       auto& opts = app.configuration();

       life_holder = netstar::create_netstar_dpdk_net_device(0, smp::count);
       auto sem = std::make_shared<semaphore>(0);
       auto sdev = life_holder.get();

       for (unsigned i = 0; i < smp::count; i++) {
           smp::submit_to(i, [opts, sdev, &fst_qp] {
               uint16_t qid = engine().cpu_id();
               if (qid < sdev->hw_queues_count()) {
                   auto qp = sdev->init_local_queue(opts, qid);
                   if(qid == 0){
                       printf("fst_qp is set\n");
                       fst_qp = qp.get();
                   }
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

       sem->wait(smp::count).then([opts, sdev, &timer, &fst_qp] {
           sdev->link_ready().then([opts, sdev, &timer, &fst_qp] {
               printf("Create device 0\n");
               timer.start(fst_qp);
           });
       });
    });
}
