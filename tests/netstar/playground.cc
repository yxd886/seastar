
#include "netstar/device/standard_device.hh"
#include "netstar/rte_packet.hh"
#include "netstar/port_manager.hh"

class wtf {
    int _i;
    explicit wtf(int i) : _i(i) {}
};

int main(){
    netstar::rte_packet pkt();
    netstar::port_manager::get();
    wtf v(1);
    wtf v1 = v;
    return 1;
}
