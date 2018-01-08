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
#include "core/print.hh"
#include "core/sleep.hh"

#include "net/udp.hh"
#include "net/ip_checksum.hh"
#include "net/ip.hh"
#include "net/net.hh"
#include "net/packet.hh"
#include "net/byteorder.hh"

#include "netstar/work_unit.hh"
#include "netstar/port_env.hh"
// Change #1
#include "netstar/af/cb_async_flow.hh"
#include "netstar/mica_client.hh"
#include "netstar/extendable_buffer.hh"

#include "bess/bess_flow_gen.hh"


#include <vector>
#include <iostream>
#include "nf/aho-corasick/fpp.h"
#include "nf/aho-corasick/aho.h"
#define MAX_MATCH 8192
#include <stdlib.h>
#include <time.h>

using namespace seastar;
using namespace netstar;
using namespace std;
using namespace std::chrono_literals;
using std::vector;

enum class dummy_udp_events : uint8_t{
    pkt_in=0
};

class dummy_udp_ppr{
private:
    bool _is_client;
    timer<lowres_clock> _t;
    std::function<void(bool)> _close_fn;
public:
    using EventEnumType = dummy_udp_events;
    using FlowKeyType = net::l4connid<net::ipv4_traits>;
    using HashFunc = net::l4connid<net::ipv4_traits>::connid_hash;

    dummy_udp_ppr(bool is_client, std::function<void(bool)> close_fn)
        : _is_client(is_client)
        , _close_fn(std::move(close_fn)){
        _t.set_callback([this]{
            // fprint(std::cout, "timer called.\n");
            this->_close_fn(this->_is_client);
        });
        _t.arm(3s);
    }

public:
    generated_events<EventEnumType> handle_packet_send(net::packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen(dummy_udp_events::pkt_in);
        if(_t.armed()) {
            _t.cancel();
            _t.arm(3s);
        }
        return ge;
    }

    generated_events<EventEnumType> handle_packet_recv(net::packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen(dummy_udp_events::pkt_in);
        return ge;
    }

    FlowKeyType get_reverse_flow_key(net::packet& pkt){
        auto ip_hd_ptr = pkt.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
        auto udp_hd_ptr = pkt.get_header<net::udp_hdr>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr));
        return FlowKeyType{net::ntoh(ip_hd_ptr->src_ip),
                           net::ntoh(ip_hd_ptr->dst_ip),
                           net::ntoh(udp_hd_ptr->src_port),
                           net::ntoh(udp_hd_ptr->dst_port)};
    }

public:
    struct async_flow_config {
        static constexpr int max_event_context_queue_size = 5;
        static constexpr int new_flow_queue_size = 1000;
        static constexpr int max_flow_table_size = 100000;
        static constexpr int max_directions = 2;

        static FlowKeyType get_flow_key(net::packet& pkt){
            auto ip_hd_ptr = pkt.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
            auto udp_hd_ptr = pkt.get_header<net::udp_hdr>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr));
            return FlowKeyType{net::ntoh(ip_hd_ptr->dst_ip),
                               net::ntoh(ip_hd_ptr->src_ip),
                               net::ntoh(udp_hd_ptr->dst_port),
                               net::ntoh(udp_hd_ptr->src_port)};
        }
    };
};
std::string lists[64]={"192.168.122.131","192.168.122.132","192.168.122.133","192.168.122.134",
                "192.168.122.135","192.168.122.136","192.168.122.137","192.168.122.138",
                "192.168.122.139","192.168.122.140","192.168.122.141","192.168.122.142"};

class Load_balancer{
public:
    Load_balancer(): _cluster_id(0){

    }


    uint32_t _cluster_id;

};

class forwarder;

distributed<forwarder> forwarders;

class forwarder {
    port& _ingress_port;
    std::experimental::optional<subscription<net::packet>> _ingress_port_sub;

    port& _egress_port;
    std::experimental::optional<subscription<net::packet>> _egress_port_sub;

    // Change #2
    cb_async_flow_manager<dummy_udp_ppr> _udp_manager;
    cb_async_flow_manager<dummy_udp_ppr>::external_io_direction _udp_manager_ingress;
    cb_async_flow_manager<dummy_udp_ppr>::external_io_direction _udp_manager_egress;

