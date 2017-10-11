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

struct tester{
    ~tester(){
        printf("Thread %d: tester object is destroyed\n", engine().cpu_id());
    }
    void call(int i){
        printf("Thread %d: test object 's call method is called with integer %d",
                engine().cpu_id(), i);
    }
};

template<class Base>
class work_unit{
    Base* _work_unit_impl;
public:

    template<typename... Args>
    work_unit(Args&&... args){
        std::unique_ptr<Base> ptr = std::make_unique<Base>(std::forward<Args>(args)...);
        _work_unit_impl = ptr.get();
        engine().at_destroy([ptr = std::move(ptr)](){});
    }

    template<typename Ret, typename... FuncArgs, typename... Args>
    Ret forward_to(Ret (Base::*func)(FuncArgs...), Args&&... args){
        return ((*_work_unit_impl).*func)(std::forward<Args>(args)...);
    }
};

int main(int ac, char** av) {
    app_template app;

    return app.run_deprecated(ac, av, [&app] {
       /*auto& opts = app.configuration();
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

       sem->wait(smp::count).then([opts, sdev] {
           sdev->link_ready().then([opts, sdev] {
               printf("Create device 0\n");

               auto fst_dev_ptr = netstar::create_netstar_dpdk_net_device(1, smp::count);
               printf("Thread %d: netstar_dpdk_device is created\n", engine().cpu_id());

               auto sem = std::make_shared<semaphore>(0);
               std::shared_ptr<net::device> new_sdev(fst_dev_ptr.release());

               for (unsigned i = 0; i < smp::count; i++) {
                  smp::submit_to(i, [opts, new_sdev] {
                      uint16_t qid = engine().cpu_id();
                      if (qid < new_sdev->hw_queues_count()) {
                          auto qp = new_sdev->init_local_queue(opts, qid);
                          std::map<unsigned, float> cpu_weights;
                          for (unsigned i = new_sdev->hw_queues_count() + qid % new_sdev->hw_queues_count(); i < smp::count; i+= new_sdev->hw_queues_count()) {
                              cpu_weights[i] = 1;
                          }
                          cpu_weights[qid] = opts["hw-queue-weight"].as<float>();
                          qp->configure_proxies(cpu_weights);
                          new_sdev->set_local_queue(std::move(qp));
                      } else {
                          abort();
                      }
                  }).then([sem] {
                      sem->signal();
                  });
               }

               sem->wait(smp::count).then([new_sdev] {
                   new_sdev->link_ready().then([new_sdev] {
                       printf("Create device 1\n");
                       engine().exit(0);
                   });
               });
           });
       });*/
        auto server = std::make_unique<distributed<work_unit<tester>>>();
        server->start().then([server = std::move(server)] () mutable {
            engine().at_exit([server] {
                return server->stop();
            });
            return server->invoke_on_all(&work_unit<tester>::forward_to, &tester::call, 1);
        }).then([] {
            engine().exit(0);
        });
    });
}
