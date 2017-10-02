# About the implementation of qp and device class

* At the bottom level, seastar provides two virtual classes, which are qp and device. seastar uses these two classes to poll packets from the NIC.

* For netstar, we primary need the qp and device to provide three basic functionalities.
1. Process raw network packets. Currently, I want to keep the original seastar design as much as possible. So we will use the packet structure provided by seastar. If the performance is not satisfactory, we might switch to a customized packet structure.
2. Deliver the packet to a tcp/ip stack. I plan to integrate this feature together with the first feature into the same class.
3. Interface with mica. This is tricky, as mica uses intel fdir functionality. My current design choice is to separate it into a different implementation other than the first two functionalities.

* Actually, how dpdk_qp uses rte_mbuf depends on the template variable. If the template variable is set to true, then dpdk_qp will share the huge page tlb dir, and the packet object is simply a warper to the rte_mbuf's internal buffer. Otherwise, dpdk_qp will allocate a buffer using the internal memory allocator and discards the rte_mbuf. This discovery basically leads to the next design decision.

* We will directly use dpdk_qp. But we will create a new class called netstar_dpdk_device, to demonstrate its use for netstar applications.
1. netstar_dpdk_device should use the default seastar tcp/ip stack in a different manner, I'm still thinking about a good design decision. My initial design decision is to add an intermediate layer that acts like a distributor. If the header information match the tcp/ip stack, process the packet using the tcp/ip stack. Otherwise, process the packet using the nf processing pipeline.
2. The client interface with mica should be built as an application on top of the nf processing pipeline.

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
3. apps/load-balancer/*
4. apps/abacus/*
5. apps/halfback/*
