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

class mica_client {
public:

    class request_descriptor{
        // index of the request descriptor from the vector.
        const unsigned _rd_index;
        // record the epoch of the request descriptor, increase by 1 after each recycle
        uint16_t _epoch;

        // record the number of retries performed by the current request
        unsigned _retry_count;
        // a timer that is used to check request timeout
        timer<> _to;

        // the request header
        RequestHeader _rq_hd;
        // record the previous sent key, cleared after each recycle
        extendable_buffer _key_buf;
        // record the previous sent value, cleared after each recycle
        extendable_buffer _val_buf;

        // the associated promise with this request
        std::experimental::optional<promise<>> _pr;

    public:
        explicit request_descriptor(unsigned rd_index, std::function<void()> fn) :
            _rd_index(rd_index), _epoch(0), _retry_count(0),
            _key_buf(64), _val_buf(256) {
            _to.set_callback(std::move(fn));
        }

        void new_action(Operation op, extendable_buffer key, extendable_buffer val){
            assert(!_pr && _retry_count == 0 && !_to.armed());
            _key_buf = std::move(key);
            _val_buf = std::move(val);

            mica::util::hash(_key_buf.data(), _key_buf.data_len());


        }
    private:
        void recycle(){
            _epoch++;
            _retry_count = 0;
            _to.cancel();
            _key_buf.clear_data();
            _val_buf.clear_data();

            // make sure that the promise has been set and cleared.
            assert(!_pr);
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
