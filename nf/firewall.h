#pragma once
#ifndef FIREWALL_H_
#define FIREWALL_H_


#include "nf/nf_common.h"
#include <vector>
#include <iostream>
using namespace seastar;



class Firewall{
public:
    Firewall(struct rte_ring** worker2interface,struct rte_ring** interface2worker):
		_worker2interface(worker2interface),_interface2worker(interface2worker),_drop(false){

    	if(DEBUG==1) printf("Initializing a firewall\n");
    	auto rules_config = ::mica::util::Config::load_file("firewall.json").get("rules");
        for (size_t i = 0; i < rules_config.size(); i++) {
            auto rule_conf = rules_config.get(i);
            uint16_t src_port = ::mica::util::safe_cast<uint16_t>(
	    		    rule_conf.get("src_port").get_uint64());
            uint16_t dst_port = ::mica::util::safe_cast<uint16_t>(
                rule_conf.get("dst_port").get_uint64());

            uint32_t src_addr = ::mica::network::NetworkAddress::parse_ipv4_addr(
                rule_conf.get("src_addr").get_str().c_str());
            uint32_t dst_addr = ::mica::network::NetworkAddress::parse_ipv4_addr(
                rule_conf.get("dst_addr").get_str().c_str());
            struct rule r(src_addr,dst_addr,src_port,dst_port);
            rules.push_back(r);


        }

    }

    struct firewall_state* update_state(struct firewall_state* firewall_state_ptr,struct tcp_hdr *tcp){


        struct firewall_state* return_state=new firewall_state(tcp->tcp_flags,tcp->sent_seq,tcp->recv_ack);
        return_state->_pass=firewall_state_ptr->_pass;
        return return_state;

	}

	void check_session(struct fivetuple* five,firewall_state* state){

		std::vector<rule>::iterator it;
		for(it=rules.begin();it!=rules.end();it++){
		    if(five->_dst_addr==it->_dst_addr&&five->_dst_port==it->_dst_port&&five->_src_addr==it->_src_addr&&five->_src_port==it->_src_port){
		        state->_pass=false;
			}
		}
		state->_pass=true;

	}

	bool state_changed(struct firewall_state* src,struct firewall_state* dst){
		if(src->_tcp_flags!=dst->_tcp_flags||src->_recv_ack!=dst->_recv_ack||src->_sent_seq!=dst->_sent_seq){
			return true;
		}
		return false;
	}
	void init_state(firewall_state& fs){
	    fs._pass=false;
	    fs._recv_ack=0;
	    fs._sent_seq=0;
	    fs._tcp_flags=0;
	}
	future<> process_packet(struct rte_mbuf* rte_pkt){


		if(DEBUG==1) printf("processing firewall on core:%d\n",rte_lcore_id());

		net::ip_hdr *iphdr;
		net::tcp_hdr *tcp;
	    _drop=false;


        iphdr =rte_pkt->get_header<net::ip_hdr>(sizeof(net::eth_hdr));


	    if (iphdr->ip_proto!=(uint8_t)net::ip_protocol_num::tcp){
		    //drop
	    	if(DEBUG==1) printf("not tcp pkt\n");
	        _drop=true;
	        return make_ready_future<>;
	    }else{

            tcp = (net::tcp_hdr *)((unsigned char *)iphdr +sizeof(net::ip_hdr));
	        struct fivetuple tuple(iphdr->src_ip.ip,iphdr->dst_ip.ip,tcp->src_port,tcp->dst_port,iphdr->next_proto_id);

	        //printf("src_addr:%d ,iphdr->dst_addr:%d tcp->src_port:%d tcp->dst_port:%d\n ",iphdr->src_addr,iphdr->dst_addr,tcp->src_port,tcp->dst_port);


            //generate key based on five-tuples
            struct firewall_state state;

	        char* key = reinterpret_cast<char*>(&tuple);
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            extendable_buffer val_buf;
            val_buf.fill_data(state);

            //generate rte_ring_item

	        if(DEBUG==1)	printf("key_hash:%d, key_length:%d, key: ox%x\n",key_hash,key_length,key);

	        if(DEBUG==1)  printf("try to enqueue to _worker2interface[%d] \n",lcore_id);
	        rte_ring_enqueue(_worker2interface[lcore_id],static_cast<void*>(&item));
	        if(DEBUG==1)  printf("enqueue to _worker2interface[%d] completed\n",lcore_id);
	        void* rev_item;
	        if(DEBUG==1)  printf("try to dequeue from _interface2worker[%d]\n",lcore_id);
	        rev_item=get_value(_interface2worker[lcore_id]);
	        if(DEBUG==1)  printf("dequeue from _interface2worker[%d] completed\n",lcore_id);
	        struct session_state* ses_state=nullptr;

	        if(rev_item==nullptr){ //new session
                //create new state and check whether this session can pass the firewall.
	        	if(DEBUG==1)	printf("create new state \n");
	            ses_state= new session_state();
	            ses_state->_action=WRITE;
	            check_session(&tuple,&(ses_state->_firewall_state));

	        }else{

	            ses_state=&(((struct rte_ring_item*)rev_item)->_state);
	        }

            //update_state
	        struct firewall_state* fw_state=update_state(&(ses_state->_firewall_state),tcp);


	        if(state_changed(&(ses_state->_firewall_state),fw_state)){
                //write updated state into mica hash table.

	            item._state._action=WRITE;
	            item._state._firewall_state.copy(fw_state);
	            if(DEBUG==1)  printf("try to enqueue to _worker2interface[%d] \n",lcore_id);
	            rte_ring_enqueue(_worker2interface[lcore_id],static_cast<void*>(&item));
	            if(DEBUG==1)  printf("enqueue to _worker2interface[%d] completed\n",lcore_id);

		        if(DEBUG==1)  printf("try to dequeue from _interface2worker[%d]\n",lcore_id);
		        rev_item=get_value(_interface2worker[lcore_id]);
		        if(DEBUG==1)  printf("dequeue from _interface2worker[%d] completed\n",lcore_id);
	        }

	        if(ses_state->_firewall_state._pass==true){
                //pass
	            _drop=false;
	            return;
	        }else{
                //drop
	            _drop=true;
	            return;
	        }



	    }


    }

	std::vector<rule> rules;
	bool _drop;

};


#endif
