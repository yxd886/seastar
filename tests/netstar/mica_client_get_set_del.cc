/*
 * This file contains basic tests for the get, set and delete functionality
 * of our mica client implementation.
 * Especially, if the mica server is configured with only 1 end-point, then
 * set large objects will generate two request packets, the first one is
 * almost full, the second one is generated because the request can not be
 * appended to the first request packet.
 *
 */
//#include "net/tcp.hh"
#include "nf/ips.h"
#include "nf/firewall.h"
#include "netstar/port.hh"
#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/print.hh"
#include "core/distributed.hh"
#include "netstar/per_core_objs.hh"
#include "netstar/mica_client.hh"
#include "netstar/extendable_buffer.hh"
#include "netstar/port_env.hh"




using namespace seastar;
using namespace netstar;

// sizeof(large_object) == 104
// roundup<8>(sizeof(large_object)) == 104
// uint64_t key size is 8
// the RequestHeader is 24
// A kSet request for large_object is 136
// max_req_len is 1466
// A single packet can hold at most 10 large_object
struct large_object{
    unsigned id;
    char unused_buf[100];
};

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;
    per_core_objs<mica_client> all_objs;
    vector<vector<port_pair>> queue_map;

    promise<> lo_send_pr;
    unsigned lo_send_count = 0;
    bool lo_send_error_hapen = false;

    promise<> lo_read_pr;
    unsigned lo_read_count = 0;
    bool lo_read_error_hapen = false;

    return app.run_deprecated(ac, av, [&app, &all_ports, &all_objs, &queue_map,
                                       &lo_send_pr, &lo_send_count, &lo_send_error_hapen,
                                       &lo_read_pr, &lo_read_count, &lo_read_error_hapen]{
        auto& opts = app.configuration();
        return all_ports.add_port(opts, 1, smp::count, port_type::fdir).then([&all_objs]{
            return all_objs.start(&all_objs);
        }).then([&all_ports, &all_objs]{
            return all_objs.invoke_on_all([&all_ports](mica_client& mc){
                mc.configure_ports(all_ports, 0, 0);
            });
        }).then([&opts, &all_ports, &queue_map]{
            queue_map = calculate_queue_mapping(
                    opts, all_ports.local_port(0));
        }).then([&all_objs, &opts, &queue_map]{
            return all_objs.invoke_on_all([&opts, &queue_map](mica_client& mc){
                mc.bootup(opts, queue_map);
            });
        }).then([&all_objs]{
            return all_objs.invoke_on_all([](mica_client& mc){
                mc.start_receiving();
            });
        }).then([&all_objs]{
            uint64_t key = 10276325;
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            uint64_t val = 8721;
            extendable_buffer val_buf;
            val_buf.fill_data(val);

            printf("Trying to set key %zu to value %zu\n", key, val);
            return all_objs.local_obj().query(Operation::kSet,
                    sizeof(key), key_buf.get_temp_buffer(),
                    sizeof(val), val_buf.get_temp_buffer()).then([key, val](mica_response response){
                assert(response.get_key_len() == 0);
                assert(response.get_val_len() == 0);
                assert(response.get_result() == Result::kSuccess);
                printf("The key %zu is set to value %zu\n", key, val);
            });
        }).then([&all_objs]{
            uint64_t key = 10276325;
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            printf("Trying to read key %zu\n", key);
            return all_objs.local_obj().query(Operation::kGet,
                    sizeof(key), key_buf.get_temp_buffer(),
                    0, temporary_buffer<char>()).then([key](mica_response response){
                assert(response.get_key_len() == 0);
                assert(response.get_val_len() == sizeof(uint64_t));
                assert(response.get_result() == Result::kSuccess);
                assert(response.get_value<uint64_t>() == 8721);
                printf("The key %zu is read as value %zu\n", key, response.get_value<uint64_t>());
            });
        }).then([&all_objs]{
            uint64_t key = 10276325;
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            printf("Trying to delete key %zu\n", key);
            return all_objs.local_obj().query(Operation::kDelete,
                    sizeof(key), key_buf.get_temp_buffer(),
                    0, temporary_buffer<char>()).then([key](mica_response response){
                assert(response.get_key_len() == 0);
                assert(response.get_val_len() == 0);
                assert(response.get_result() == Result::kSuccess);
                printf("The key %zu is deleted\n", key);
            });
        }).then([&all_objs]{
            uint64_t key = 10276325;
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            printf("Trying to read key %zu again\n", key);
            return all_objs.local_obj().query(Operation::kGet,
                    sizeof(key), key_buf.get_temp_buffer(),
                    0, temporary_buffer<char>()).then([key](mica_response response){
                assert(response.get_key_len() == 0);
                assert(response.get_val_len() == 0);
                assert(response.get_result() == Result::kNotFound);
                printf("The key %zu is not found\n", key);
            });
        }).then([&all_objs]{
            uint64_t key = 10276326;
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            printf("Trying to read another key %zu\n", key);
            return all_objs.local_obj().query(Operation::kGet,
                    sizeof(key), key_buf.get_temp_buffer(),
                    0, temporary_buffer<char>()).then([key](mica_response response){
                assert(response.get_key_len() == 0);
                assert(response.get_val_len() == 0);
                assert(response.get_result() == Result::kNotFound);
                printf("The key %zu is not found\n", key);
            });
        }).then([&all_objs, &lo_send_pr, &lo_send_count, &lo_send_error_hapen]{
            for(uint64_t i=0; i<11; i++){
                // Query 11 times with large_object
                uint64_t key = 10276327+i;
                extendable_buffer key_buf;
                key_buf.fill_data(key);

                large_object lo;
                lo.id = i;
                extendable_buffer val_buf;
                val_buf.fill_data(lo);

                all_objs.local_obj().query(Operation::kSet,
                        sizeof(key), key_buf.get_temp_buffer(),
                        sizeof(lo), val_buf.get_temp_buffer()).then([key, i](mica_response response){
                    assert(response.get_key_len() == 0);
                    assert(response.get_val_len() == 0);
                    assert(response.get_result() == Result::kSuccess);
                    printf("Large object with index %zu is set to key %zu\n", i, key);
                }).then_wrapped([&lo_send_pr, &lo_send_count, &lo_send_error_hapen](auto&& f){
                    lo_send_count += 1;
                    try{
                        f.get();
                    }
                    catch(...){
                        lo_send_error_hapen = true;
                    }
                    if(lo_send_count == 11){
                        // All the queries have been finished.
                        if(lo_send_error_hapen){
                            lo_send_pr.set_exception(std::runtime_error("wtf"));
                        }
                        else{
                            lo_send_pr.set_value();
                        }
                    }
                });
            }
            return lo_send_pr.get_future();
        }).then([&all_objs, &lo_read_pr, &lo_read_count, &lo_read_error_hapen]{
            for(uint64_t i=0; i<11; i++){
                // Query 11 times with large_object
                uint64_t key = 10276327+i;
                extendable_buffer key_buf;
                key_buf.fill_data(key);

                all_objs.local_obj().query(Operation::kGet,
                        sizeof(key), key_buf.get_temp_buffer(),
                        0, temporary_buffer<char>()).then([key](mica_response response){
                    assert(response.get_key_len() == 0);
                    assert(response.get_val_len() == sizeof(large_object));
                    assert(response.get_result() == Result::kSuccess);
                    printf("Large object with key %zu has index %d\n", key, response.get_value<large_object>().id);
                }).then_wrapped([&all_objs, &lo_read_pr, &lo_read_count, &lo_read_error_hapen](auto&& f){
                    lo_read_count += 1;
                    try{
                        f.get();
                    }
                    catch(...){
                        lo_read_error_hapen = true;
                    }
                    if(lo_read_count == 11){
                        // All the queries have been finished.
                        if(lo_read_error_hapen){
                            lo_read_pr.set_exception(std::runtime_error("wtf"));
                        }
                        else{
                            lo_read_pr.set_value();
                        }
                    }
                });
            }
            return lo_read_pr.get_future();
        }).then_wrapped([](auto&& f){
            try{
                f.get();
                engine().exit(0);
            }
            catch(...){
                printf("Failure happens\n");
                engine().exit(0);
            }
        });
    });
}
