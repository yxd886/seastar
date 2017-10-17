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
#include "netstar/netstar_dpdk_device.hh"
#include "net/udp.hh"

using namespace seastar;
using namespace std::chrono_literals;

struct stats_timer {
    uint64_t n_sent {};
    uint64_t n_received {};
    uint64_t n_failed {};
    void start(net::qp* qp) {
        assert(engine().cpu_id() == 0);
        _stats_timer.set_callback([this] {
            std::cout << "Out: " << n_sent << " pps, \t";
            std::cout << "Err: " << n_failed << " pps, \t";
            std::cout << "In: " << n_received << " pps" << std::endl;
            n_sent = 0;
            n_received = 0;
            n_failed = 0;
        });
        _stats_timer.arm_periodic(1s);

        // keep_doing([this, qp](){
           ipv4_addr ipv4_src_addr("10.1.2.4:666");
           ipv4_addr ipv4_dst_addr("10.1.2.4:666");

           const char* buf = "hello!";
           auto pkt = net::packet::from_static_data(buf, strlen(buf));

           auto hdr = pkt.prepend_header<net::udp_hdr>();
           hdr->src_port = ipv4_src_addr.port;
           hdr->dst_port = ipv4_dst_addr.port;
           hdr->len = pkt.len();
           *hdr = net::hton(*hdr);

           net::checksummer csum;
           net::ipv4_traits::udp_pseudo_header_checksum(csum, ipv4_src_addr, ipv4_dst_addr, pkt.len());
           csum.sum(pkt);
           hdr->cksum = csum.get();

           net::offload_info oi;
           oi.needs_csum = false;
           oi.protocol = net::ip_protocol_num::udp;
           pkt.set_offload_info(oi);

           auto iph = pkt.prepend_header<net::ip_hdr>();
           iph->ihl = sizeof(*iph) / 4;
           iph->ver = 4;
           iph->dscp = 0;
           iph->ecn = 0;
           iph->len = pkt.len();
           iph->id = 0;
           iph->frag = 0;
           iph->ttl = 64;
           iph->ip_proto = (uint8_t)net::ip_protocol_num::udp;
           iph->csum = 0;
           iph->src_ip = ipv4_src_addr.ip;
           iph->dst_ip = ipv4_dst_addr.ip;
           *iph = hton(*iph);
           net::checksummer ip_csum;
           ip_csum.sum(reinterpret_cast<char*>(iph), sizeof(*iph));
           iph->csum = csum.get();

           auto eh = pkt.prepend_header<net::eth_hdr>();
           net::ethernet_address eth_src{0x52, 0x54, 0x00, 0xfe, 0x22, 0x11};
           net::ethernet_address eth_dst{0x52, 0x54, 0x00, 0xfe, 0x22, 0x42};
           eh->dst_mac = eth_dst;
           eh->src_mac = eth_src;
           eh->eth_proto = uint16_t(net::eth_protocol_num::ipv4);
           *eh = hton(*eh);

           if(qp->peek_size() < 1024){
               qp->proxy_send(std::move(pkt));
               this->n_sent+=1;
           }
           else{
               this->n_failed+=1;
           }

        //   return make_ready_future<>();
        //});
    }
private:
    timer<> _stats_timer;
};


int main(int ac, char** av) {
    app_template app;
    std::unique_ptr<net::device> life_holder;
    net::qp* fst_qp;
    stats_timer timer;

    return app.run_deprecated(ac, av, [&app, &life_holder, &fst_qp, &timer] {
       auto& opts = app.configuration();

       life_holder = netstar::create_netstar_dpdk_net_device(0, smp::count);
       auto sem = std::make_shared<semaphore>(0);
       auto sdev = life_holder.get();

       for (unsigned i = 0; i < smp::count; i++) {
           smp::submit_to(i, [opts, sdev, &fst_qp] {
               uint16_t qid = engine().cpu_id();
               if (qid < sdev->hw_queues_count()) {
                   auto qp = sdev->init_local_queue(opts, qid);
                   if(qid == 0){
                       printf("fst_qp is set\n");
                       fst_qp = qp.get();
                   }
                   std::map<unsigned, float> cpu_weights;
                   for (unsigned i = sdev->hw_queues_count() + qid % sdev->hw_queues_count(); i < smp::count; i+= sdev->hw_queues_count()) {
                       cpu_weights[i] = 1;
                   }
                   cpu_weights[qid] = opts["hw-queue-weight"].as<float>();
                   qp->configure_proxies(cpu_weights);
                   qp->force_register_pkt_provider();
                   sdev->set_local_queue(std::move(qp));
               } else {
                   abort();
               }
           }).then([sem] {
               sem->signal();
           });
       }

       sem->wait(smp::count).then([opts, sdev, &timer, &fst_qp] {
           sdev->link_ready().then([opts, sdev, &timer, &fst_qp] {
               printf("Create device 0\n");
               timer.start(fst_qp);
           });
       });
    });
}
