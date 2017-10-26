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

        // maximum number of allowed timeout retries
        static constexpr unsigned max_retries = 4;
    public:
        // A series of actions that can be applied to request_descriptor.

        explicit request_descriptor(unsigned rd_index, std::function<void()> fn,
                                    RequestHeader rq_hd = RequestHeader{}) :
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
            assert(get_request_size()<=max_req_len);

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

        void append_frags(std::vector<net::fragment>& frags){
            // RequestHeader is a plain old object, doing reinterpret_cast here
            // is fine. Plus we have the guarantee of the lifetime of _rq_hd object.
            frags.push_back(net::fragment{reinterpret_cast<char*>(&_rq_hd), sizeof(RequestHeader)});

            frags.push_back(_key_buf.fragment());
            frags.push_back(_val_buf.fragment());
        }

        size_t get_request_size(){
            return sizeof(RequestHeader)+roundup<8>(_key_buf.data_len())+roundup<8>(_val_buf.data_len());
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
            _epoch++;
            _retry_count = 0;
            _key_buf.clear_data();
            _val_buf.clear_data();
            _pr = std::experimental::nullopt;
        }
    };

    // The request_assembler should be
    // launched for each partition of a server
    class request_assembler{
        endpoint_info _remote_ei;
        endpoint_info _local_ei;
        port& _port;

        // the batch packet header
        net::packet _batch_header_pkt;
        net::fragment _batch_header_frag;

        // remaining request packet size
        unsigned _remaining_size;
        // This vector records the requests that are going
        // to be sent out in a single packet.
        std::vector<int> _rd_idxs;

        // a vector of request descriptors
        std::vector<request_descriptor>& _rds;

        // fragments to append
        std::vector<net::fragment> _frags;
    public:
        explicit request_assembler(endpoint_info remote_ei,
                endpoint_info local_ei, port& p,
                std::vector<request_descriptor>& rds) :
                _remote_ei(remote_ei), _local_ei(local_ei),
                _port(p), _remaining_size(max_req_len),
                _rds(rds) {
            _batch_header_pkt = build_requet_batch_header();
            _batch_header_frag = _batch_header_pkt.frag(0);
            int reqs_reserve_num = 5;
            _rd_idxs.reserve(reqs_reserve_num);
            _frags.reserve(3*reqs_reserve_num+1);
        }

        void force_packet_out(){
            assert(_rd_idxs.size() > 0);

            // First, put the batch_header in to the _frags
            _frags.push_back(_batch_header_frag);

            // Then, for each request descriptor, put the request header
            // , key and value into the _frags.
            // After this, arm the timer for the request descriptor.
            for(auto rd_idx : _rd_idxs){
                _rds[rd_idx].append_frags(_frags);
                _rds[rd_idx].arm_timer();
            }

            // build up the packet;
            net::packet p(_frags, deleter());
            p.linearize();
            assert(p.nr_frags() == 1 && ETHER_MAX_LEN - ETHER_CRC_LEN - _remaining_size == p.len());

            // send the packet out and reset
            _port.send(std::move(p));
            reset();
        }

        void append_new_request_descriptor(unsigned rd_index){
            auto total_request_size = _rds[rd_index].get_request_size();
            if(_remaining_size < total_request_size){
                force_packet_out();
            }
            _rd_idxs.push_back(rd_index);
            _remaining_size -= total_request_size;
        }

        bool need_force_out(){
            return _rd_idxs.size()>0;
        }

    private:
        net::packet build_requet_batch_header();
        void reset(){
            // reset the status of the request_assembler
            _rd_idxs.clear();
            _frags.clear();
            _remaining_size = max_req_len;
        }
    };
};

} // namespace netstar

#endif // _MICA_CLIENT
