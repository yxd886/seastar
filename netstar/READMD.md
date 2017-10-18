#Netstar framework for buildling asynchronous network functions.

* This contains the source code of netstar framework for building asynchronous network functions. It is built on seastar framework.

* Currently, we only modify seastar to add a customized device class, and modify some part for packet handling. No further modifications are made to seastar.

* For netstar, we plan several core abstractions for building asynchronous network functions.
1. core/meta_flow: meta_flow is a generalized flow abstraction that is bound to each NIC of the server. meta_flow processes all the packets received from a NIC. It has a programmable interface that is exposed to the programmer.
2. core/flow_distributor: a key-value based flow distributor that can be used by the meta_flow to distribute flows to the actual async_flow class. Currently, we can feed a generalized flow_key information to the flow_distributor, and get a corresponding async_flow class.
3. core/async_flow: is the basic abstraction for implementing the underlying asynchronous operations. It receives a packet and executes the actual flow processing logic. Finally, it outputs the packet. All the asynchronous functions can be performed by the async_flow structure.
4. core/shared_service: This is used as an option to shared state accessing in traditional network functions. This is an asynchronous service that has serialized accessibility for all the async_flows.

* To improve StatelessNF, we use mica as the underlying database implementation. Therefore, we provide interfaces for to access mica.
1. mica/mica_client

* netstar has a C++ based modern re-implementation of mos, this is available at
1. mos/*

* All the evaluation apps are available at
1. apps/stateless-nf/*
2. apps/ovs-nat/*
3. apps/abacus/*
4. apps/halfback/*

void receive(per_core_objs<port> ports, )
