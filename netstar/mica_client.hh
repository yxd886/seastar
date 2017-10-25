#ifndef _MICA_CLIENT
#define _MICA_CLIENT

#include "netstar/port.hh"
#include "netstar/work_unit.hh"
#include "netstar/extendable_buffer.hh"
#include "netstar/mica_def.hh"

#include "mica/util/hash.h"

#include "net/packet.hh"
#include "net/udp.hh"
#include "net/ip_checksum.hh"
#include "net/ip.hh"
#include "net/net.hh"
#include "net/byteorder.hh"

#include "core/future.hh"
#include "core/chunked_fifo.hh"

#include <string>
#include <experimental/optional>
#include <utility>
#include <chrono>

namespace netstar {

using namespace seastar;
using namespace std::chrono_literals;

class kill_flow : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "killflow";
    }
};

class mica_client {
public:
    static constexpr unsigned max_req_len = ETHER_MAX_LEN - ETHER_CRC_LEN - sizeof(RequestBatchHeader);

    enum class action {
        recycle_rd,
        resend_rd,
        no_action
    };

    class request_descriptor{
        // index of the request descriptor from the vector.
        const uint16_t _rd_index;
        // record the epoch of the request descriptor, increase by 1 after each recycle
        uint16_t _epoch;

        // record the number of retries performed by the current request
        unsigned _retry_count;
        // a timer that is used to check request timeout
        timer<steady_clock_type> _to;

        // the request header
        RequestHeader _rq_hd;
        // record the previous sent key, cleared after each recycle
        extendable_buffer _key_buf;
        // record the previous sent value, cleared after each recycle
        extendable_buffer _val_buf;

        // the associated promise with this request
        std::experimental::optional<promise<>> _pr;

        static constexpr unsigned max_retries = 4;
    public:
        // A series of actions that can be applied to request_descriptor.

        explicit request_descriptor(unsigned rd_index, std::function<void()> fn) :
            _rd_index(rd_index), _epoch(0), _retry_count(0),
            _key_buf(64), _val_buf(256) {
            _to.set_callback(std::move(fn));
        }

        void new_action(Operation op, extendable_buffer key, extendable_buffer val){
            // an assertion to make sure that the request_descriptor is in correct state.
            // If this method is called, the rd must be popped out from
            // the fifo. When the rd is popped out from the fifo, it is either in
            // initialized state, or be recycled. This means that:
            // 1. _pr contains nothing and associate with no continuation
            // 2. _retry_count is cleared and is zero
            // 3. _to timer is not armed.
            assert(!_pr && _retry_count == 0 && !_to.armed());

            _key_buf = std::move(key);
            _val_buf = std::move(val);
            // some assertions adopted form mica source code
            assert(_key_buf.data_len() < (1 << 8));
            assert(roundup<8>(_val_buf.data_len()) +
                   roundup<8>(_key_buf.data_len()) +
                   sizeof(RequestHeader) <= max_req_len);

            // set up the request header
            _rq_hd.operation = static_cast<uint8_t>(op);
            _rq_hd.result = static_cast<uint8_t>(Result::kSuccess);
            _rq_hd.key_hash =  mica::util::hash(_key_buf.data(), _key_buf.data_len());;
            adjust_request_header_opaque();
            // _rq_hd.opaque = (static_cast<uint32_t>(_rd_index) << 16) |
            //                  static_cast<uint32_t>(_epoch);
            _rq_hd.reserved0 = 0;
            _rq_hd.kv_length_vec =
                static_cast<uint32_t>((_key_buf.data_len() << 24) | _val_buf.data_len());
        }

        future<> obtain_future(){
            // This is called after new_action is called. So we
            // perform the same assertion.
            assert(!_pr && _retry_count == 0 && !_to.armed());
            _pr = promise<>();
            return _pr->get_future();
        }

        void arm_timer(){
            assert( _retry_count < 4 && !_to.armed());
            _to.arm(1ms);
        }

