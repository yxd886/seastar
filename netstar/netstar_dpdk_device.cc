/*
 * netstar
 */

#ifdef HAVE_DPDK

#include <cinttypes>
#include "core/posix.hh"
#include "core/vla.hh"
#include "net/virtio-interface.hh"
#include "core/reactor.hh"
#include "core/stream.hh"
#include "core/circular_buffer.hh"
#include "core/align.hh"
#include "core/sstring.hh"
#include "core/memory.hh"
#include "core/metrics.hh"
#include "util/function_input_iterator.hh"
#include "util/transform_iterator.hh"
#include <atomic>
#include <vector>
#include <queue>
#include <experimental/optional>
#include <boost/preprocessor.hpp>
#include "net/ip.hh"
#include "net/const.hh"
#include "core/dpdk_rte.hh"
#include "netstar_dpdk_device.hh"
#include "net/toeplitz.hh"

#include <getopt.h>
#include <malloc.h>

#include <cinttypes>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_pci.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_memzone.h>

using namespace seastar;

namespace netstar{

#if RTE_VERSION <= RTE_VERSION_NUM(2,0,0,16)

static
inline
char*
rte_mbuf_to_baddr(rte_mbuf* mbuf) {
    return reinterpret_cast<char*>(RTE_MBUF_TO_BADDR(mbuf));
}

void* as_cookie(struct rte_pktmbuf_pool_private& p) {
    return reinterpret_cast<void*>(uint64_t(p.mbuf_data_room_size));
};

#else

void* as_cookie(struct rte_pktmbuf_pool_private& p) {
    return &p;
};

#endif

#ifndef MARKER
typedef void    *MARKER[0];   /**< generic marker for a point in a structure */
#endif

namespace netstar_dpdk{

/******************* Net device related constatns *****************************/
static constexpr uint16_t default_ring_size      = 512;

//
// We need 2 times the ring size of buffers because of the way PMDs
// refill the ring.
//
static constexpr uint16_t mbufs_per_queue_rx     = 2 * default_ring_size;
static constexpr uint16_t rx_gc_thresh           = 64;

//
// No need to keep more descriptors in the air than can be sent in a single
// rte_eth_tx_burst() call.
//
static constexpr uint16_t mbufs_per_queue_tx     = 2 * default_ring_size;

static constexpr uint16_t mbuf_cache_size        = 512;
static constexpr uint16_t mbuf_overhead          =
                                 sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM;
//
// We'll allocate 2K data buffers for an inline case because this would require
// a single page per mbuf. If we used 4K data buffers here it would require 2
// pages for a single buffer (due to "mbuf_overhead") and this is a much more
// demanding memory constraint.
//
static constexpr size_t   inline_mbuf_data_size  = 2048;

//
// Size of the data buffer in the non-inline case.
//
// We may want to change (increase) this value in future, while the
// inline_mbuf_data_size value will unlikely change due to reasons described
// above.
//
static constexpr size_t   mbuf_data_size         = 2048;

// (INLINE_MBUF_DATA_SIZE(2K)*32 = 64K = Max TSO/LRO size) + 1 mbuf for headers
static constexpr uint8_t  max_frags              = 32 + 1;

//
// Intel's 40G NIC HW limit for a number of fragments in an xmit segment.
//
// See Chapter 8.4.1 "Transmit Packet in System Memory" of the xl710 devices
// spec. for more details.
//
static constexpr uint8_t  i40e_max_xmit_segment_frags = 8;

//
// VMWare's virtual NIC limit for a number of fragments in an xmit segment.
//
// see drivers/net/vmxnet3/base/vmxnet3_defs.h VMXNET3_MAX_TXD_PER_PKT
//
static constexpr uint8_t vmxnet3_max_xmit_segment_frags = 16;

static constexpr uint16_t inline_mbuf_size       =
                                inline_mbuf_data_size + mbuf_overhead;

} // namespace netstar_dpdk


} // namespace netstar

#endif // HAVE_DPDK
