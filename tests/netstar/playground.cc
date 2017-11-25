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
#include "netstar/af/async_flow.hh"

using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

enum class fk_events : uint8_t{
    fk_me,
    fk_you,
    fk_everybody
};

class dummy_ppr{
private:
    side _s;
public:
    using EventEnumType = fk_events;
    using FlowKeyType = int;

    dummy_ppr(side s)
        : _s(s) {
    }
};

int main(int ac, char** av) {
    app_template app;
    timer<steady_clock_type> to;

    return app.run_deprecated(ac, av, [&app, &to]{
        async_flow_impl<dummy_ppr> af(1, 1);
    });


}
