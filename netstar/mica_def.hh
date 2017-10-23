#ifndef _MICA_DEF
#define _MICA_DEF

#include <rte_ethdev.h>
#include <array>

namespace netstar{

// Mica definition of server partion information
struct ServerPartitionInfo {
    volatile uint16_t owner_lcore_id;
};

// Mica definition of server endpoint information
struct ServerEndpointInfo {
    ether_addr mac_addr;
    uint32_t ipv4_addr;
    uint16_t udp_port;

    // The following is used for diagnosis only.
    uint32_t eid;
    uint16_t owner_lcore_id;

    // Local endpoint IDs that can talk to a remote endpoint.
    // For now, this remains static once probing is done.
    // std::unordered_set<EndpointId> valid_local_eids;
    // std::unordered_set<EndpointId> invalid_local_eids;
};

// Mica definition for server
struct Server {
    // Partitioning scheme.
    uint8_t concurrent_read;
    uint8_t concurrent_write;
    uint16_t partition_count;
    uint16_t endpoint_count;

    // Remote endpoint on the server.  Indexed by the endpoint index.
    std::array<ServerEndpointInfo, /*StaticConfig::kMaxEndpointsPerServer = */ 32> endpoints;

    // Partitions on the server.  Indexed by the partitionId.
    std::array<ServerPartitionInfo, /*StaticConfig::kMaxPartitionsPerServer = */ 256> partitions;

    // For diagnosis only.
    std::string server_name;
};

// Mica definition for the endpoint
struct EndpointInfo {
    uint16_t owner_lcore_id;

    // Statistics counter.
    // I think I can remove them later.
    volatile uint64_t rx_bursts;
    volatile uint64_t rx_packets;
    volatile uint64_t tx_bursts;
    volatile uint64_t tx_packets;
    volatile uint64_t tx_dropped;

    // Specific to DPDK.
    // Values copied from Port.
    ether_addr mac_addr;
    uint32_t ipv4_addr;
    uint16_t numa_id;

    // UDP port for flow direction.
    uint16_t udp_port;

    uint16_t port_id;
    uint16_t queue_id;
};



} // namespace netstar

#endif // _MICA_CLIENT_DEF