    mica_client& _mc;

public:
    forwarder (ports_env& all_ports, per_core_objs<mica_client>& mica_clients)
        : _ingress_port(std::ref(all_ports.local_port(0)))
        , _egress_port(std::ref(all_ports.local_port(1)))
        , _udp_manager_ingress(0)
        , _udp_manager_egress(1)
        , _mc(std::ref(mica_clients.local_obj())){
    }

    future<> stop(){
        return make_ready_future<>();
    }

    future<> mica_test(int ) {
        // only test mica performance on thread 1.
        return repeat([this]{
            uint64_t key = engine().cpu_id()+1;
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            return _mc.query(Operation::kGet,sizeof(key), key_buf.get_temp_buffer(),
                             0, temporary_buffer<char>()).then([this](mica_response response){
                assert(response.get_result() == Result::kNotFound);

                uint64_t key = engine().cpu_id()+1;
                extendable_buffer key_buf;
                key_buf.fill_data(key);

                uint64_t val = 6;
                extendable_buffer val_buf;
                val_buf.fill_data(val);
                return _mc.query(Operation::kSet, sizeof(key), key_buf.get_temp_buffer(),
                                 sizeof(val), val_buf.get_temp_buffer());
            }).then([this](mica_response response){
                assert(response.get_result() == Result::kSuccess);

                uint64_t key = engine().cpu_id()+1;
                extendable_buffer key_buf;
                key_buf.fill_data(key);

                return _mc.query(Operation::kGet,sizeof(key), key_buf.get_temp_buffer(),
                                 0, temporary_buffer<char>());
            }).then([this](mica_response response){
                assert(response.get_value<uint64_t>() == 6);

                uint64_t key = engine().cpu_id()+1;
                extendable_buffer key_buf;
                key_buf.fill_data(key);

                return _mc.query(Operation::kDelete,
                                 sizeof(key), key_buf.get_temp_buffer(),
                                 0, temporary_buffer<char>());
            }).then([this](mica_response response){
                assert(response.get_result() == Result::kSuccess);
            }).then_wrapped([](auto&& f){
                try{
                    f.get();
                    fprint(std::cout, "mica_test succeeds on core %d!\n", engine().cpu_id());
                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                }
                catch(...) {
                    fprint(std::cout, "mica_test fails on core %d, retry in 3s.\n", engine().cpu_id());
                    return sleep(3s).then([]{
                        return stop_iteration::no;
                    });
                }
            });
        });
    }

