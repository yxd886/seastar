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

} // namespace netstar

#endif // HAVE_DPDK