        uint64_t get_key_hash(){
            return _rq_hd.key_hash;
        }
    public:
        action match_response(RequestHeader& res_hd, net::packet res_key, net::packet res_val){
            auto opaque = res_hd.opaque;
            uint16_t epoch = opaque & ((1 << 16) - 1);
            if(epoch != _epoch){
                // the epoch doesn't match, the response is a late response, ignore it.
                return action::no_action;
            }

            // the epoch matches, we got the response for this request.
            // Here, the timer should be armed and not timed out.
            // The retry count should not exceed the maximum value.
            // the _pr must be associated with some continuations.
            assert(_to.armed() && _retry_count < max_retries && _pr);

            if(res_hd.result != static_cast<uint8_t>(Result::kSuccess)){
                // If the operation fails, we force the flow down.
                _pr->set_exception(kill_flow());
            }
            else{
                // set the value for the current promise
                _pr->set_value();
            }

            normal_recycle_prep();
            return action::recycle_rd;
        }
        // The timeout handler
        action timeout_handler(){
            // handle _to timeout
            assert(_pr);

            _retry_count++;

            if(_retry_count == max_retries){
                // we have retried four times without receiving a response, timeout
                _pr->set_exception(kill_flow());
                timeout_recycle_prep();
                return action::recycle_rd;
            }
            else{
                // increase the epoch to discard late responses.
                _epoch++;
                adjust_request_header_opaque();
                return action::resend_rd;
            }
        }
    private:
        void adjust_request_header_opaque(){
            _rq_hd.opaque = (static_cast<uint32_t>(_rd_index) << 16) |
                             static_cast<uint32_t>(_epoch);
        }
        void normal_recycle_prep(){
            // Preparation for a normal recycle.
            // This is only triggered by correctly match a response.
            // We have the following assertions to check
            assert(_to.armed() && _retry_count < max_retries && _pr);
            // increase the epoch for a new request_descriptor
            _epoch++;
            // reset the _retyry_count to 0
            _retry_count = 0;
            // cancel the _to timer
            _to.cancel();
            // clear the data for both key buf and val buf
            _key_buf.clear_data();
            _val_buf.clear_data();
            // clear the promise
            _pr = std::experimental::nullopt;
            // no need to touch the _rq_hd
        }
        void timeout_recycle_prep(){
            // Preparation for a timeout recycle.
            // This is only triggered after consecutive timeout is triggered
            // without getting a response.
            assert(_retry_count == max_retries && _pr);
            // increase the epoch for a new request_descriptor
            _epoch++;
            // reset the _retyry_count to 0
            _retry_count = 0;
            // clear the data for both key buf and val buf
            _key_buf.clear_data();
            _val_buf.clear_data();
            // clear the promise
            _pr = std::experimental::nullopt;
            // no need to touch the _rq_hd
        }
    };

    // The request_assembler should be
    // launched for each partition of a server
    class request_assembler{
        // properties
        server_id _server_id;
        partition_id _partition_id;
        endpoint_id _remote_eid;
        lcore_id _lcore_id;
        port_id _port_id;
        bool _is_concurrent_assembler;

        // the packet to use
        net::packet _output_pkt;
        // the timer
        timer<> _to;

        // a vector of request descriptors
        // std::vector<request_descriptor>& _rds;
    public:
        explicit request_assembler(server_id sid,
                                   partition_id partid,
                                   endpoint_id eid,
                                   lcore_id lcid,
                                   port_id pid) :
            _server_id(sid), _partition_id(partid), _remote_eid(eid),
            _lcore_id(lcid), _port_id(pid), _is_concurrent_assembler(false) {}
        explicit request_assembler() :
            _server_id(server_id{0}),
            _partition_id(partition_id{0}),
            _remote_eid(std::make_pair(lcore_id{0},port_id{0})),
            _lcore_id(lcore_id{0}),
            _port_id(port_id{0}),
            _is_concurrent_assembler(true) {}

    private:
        net::packet build_requet_batch_header(std::string src_mac,
                                              std::string dst_mac,
                                              std::string src_ip,
                                              std::string dst_ip,
                                              uint16_t udp_src_port,
                                              uint16_t udp_dst_port);



    };
};

} // namespace netstar

#endif // _MICA_CLIENT
