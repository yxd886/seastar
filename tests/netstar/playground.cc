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
#include "netstar/async_flow.hh"

using namespace seastar;
using namespace netstar;

class mock_monitor{
    circular_buffer<net::packet> _receiveq;
    bool _end;
    std::experimental::optional<promise<>> _new_pkt_promise;
public:
    mock_monitor(size_t size)
        : _receiveq(size)
        , _end(false) {
    }

    future<> on_new_packet(){
        assert(!_new_pkt_promise);
        if(_receiveq.size()>0){
            return make_ready_future<>();
        }
        _new_pkt_promise = promise<>();
        return _new_pkt_promise->get_future();
    }

    void receive_pkt(net::packet pkt){
        _receiveq.push_back(std::move(pkt));
        if(_new_pkt_promise){
            _new_pkt_promise->set_value();
        }
    }

    net::packet read_packet(){
        net::packet p(_receiveq.front());
        _receiveq.pop_front();
        return p;
    }

    void set_end(){
        _end = true;
    }

    bool get_end(){
        return _end;
    }
};

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;
    // timer<steady_clock_type>

    return app.run_deprecated(ac, av, [&app, &all_ports]{
        auto monitor = make_lw_shared<mock_monitor>(5);
        auto monitor_ptr = monitor.get();
        return do_until([monitor]{return monitor->get_end();}, [monitor_ptr]{
            return monitor_ptr->on_new_packet().then([monitor_ptr]{
                // monitor_ptr->read_packet();
                printf("monitor receives new packet");
            });
        });
    });
}
