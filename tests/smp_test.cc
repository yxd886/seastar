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
/*
#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/print.hh"

using namespace seastar;

future<bool> test_smp_call() {
    return smp::submit_to(1, [] {
        return make_ready_future<int>(3);
    }).then([] (int ret) {
        return make_ready_future<bool>(ret == 3);
    });
}

struct nasty_exception {};

future<bool> test_smp_exception() {
    print("1\n");
    return smp::submit_to(1, [] {
        print("2\n");
        auto x = make_exception_future<int>(nasty_exception());
        print("3\n");
        return x;
    }).then_wrapped([] (future<int> result) {
        print("4\n");
        try {
            result.get();
            return make_ready_future<bool>(false); // expected an exception
        } catch (nasty_exception&) {
            // all is well
            return make_ready_future<bool>(true);
        } catch (...) {
            // incorrect exception type
            return make_ready_future<bool>(false);
        }
    });
}

int tests, fails;

future<>
report(sstring msg, future<bool>&& result) {
    return std::move(result).then([msg] (bool result) {
        print("%s: %s\n", (result ? "PASS" : "FAIL"), msg);
        tests += 1;
        fails += !result;
    });
}

int main(int ac, char** av) {
    return app_template().run_deprecated(ac, av, [] {
       return report("smp call", test_smp_call()).then([] {
           return report("smp exception", test_smp_exception());
       }).then([] {
           print("\n%d tests / %d failures\n", tests, fails);
           engine().exit(fails ? 1 : 0);
       });
    });
}
*/
#include "core/seastar.hh"
#include "core/reactor.hh"
#include "core/future-util.hh"
#include <iostream>
#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/print.hh"



/*
const char* canned_response = "Seastar is the future!\n";

seastar::future<> service_loop() {
    seastar::listen_options lo;
    lo.reuse_address = true;
    return seastar::do_with(seastar::listen(seastar::make_ipv4_address({1234}), lo),
            [] (auto& listener) {
        return seastar::keep_doing([&listener] () {
            return listener.accept().then(
                [] (seastar::connected_socket s, seastar::socket_address a) {
            		std::cout << "Accepted connection from " << a << "\n";
            		auto out = s.output();
                    return seastar::do_with(std::move(s), std::move(out),
                        [] (auto& s, auto& out) {
                            return out.write(canned_response).then([&out] {
                                return out.close();
			    });
		    });
	        });
        });
    });
}


seastar::future<> f() {
    return seastar::parallel_for_each(boost::irange<unsigned>(0, seastar::smp::count),
            [] (unsigned c) {
        return seastar::smp::submit_to(c, service_loop);
    });
}

int main(int argc, char** argv) {
    seastar::app_template app;
    try {
        app.run(argc, argv, f);
    } catch(...) {
        std::cerr << "Couldn't start application: "
                  << std::current_exception() << "\n";
        return 1;
    }
    return 0;
}*/
int number=0;

seastar::future<> get(){
    // promises an int will be produced eventually
    return make_ready_future<int>(number);
}
seastar::future<> put(int i){
    // promises to store an int
    number=i;
    return make_ready_future<>();
}

seastar::future<> loop_to(int end) {
    if (number == end) {
        print("loop end\n");
        return make_ready_future<>();
    }
    print("number: %d \n",number);
    get().then([end] (int value) {
        return put(value + 1);
    }).then([end] {
        return loop_to(end);
    });
}

int main(int ac, char** av) {
    seastar::app_template app;
    return app.run_deprecated(ac, av, [] {
            return loop_to(100);
    });
}

