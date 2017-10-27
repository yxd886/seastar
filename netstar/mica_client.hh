#ifndef _MICA_CLIENT
#define _MICA_CLIENT

#include "netstar/port.hh"
#include "netstar/work_unit.hh"
#include "netstar/mica_def.hh"
#include "netstar/roundup.hh"

#include "mica/util/hash.h"

#include "net/packet.hh"
#include "net/udp.hh"
#include "net/ip_checksum.hh"
#include "net/ip.hh"
#include "net/net.hh"
#include "net/byteorder.hh"

#include "core/future.hh"
#include "core/chunked_fifo.hh"
#include "core/scattered_message.hh"

#include <string>
#include <experimental/optional>
#include <utility>
#include <chrono>
#include <boost/program_options/variables_map.hpp>

namespace netstar {

using namespace seastar;
using namespace std::chrono_literals;

class kill_flow : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "killflow";
    }
};

class mica_client : public work_unit<mica_client>{
public:
    static constexpr unsigned max_req_len = ETHER_MAX_LEN - ETHER_CRC_LEN - sizeof(RequestBatchHeader);
    static constexpr unsigned max_payload_len =
            ETHER_MAX_LEN - ETHER_CRC_LEN - sizeof(net::eth_hdr) - sizeof(net::ip_hdr) - sizeof(net::udp_hdr);

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
        size_t _key_len;
        temporary_buffer<char> _key_buf;

        // record the previous sent value, cleared after each recycle
        size_t _val_len;
        temporary_buffer<char> _val_buf;

        // the associated promise with this request
        std::experimental::optional<promise<>> _pr;

        // maximum number of allowed timeout retries
        static constexpr unsigned max_retries = 4;
    public:
        // A series of actions that can be applied to request_descriptor.

        explicit request_descriptor(unsigned rd_index, std::function<void()> fn,
                                    RequestHeader rq_hd = RequestHeader{}) :
            _rd_index(rd_index), _epoch(0), _retry_count(0),
            _key_len(0), _key_buf(), _val_len(0), _val_buf() {
            _to.set_callback(std::move(fn));
        }

