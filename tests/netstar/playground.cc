
#include "netstar/device/standard_device.hh"
#include "netstar/rte_packet.hh"
#include "netstar/port_manager.hh"

int main(){
    netstar::rte_packet pkt();
    auto wtf = netstar::port_manager::get();
    return 1;
}
