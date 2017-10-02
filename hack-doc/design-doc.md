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