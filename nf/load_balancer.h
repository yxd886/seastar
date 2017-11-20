#pragma once
#ifndef MICA_LOAD_BALANCER_H_
#define MICA_LOAD_BALANCER_H_



#include "mica/util/hash.h"

#include "nf/nf_common.h"
#include <vector>
#include <iostream>
#include <stdlib.h>
#include <time.h>


std::string lists[64]={"192.168.122.131","192.168.122.132","192.168.122.133","192.168.122.134",
                "192.168.122.135","192.168.122.136","192.168.122.137","192.168.122.138",
                "192.168.122.139","192.168.122.140","192.168.122.141","192.168.122.142"};

class Load_balancer{
public:
    Load_balancer(struct rte_ring** worker2interface,struct rte_ring** interface2worker,uint32_t cluster_id):
        _worker2interface(worker2interface),_interface2worker(interface2worker),_cluster_id(cluster_id),_drop(false){

    }


    void form_list(uint64_t backend_list_bit){
        _backend_list.clear();
        for (uint32_t i=0;i<sizeof(backend_list_bit);i++){
            uint64_t t=backend_list_bit&0x1;
            if(t==1) _backend_list.push_back(mica::network::NetworkAddress::parse_ipv4_addr(lists[i].c_str()));
            backend_list_bit=backend_list_bit>>1;
        }
    }

    uint32_t next_server(){
        srand((unsigned)time(nullptr));
        long unsigned int index=(long unsigned int)rand()%_backend_list.size();
        return _backend_list[index];
    }

    void process_packet(struct rte_mbuf* rte_pkt){
        struct ipv4_hdr *iphdr;
        struct tcp_hdr *tcp;
        unsigned lcore_id;
        _drop=false;

        lcore_id = rte_lcore_id();
        iphdr = rte_pktmbuf_mtod_offset(rte_pkt,
                struct ipv4_hdr *,
                sizeof(struct ether_hdr));

        if (iphdr->next_proto_id!=IPPROTO_TCP){
            //drop
            _drop=true;
            return;
        }else{

            tcp = (struct tcp_hdr *)((unsigned char *)iphdr +sizeof(struct ipv4_hdr));
            uint32_t server=0;
            char* key=nullptr;
            size_t key_length;
            uint64_t key_hash;
            struct fivetuple tuple(iphdr->src_addr,iphdr->dst_addr,tcp->src_port,tcp->dst_port,iphdr->next_proto_id);



			key= reinterpret_cast<char*>(&tuple);
			key_length= sizeof(tuple);
			key_hash= hash(key, key_length);
			struct rte_ring_item item(key_hash,key_length,key);
			item._state._action=READ;
			rte_ring_enqueue(_worker2interface[lcore_id],static_cast<void*>(&item));

			void* rev_item;
			rev_item=get_value(_interface2worker[lcore_id]);
			if(rev_item==nullptr){

				key= reinterpret_cast<char*>(&_cluster_id);
				key_length= sizeof(_cluster_id);
				key_hash= hash(key, key_length);
				uint64_t backend_list=0x1111111;
				struct rte_ring_item item(key_hash,key_length,key);
				rte_ring_enqueue(_worker2interface[lcore_id],static_cast<void*>(&item));
				void* rev_item;
				rev_item=get_value(_interface2worker[lcore_id]);
				struct session_state* ses_state=nullptr;

				if(rev_item==nullptr){
					//backend list is empty
					item._state._action=WRITE;
					 item._state._load_balancer_state._backend_list=backend_list;
					rte_ring_enqueue(_worker2interface[lcore_id],static_cast<void*>(&item));
					void* rev_item;
					rev_item=get_value(_interface2worker[lcore_id]);


				}else{

					ses_state=&(((struct rte_ring_item*)rev_item)->_state);


					backend_list=ses_state->_load_balancer_state._backend_list;
				}
				form_list(backend_list);

				server=next_server();

				//generate key and write hash table
				key= reinterpret_cast<char*>(&tuple);
				key_length= sizeof(tuple);
				key_hash= hash(key, key_length);
				struct rte_ring_item item1(key_hash,key_length,key);
				item1._state._action=WRITE;
				item1._state._load_balancer_state._dst_ip_addr=server;
				rte_ring_enqueue(_worker2interface[lcore_id],static_cast<void*>(&item1));

				if(DEBUG==1)  printf("try to dequeue from _interface2worker[%d]\n",lcore_id);
				rev_item=get_value(_interface2worker[lcore_id]);
				if(DEBUG==1)  printf("dequeue from _interface2worker[%d] completed\n",lcore_id);


				//To do:send packet to server
				iphdr->dst_addr=server;
				_drop=false;
				return;


			}else{

				struct session_state* ses_state=&(((struct rte_ring_item*)rev_item)->_state);
				server=ses_state->_load_balancer_state._dst_ip_addr;

				//To do: send this packet to address server.
				iphdr->dst_addr=server;
				_drop=false;
				return;
			}


        }
    }



struct rte_ring** _worker2interface;
struct rte_ring** _interface2worker;
uint32_t _cluster_id;
std::vector<uint32_t> _backend_list;
bool _drop;
};

#endif
