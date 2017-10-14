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
#include "netstar/env.hh"

using namespace seastar;

struct tester{
    netstar::per_core_objs<tester>* per_core_testers = nullptr;
    ~tester(){
        printf("Thread %d: tester object is destroyed\n", engine().cpu_id());
    }
    void call(int i){
        printf("Thread %d: test object 's call method is called with integer %d \n",
                engine().cpu_id(), i);
    }
    future<> stop() {
       printf("Thread %d: tester object is stopped\n", engine().cpu_id());
       return make_ready_future<>();
   }
    void set_testers(netstar::per_core_objs<tester>* per_core_testers){
        printf("Thread %d: testers is set.\n", engine().cpu_id());
        this->per_core_testers = per_core_testers;
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

    Base* get_impl(){
        return _work_unit_impl;
    }

    future<> stop() {
        printf("Thread %d: work_unit object is destroyed\n", engine().cpu_id());
        return make_ready_future<>();
    }
};

int main(int ac, char** av) {
    app_template app;
    // distributed<work_unit<tester>> server;
    netstar::per_core_objs<tester> per_core_testers_o;
    auto per_core_testers = & per_core_testers_o;

    return app.run_deprecated(ac, av, [&app, per_core_testers] {
        per_core_testers->start_on(0).then([per_core_testers] () {
            engine().at_exit([per_core_testers] () {
                return per_core_testers->stop().then([per_core_testers](){
                    return make_ready_future<>();
                });
            });
            return per_core_testers->start_on(1);
        }).then([per_core_testers] {
            return per_core_testers->invoke_on_all([](tester& local_inst){
                local_inst.call(1);
            });
        }).then_wrapped([] (future<> f) {
            try {
                f.get();
                return make_ready_future<>();
            } catch (...) {
                printf("Catch an exception\n");
                return make_ready_future<>();
            }
        }).then([per_core_testers]{
            return per_core_testers->invoke_on(0, &tester::call, 0);
        }).then([per_core_testers]{
            return per_core_testers->invoke_on(1, &tester::call, 1);
        }).then([per_core_testers]{
            return per_core_testers->invoke_on(2, &tester::call, 2);
        }).then_wrapped([] (future<> f) {
            try {
                f.get();
                return make_ready_future<>();
            } catch (netstar::no_per_core_obj&) {
                printf("Catch no_per_core_obj\n");
                return make_ready_future<>();
            }
        }).then([per_core_testers]{
            return per_core_testers->invoke_on(100, &tester::call, 100);
        }).then_wrapped([] (future<> f) {
            try {
                f.get();
                return make_ready_future<>();
            } catch (netstar::no_per_core_obj&) {
                printf("Catch no_per_core_obj\n");
                return make_ready_future<>();
            }
        });
        /*server.start_on(0).then([&server](){
            engine().at_exit([&server] () {
                return server.stop().then([](){
                    return make_ready_future<>();
                });
            });
            return server.invoke_on_all([](tester& local_inst){
                local_inst.call(1);
            });
        }).then_wrapped([] (future<> f) {
            try {
                f.get();
                return make_ready_future<>();
            } catch (...) {
                printf("Catch an exception\n");
                return make_ready_future<>();
            }
        }).then([&server] {
            return server.invoke_on_all([&server](tester& local_inst){
                local_inst.set_testers(&server);
            });
        }).then_wrapped([] (future<> f) {
            try {
                f.get();
                return make_ready_future<>();
            } catch (...) {
                printf("Catch an exception\n");
                return make_ready_future<>();
            }
        }).then([] {
            engine().exit(0);
        });*/

    });
}