    void configure(int i) {

        auto udp_manager_ingress_output_fn = [this](net::packet pkt) {
            // fprint(std::cout, "udp_manager_ingress_output_fn receives.\n");
            auto eth_h = pkt.get_header<net::eth_hdr>(0);
            eth_h->src_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x62};
            eth_h->dst_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x07, 0x82};
            _ingress_port.send(std::move(pkt));
            return make_ready_future<>();
        };

        auto udp_manager_egress_output_fn = [this](net::packet pkt) {
            // fprint(std::cout, "udp_manager_egress_output_fn receives.\n");
            auto eth_h = pkt.get_header<net::eth_hdr>(0);
            eth_h->src_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x60};
            eth_h->dst_mac = net::ethernet_address{0x3c, 0xfd, 0xfe, 0x06, 0x08, 0x00};
            _ingress_port.send(std::move(pkt));
            return make_ready_future<>();
        };

        _udp_manager_ingress.register_to_manager(_udp_manager,
                                                 std::move(udp_manager_egress_output_fn),
                                                 _udp_manager_egress);

        _udp_manager_egress.register_to_manager(_udp_manager,
                                                std::move(udp_manager_ingress_output_fn),
                                                _udp_manager_ingress);

        _ingress_port_sub.emplace(_ingress_port.receive([this](net::packet pkt){

            auto eth_h = pkt.get_header<net::eth_hdr>(0);
            if(!eth_h) {
                return make_ready_future<>();
            }

            if(net::ntoh(eth_h->eth_proto) == static_cast<uint16_t>(net::eth_protocol_num::ipv4)) {
                auto ip_h = pkt.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
                if(!ip_h) {
                    return make_ready_future<>();
                }

                if(ip_h->ip_proto == static_cast<uint8_t>(net::ip_protocol_num::udp)) {
                    auto udp_h = pkt.get_header<net::udp_hdr>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr));
                    if(!udp_h) {
                        return make_ready_future<>();
                    }

                    dummy_udp_ppr::FlowKeyType fk{net::ntoh(ip_h->dst_ip),
                                                  net::ntoh(ip_h->src_ip),
                                                  net::ntoh(udp_h->dst_port),
                                                  net::ntoh(udp_h->src_port)};
                    _udp_manager_ingress.get_send_stream().produce(std::move(pkt), &fk);
                    return make_ready_future<>();

                }
                else{
                    return make_ready_future<>();
                }
            }
            else{
                return make_ready_future<>();
            }
        }));
    }

    struct lb_flow_state{
        uint32_t _dst_ip_addr;
        uint64_t _backend_list;


    };

    struct query_key {
        uint64_t v1;
        uint64_t v2;
    };


    // Change #3.
    class lb_runner {
        cb_async_flow<dummy_udp_ppr> _ac;
        forwarder& _f;
        lb_flow_state _fs;
        std::vector<uint32_t> _backend_list;
        uint64_t backend_list_bit;
        uint32_t server;
    public:
        lb_runner(cb_async_flow<dummy_udp_ppr> ac, forwarder& f)

            : _ac(std::move(ac))
            , _f(f){}

        void events_registration() {
            _ac.register_events(dummy_udp_events::pkt_in);
        }


        void close_cb() {
            delete this;
        }

        void flow_write_cb(int err_code, mica_response res) {
            if(err_code!=0){
                _ac.drop_cur_packet();
                return;
            }
            net::ip_hdr* ip_hd= _ac.cur_packet().get_header<net::ip_hdr>(sizeof(net::eth_hdr));
            ip_hd->dst_ip.ip=server;
            _ac.forward_cur_packet();
        }
        void cluster_write_cb(int err_code, mica_response res) {
            if(err_code!=0){
                _ac.drop_cur_packet();
                return;
            }
            form_list(backend_list_bit);
            server=next_server();
            _fs._dst_ip_addr=server;
            auto key = query_key{_ac.get_flow_key_hash(), _ac.get_flow_key_hash()};
            _f._mc.query_with_cb(Operation::kSet, mica_key(key), mica_value(_fs), [this](int err_code, mica_response res){
                flow_write_cb(err_code, std::move(res));
            });
        }
        void flow_read_cb(int err_code, mica_response res) {
            if(err_code!=0){
                _ac.drop_cur_packet();
                return;
            }
            if(res.get_result() == Result::kNotFound) {
                backend_list_bit=0x1111111;
                auto key = query_key{_f.lb._cluster_id, _f.lb._cluster_id};
                _f._mc.query_with_cb(Operation::kGet, mica_key(key),
                            mica_value(0, temporary_buffer<char>()), [this](int err_code, mica_response res){
                    cluster_read_cb(err_code, std::move(res));
                });
            }
            else{
                _fs = res.get_value<lb_flow_state>();
                server=_fs._dst_ip_addr;
                net::ip_hdr* ip_hd= _ac.cur_packet().get_header<net::ip_hdr>(sizeof(net::eth_hdr));
                ip_hd->dst_ip.ip=server;
                _ac.forward_cur_packet();
            }


        }
        void cluster_read_cb(int err_code, mica_response res) {
            if(err_code!=0){
                _ac.drop_cur_packet();
                return;
            }
            if(res.get_result() == Result::kNotFound) {
                _fs._backend_list=backend_list_bit;
                auto key = query_key{_f.lb._cluster_id, _f.lb._cluster_id};
                _f._mc.query_with_cb(Operation::kSet, mica_key(key), mica_value(_fs), [this](int err_code, mica_response res){
                    cluster_write_cb(err_code, std::move(res));
                });
            }
            else{
                _fs = res.get_value<lb_flow_state>();
                backend_list_bit=_fs._backend_list;
                form_list(backend_list_bit);
                server=next_server();
                _fs._dst_ip_addr=server;
                auto key = query_key{_ac.get_flow_key_hash(), _ac.get_flow_key_hash()};
                _f._mc.query_with_cb(Operation::kSet, mica_key(key), mica_value(_fs), [this](int err_code, mica_response res){
                    flow_write_cb(err_code, std::move(res));
                });

            }


        }
        void pkt_cb() {
            if(_ac.cur_event().on_close_event()) {
                _ac.drop_cur_packet();
                _ac.unregister_packet_cb();
                return;
            }

            auto key = query_key{_ac.get_flow_key_hash(), _ac.get_flow_key_hash()};
            _f._mc.query_with_cb(Operation::kGet, mica_key(key),
                        mica_value(0, temporary_buffer<char>()), [this](int err_code, mica_response res){
                flow_read_cb(err_code, std::move(res));
            });
        }

        void run_lb() {
            _ac.register_cbs([this]{pkt_cb();}, [this]{close_cb();});
        }

        /*future<> run_lb() {

            return _ac.run_async_loop([this](){
                if(_ac.cur_event().on_close_event()) {
                    return make_ready_future<af_action>(af_action::close_forward);
                }
                auto key = query_key{_ac.get_flow_key_hash(), _ac.get_flow_key_hash()};
                return _f._mc.query(Operation::kGet, mica_key(key),
                        mica_value(0, temporary_buffer<char>())).then([this](mica_response response){

                    bool state_changed = true;
                   if(res.get_result() == Result::kNotFound) {
                       initialize_session_state(_ac.cur_packet(), _fs);
                   }
                   else{
                       _fs = res.get_value<lb_flow_state>();
                      state_changed = update_session_state(_ac.cur_packet(), _fs);
                   }

                   if(state_changed) {
                       auto key = query_key{_ac.get_flow_key_hash(), _ac.get_flow_key_hash()};
                       return _f._mc.query(Operation::kSet, mica_key(key),
                               mica_value(_fs)).then([this](mica_response response){
                           return forward_packet(_fs);
                       });
                   }
                   else{
                       forward_packet(_fs);
                   }

                }).then_wrapped([this](auto&& f){
                    try{
                        auto result = f.get0();
                        return result;

                    }
                    catch(...){


                        return af_action::drop;
                    }
                });
            });
        }*/

        void form_list(uint64_t backend_list_bit){
            _backend_list.clear();
            for (uint32_t i=0;i<sizeof(backend_list_bit);i++){
                uint64_t t=backend_list_bit&0x1;
                if(t==1) _backend_list.push_back(::net::ipv4_address(lists[i].c_str()).ip);
                backend_list_bit=backend_list_bit>>1;
            }
        }

        uint32_t next_server(){
            srand((unsigned)time(nullptr));
            long unsigned int index=(long unsigned int)rand()%_backend_list.size();
            return _backend_list[index];
        }

    };

    void run_udp_manager(int) {
        repeat([this]{
            return _udp_manager.on_new_initial_context().then([this]() mutable {
                auto ic = _udp_manager.get_initial_context();


                auto runner = new lb_runner(ic.get_cb_async_flow(), (*this));
                runner->events_registration();
                runner->run_lb();
                return stop_iteration::no;
            });
        });
    }

    struct info {
        uint64_t ingress_received;
        uint64_t egress_send;
        unsigned egress_failed_send;
        size_t active_flow_num;

        unsigned mica_timeout_error;
        unsigned insufficient_mica_rd_erorr;

        void operator+=(const info& o) {
            ingress_received += o.ingress_received;
            egress_send += o.egress_send;
            egress_failed_send += o.egress_failed_send;
            active_flow_num += o.active_flow_num;
            mica_timeout_error += o.mica_timeout_error;
            insufficient_mica_rd_erorr += o.insufficient_mica_rd_erorr;
        }
    };
    info _old{0,0,0,0,0,0};
    unsigned _mica_timeout_error = 0;
    unsigned _insufficient_mica_rd_erorr=0;

    future<info> get_info() {
        /*return make_ready_future<info>(info{_ingress_port.get_qp_wrapper().rx_pkts(),
                                            _egress_port.get_qp_wrapper().tx_pkts(),
                                            _egress_port.peek_failed_send_cout(),
                                            _udp_manager.peek_active_flow_num()});*/
        return make_ready_future<info>(info{_ingress_port.get_qp_wrapper().rx_pkts(),
                                            _ingress_port.get_qp_wrapper().tx_pkts(),
                                            _ingress_port.peek_failed_send_cout(),
                                            _udp_manager.peek_active_flow_num(),
                                            _mica_timeout_error,
                                            _insufficient_mica_rd_erorr});
    }
    void collect_stats(int) {
        repeat([this]{
            return forwarders.map_reduce(adder<info>(), &forwarder::get_info).then([this](info i){
                fprint(std::cout, "ingress_received=%d, egress_send=%d, egress_failed_send=%d, active_flow_num=%d, mica_timeout_error=%d, insufficient_mica_rd_erorr=%d.\n",
                        i.ingress_received-_old.ingress_received,
                        i.egress_send - _old.egress_send,
                        i.egress_failed_send - _old.egress_failed_send,
                        i.active_flow_num,
                        i.mica_timeout_error - _old.mica_timeout_error,
                        i.insufficient_mica_rd_erorr - _old.insufficient_mica_rd_erorr);
                _old = i;
            }).then([]{
                return seastar::sleep(1s).then([]{
                    return stop_iteration::no;
                });
            });
        });
    }
