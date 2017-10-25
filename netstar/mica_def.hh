#ifndef _MICA_DEF
#define _MICA_DEF

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

#include <array>
#include <cstdint>
#include <utility>

#include "net/packet.hh"
#include "net/udp.hh"
#include "net/ip_checksum.hh"
#include "net/ip.hh"
#include "net/net.hh"
#include "net/byteorder.hh"

using namespace seastar;

namespace netstar{

struct endpoint_info{
    net::ethernet_address eth_addr;
    net::ipv4_address ip_addr;
    uint16_t core_id;

    explicit endpoint_info(net::ethernet_address eaddr,
                           net::ipv4_address ipaddr,
                           uint16_t cid) :
            eth_addr(eaddr), ip_addr(ipaddr), core_id(cid) {}
};

struct server_id{
    uint16_t val;
};

struct lcore_id{
    uint16_t val;
};

struct port_id{
    uint16_t val;
};

struct partition_id{
    uint16_t val;
};

using endpoint_id = std::pair<lcore_id, port_id>;

struct RequestBatchHeader {
    // 0
    uint8_t header[sizeof(ether_hdr) + sizeof(ipv4_hdr) + sizeof(udp_hdr)];
    // 42
    uint8_t magic;  // 0x78 for requests; 0x79 for responses.
    uint8_t num_requests;
    // 44
    uint32_t reserved0;
    // 48
    // KeyValueRequestHeader
};

struct RequestHeader {
    // 0
    uint8_t operation;  // ::mica::processor::Operation
    uint8_t result;     // ::mica::table::Result
    // 2
    uint16_t reserved0;
    // 4
    uint32_t kv_length_vec;  // key_length: 8, value_length: 24
    // 8
    uint64_t key_hash;
    // 16
    uint32_t opaque;
    uint32_t reserved1;
    // 24
    // Key-value data
};

enum class Operation : uint8_t{
    kReset = 0,
    kNoopRead,
    kNoopWrite,
    kAdd,
    kSet,
    kGet,
    kTest,
    kDelete,
    kIncrement,
};

enum class Result : uint8_t {
    kSuccess = 0,
    kError,
    kInsufficientSpace,
    kExists,
    kNotFound,
    kPartialValue,
    kNotProcessed,
    kNotSupported,
    kTimedOut,
    kRejected,
};

} // namespace netstar

#endif // _MICA_CLIENT_DEF