        void new_action(Operation op,
                        size_t key_len, temporary_buffer<char> key,
                        size_t val_len, temporary_buffer<char> val){
            // an assertion to make sure that the request_descriptor is in correct state.
            // If this method is called, the rd must be popped out from
            // the fifo. When the rd is popped out from the fifo, it is either in
            // initialized state, or be recycled. This means that:
            // 1. _pr contains nothing and associate with no continuation
            // 2. _retry_count is cleared and is zero
            // 3. _to timer is not armed.
            assert(!_pr && _retry_count == 0 && !_to.armed());
            assert(roundup<8>(key_len) == key.size() && roundup<8>(val_len) == val.size());

            _key_len = key_len;
            _key_buf = std::move(key);
            _val_len = val_len;
            _val_buf = std::move(val);

            // some assertions adopted form mica source code
            assert(_key_len < (1 << 8));
            assert(get_request_size()<=max_req_len);

            setup_request_header(op);
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

        void append_frags(scattered_message<char>& msg){
            // RequestHeader is a plain old object, doing reinterpret_cast here
            // is fine. Plus we have the guarantee of the lifetime of _rq_hd object.
            msg.append_static(reinterpret_cast<char*>(&_rq_hd), sizeof(RequestHeader));
            msg.append_static(_key_buf.get(), _key_buf.size());
            msg.append_static(_val_buf.get(), _val_buf.size());
        }

        size_t get_request_size(){
            return sizeof(RequestHeader)+_key_buf.size()+_val_buf.size();
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
        void setup_request_header(Operation& op){
            // set up the request header
            _rq_hd.operation = static_cast<uint8_t>(op);
            _rq_hd.result = static_cast<uint8_t>(Result::kSuccess);
            _rq_hd.key_hash =  mica::util::hash(_key_buf.get(), _key_len);
            _rq_hd.opaque = (static_cast<uint32_t>(_rd_index) << 16) |
                             static_cast<uint32_t>(_epoch);
            _rq_hd.reserved0 = 0;
            _rq_hd.kv_length_vec =
                static_cast<uint32_t>((_key_len << 24) | _val_len);
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
            // clear the promise
            _pr = std::experimental::nullopt;
            // no need to touch the _rq_hd
        }
        void timeout_recycle_prep(){
            // Preparation for a timeout recycle.
            // This is only triggered after consecutive timeout is triggered
            // without getting a response.
            assert(_retry_count == max_retries && _pr && !_to.armed());
            _epoch++;
            _retry_count = 0;
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

    public:
        explicit request_assembler(endpoint_info remote_ei,
                endpoint_info local_ei, port& p,
                std::vector<request_descriptor>& rds) :
                _remote_ei(remote_ei), _local_ei(local_ei),
                _port(p), _remaining_size(max_req_len),
                _rds(rds) {
            _batch_header_pkt = build_requet_batch_header();
            _batch_header_frag = _batch_header_pkt.frag(0);
            _rd_idxs.reserve(10);
        }

        void force_packet_out(){
            assert(_rd_idxs.size() > 0);

            scattered_message<char> msg;
            msg.reserve(1+3*_rd_idxs.size());

            // First, put the batch_header in to the _frags
            msg.append_static(_batch_header_frag.base, _batch_header_frag.size);

            // Then, for each request descriptor, put the request header
            // , key and value into the _frags.
            // After this, arm the timer for the request descriptor.
            for(auto rd_idx : _rd_idxs){
                _rds[rd_idx].append_frags(msg);
                _rds[rd_idx].arm_timer();
            }

            // build up the packet;
            net::packet p(std::move(msg).release());
            p.linearize();
            assert(p.nr_frags() == 1 && ETHER_MAX_LEN - ETHER_CRC_LEN - _remaining_size == p.len());

            setup_ip_udp_length(p);
            setup_request_num(p);
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
        void setup_ip_udp_length(net::packet& p){
            // payload length is : max_payload_len - _remainng_size
            // udp length is : max_payload_len + sizeof(net::udp_hdr) - _remainng_size
            // ip length is : udp length + sizeof(net::ip_hdr)
            auto udp_length = static_cast<uint16_t>(p.len()-sizeof(net::eth_hdr)-sizeof(net::ip_hdr));
            auto ip_length = static_cast<uint16_t>(p.len() - sizeof(net::eth_hdr));
            auto ip_hdr = p.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
            ip_hdr->len = net::hton(ip_length);
            auto udp_hdr = p.get_header<net::udp_hdr>(sizeof(net::eth_hdr) + sizeof(net::ip_hdr));
            udp_hdr->len = net::hton(udp_length);
        }
        void setup_request_num(net::packet& p){
            // set up num_requests in the batch header
            auto num_requests = p.get_header<uint8_t>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr)+sizeof(net::udp_hdr)+1);
            *num_requests = static_cast<uint8_t>(_rd_idxs.size());
        }
        void reset(){
            // reset the status of the request_assembler
            _rd_idxs.clear();
            _remaining_size = max_req_len;
        }
    };

private:
    static constexpr unsigned total_request_descriptor_count = 1024;
    static constexpr unsigned max_samephore_waiting_count = 2048;
    std::vector<request_descriptor> _rds;
    std::vector<request_assembler> _ras;
    semaphore _pending_work_queue = {total_request_descriptor_count};
    using rd_index = unsigned;
    circular_buffer<rd_index> _recycled_rds;
public:
    explicit mica_client(per_core_objs<mica_client>* all_objs) :
            work_unit<mica_client>(all_objs) {}

    mica_client(const mica_client& other) = delete;
    mica_client(mica_client&& other)  = delete;
    mica_client& operator=(const mica_client& other) = delete;
    mica_client& operator=(mica_client&& other) = delete;

    future<> stop(){
        return make_ready_future<>();
    }

    void bootup(boost::program_options::variables_map& opts){
        // Currently, we only support one port for mica client, and
        // one port for mica server.
        assert(ports().size() == 1);

        net::ethernet_address remote_ei_eth_addr(
                net::parse_ethernet_address(opts["mica-server-mac"].as<std::string>()));
        net::ipv4_address remote_ei_ip_addr(opts["mica-server-ip"].as<std::string>());
        uint16_t remote_ei_port_id = opts["mica-server-port-id"].as<uint16_t>();

        net::ethernet_address local_ei_eth_addr(ports()[0]->get_eth_addr());
        net::ipv4_address local_ei_ip_addr(opts["mica-client-ip"].as<std::string>());
        uint16_t local_ei_port_id = 0;

        // first, create request_assembler for each pair of remote endpoint
        // and local endpoint
        for(uint16_t remote_ei_core_id=0;
            remote_ei_core_id<opts["ms-smp-count"].as<uint16_t>();
            remote_ei_core_id++){
            uint16_t remote_ei_udp_port = remote_ei_core_id;
            uint16_t local_ei_core_id = static_cast<uint16_t>(engine().cpu_id());
            uint16_t local_ei_udp_port = local_ei_core_id;

            endpoint_info remote_ei_info(remote_ei_eth_addr, remote_ei_ip_addr, remote_ei_udp_port,
                                         std::make_pair(remote_ei_core_id, remote_ei_port_id));

            endpoint_info local_ei_info(local_ei_eth_addr, local_ei_ip_addr, local_ei_udp_port,
                                         std::make_pair(local_ei_core_id, local_ei_port_id));


            _ras.emplace_back(remote_ei_info, local_ei_info, *(ports()[0]), _rds);
        }

        // second, create all the request_descriptor
    }
private:
    void receive(net::packet p){
        if (!is_valid(p) || !is_response(p) || p.nr_frags()!=1){
            return;
        }

        size_t offset = sizeof(RequestBatchHeader);

        while(offset < p.len()){
            auto rh = p.get_header<RequestHeader>(offset);

            size_t key_len = (rh->kv_length_vec >> 24);
            size_t roundup_key_len = roundup<8>(key_len);
            size_t val_len = (rh->kv_length_vec & ((1 << 24) - 1));
            size_t roundup_val_len = roundup<8>(val_len);

            size_t key_offset = offset+sizeof(RequestHeader);
            size_t val_offset = key_offset + roundup_key_len;
            size_t next_req_offset = val_offset + roundup_val_len;

            unsigned rd_idx = static_cast<unsigned>(rh->opaque >> 16);


            auto action_res = _rds[rd_idx].match_response(*rh,
                                  p.share(key_offset, roundup_key_len),
                                  p.share(val_offset, roundup_val_len));

            switch (action_res){
            case action::no_action :
                break;
            case action::recycle_rd : {
                // recycle the request descriptor.
                break;
            }
            case action::resend_rd : {
                assert(false);
                break;
            }
            default:
                break;
            }

            offset = next_req_offset;
        }
        assert(offset == p.len());
    }
    bool is_valid(net::packet& p);
    bool is_response(net::packet& p) const;
};

} // namespace netstar

#endif // _MICA_CLIENT