public:

    Load_balancer lb;

};

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;
    per_core_objs<mica_client> mica_clients;
    vector<vector<port_pair>> queue_map;

    return app.run_deprecated(ac, av, [&app, &all_ports, &mica_clients, &queue_map] {
        auto& opts = app.configuration();
        return all_ports.add_port(opts, 0, smp::count, port_type::netstar_dpdk).then([&opts, &all_ports]{
            return all_ports.add_port(opts, 1, smp::count, port_type::fdir);
        }).then([&mica_clients]{
           return mica_clients.start(&mica_clients);
        }).then([&all_ports, &mica_clients]{
            return mica_clients.invoke_on_all([&all_ports](mica_client& mc){
                mc.configure_ports(all_ports, 1, 1);
            });
        }).then([&opts, &all_ports, &queue_map]{
            queue_map = calculate_queue_mapping(opts, all_ports.local_port(1));
        }).then([&mica_clients, &opts, &queue_map]{
            return mica_clients.invoke_on_all([&opts, &queue_map](mica_client& mc){
                mc.bootup(opts, queue_map);
            });
        }).then([&mica_clients]{
            return mica_clients.invoke_on_all([](mica_client& mc){
                mc.start_receiving();
            });
        }).then([&all_ports, &mica_clients]{
            return forwarders.start(std::ref(all_ports), std::ref(mica_clients));
        })/*.then([]{
            return forwarders.invoke_on_all(&forwarder::mica_test, 1);
        }).then([]{
            return forwarders.invoke_on_all(&forwarder::mica_test, 1);
        }).then([]{
            return forwarders.invoke_on_all(&forwarder::mica_test, 1);
        })*/.then([]{
            return forwarders.invoke_on_all(&forwarder::configure, 1);
        }).then([]{
            return forwarders.invoke_on_all(&forwarder::run_udp_manager, 1);
        }).then([]{
            return forwarders.invoke_on(0, &forwarder::collect_stats, 1);
        }).then([]{
            fprint(std::cout, "forwarder runs!\n");
            engine().at_exit([]{
                return forwarders.stop();
            });
        });
    });
}

// 1 thread forwarder, sender use static udp traffic gen, 700000 total pps, 1000 flows
// The system barely crashes, which is good.

// When turn on mica debug mode, mica will fail to made an assertion about memory alignment in the packet
// This is easy to reproduce even when 1 flow with 1pps. Please check this out carefully!

// 1r: 8.5M.
// 1r1w: 5.35M
// 2r2w: 3.4M
// 3r3w: 2.43M
