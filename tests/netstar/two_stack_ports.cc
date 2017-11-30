/*
 * Set up two stack ports.
 * We can ping these two stack ports from another server.
 */

#include "netstar/port.hh"
#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/print.hh"
#include "core/distributed.hh"

#include "netstar/per_core_objs.hh"
#include "netstar/mica_client.hh"
#include "netstar/extendable_buffer.hh"
#include "netstar/stack_port.hh"
#include "netstar/port_env.hh"

using namespace seastar;
using namespace netstar;

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;

    return app.run_deprecated(ac, av, [&app, &all_ports]{
        auto& opts = app.configuration();

        std::unordered_map<std::string, net::ipv4_address> p0_addr_map;
        p0_addr_map["host-ipv4-addr"] = net::ipv4_address("10.28.1.13");
        p0_addr_map["gw-ipv4-addr"] = net::ipv4_address("10.28.1.1");
        p0_addr_map["netmask-ipv4-addr"] = net::ipv4_address("255.255.255.255");

        return all_ports.add_stack_port(opts, 0, smp::count, std::move(p0_addr_map)).then([&opts, &all_ports]{
            std::unordered_map<std::string, net::ipv4_address> p1_addr_map;
            p1_addr_map["host-ipv4-addr"] = net::ipv4_address("10.29.1.13");
            p1_addr_map["gw-ipv4-addr"] = net::ipv4_address("10.29.1.1");
            p1_addr_map["netmask-ipv4-addr"] = net::ipv4_address("255.255.255.255");

            return all_ports.add_stack_port(opts, 1, smp::count, std::move(p1_addr_map));
        }).then_wrapped([](auto&& f){
            try{
                f.get();
                printf("Finish creating two stack ports\n");
            }
            catch(...){
                printf("Error creating two stack ports\n");
            }
        });
    });
}
