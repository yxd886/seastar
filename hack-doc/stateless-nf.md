# Some notes on re-implementing stateless-nf.

* The basic starting point is mica2. We use mica2 as the backend database.

* The NF program should contain one thread for accessing the mica2 server and several worker threads for processing the network packets.

* The worker threads should properly setup RSS to distribute input flows to different worker threads.

* The worker threads and the mica2-client thread should share a lockless queue for communication. (we can use the lockless queue implementation provided by DPDK.)
