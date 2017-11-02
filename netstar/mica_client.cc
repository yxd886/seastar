#include "mica_client.hh"

namespace netstar{

namespace queue_mapping {

vector<vector<port_pair>>
calculate_queue_mapping(boost::program_options::variables_map& opts,
                        unsigned local_smp_count, unsigned remote_smp_count,
                        std::string local_ip_addr_str,
                        std::string remote_ip_addr_str,
                        port& pt){
    // res[x][y].local_port: local port that maps local queue x to remote queue y
    // res[x][y].remote_port: remote port that maps local queue x to remote queue y
    vector<vector<port_pair>> res;
    res.resize(local_smp_count);
    for(auto& v : res){
        v.resize(remote_smp_count);
    }

    // record whether a position in res is used
    vector<vector<bool>> res_pos_flag;
    res_pos_flag.resize(local_smp_count);
    for(auto& v : res_pos_flag){
        v.resize(remote_smp_count, false);
    }

    unsigned total = local_smp_count * remote_smp_count;
    net::ipv4_address local_ip_addr(local_ip_addr_str);
    net::ipv4_address remote_ip_addr(remote_ip_addr_str);

    // manually build a redirection table for remote side.
    // We assume that the size of the redirection table is 512
    vector<uint8_t> remote_redir_table(512);
    unsigned i = 0;
    for (auto& r : remote_redir_table) {
        r = i++ % remote_smp_count;
    }

    for(uint16_t local_port = 10240; local_port < 65535; local_port ++){
        for(uint16_t remote_port = 10240; remote_port < 65535; remote_port ++){
            // iterate through each of the local and remote port;
            net::l4connid<net::ipv4_traits> to_local{local_ip_addr, remote_ip_addr, local_port, remote_port};
            net::l4connid<net::ipv4_traits> to_remote{remote_ip_addr, local_ip_addr, remote_port, local_port};

            unsigned local_queue = pt.hash2cpu(to_local.hash(pt.get_rss_key()));
            unsigned remote_queue = remote_redir_table[
                                                       to_remote.hash(pt.get_rss_key()) &
                                                       (remote_redir_table.size() - 1)
                                                       ];

            if(!res_pos_flag[local_queue][remote_queue]){
                res_pos_flag[local_queue][remote_queue] = true;
                total--;
                res[local_queue][remote_queue].local_port = local_port;
                res[local_queue][remote_queue].remote_port = remote_port;

                printf("Find one valid queue mapping entry: local_queue %d <-> remote_queue %d, \\"
                       "local_port %d, remote_port %d\n", local_queue, remote_queue, local_port, remote_port);
            }

            if(total == 0){
                return res;
            }
        }
    }

    if(total!=0){
        printf("Fail to find a valid queue mapping. \n");
        assert(false);
    }

    return res;
}

vector<vector<port_pair>>& get_queue_mapping(){
    static vector<vector<port_pair>> qm;
    return qm;
}

future<> initialize_queue_mapping(boost::program_options::variables_map& opts,
                              port& pt){

    auto result = calculate_queue_mapping(
                        opts,
                        smp::count,
                        static_cast<unsigned>(opts["mica-sever-smp-count"].as<uint16_t>()),
                        opts["mica-client-ip"].as<std::string>(),
                        opts["mica-server-ip"].as<std::string>(),
                        pt);
    get_queue_mapping() = std::move(result);

    return make_ready_future<>();
}

} // namespace queue_mapping

net::packet mica_client::request_assembler::build_requet_batch_header(){
    net::packet pkt;

    // reserved0
    pkt.prepend_header<uint32_t>();

    // num_requests
    pkt.prepend_header<uint8_t>();

    // magic number
    auto magic_hdr = pkt.prepend_header<uint8_t>();
    *magic_hdr = 0x78;

    // udp header
    auto uhdr = pkt.prepend_header<net::udp_hdr>();
    uhdr->src_port = _local_ei.udp_port;
    uhdr->dst_port = _remote_ei.udp_port;
    uhdr->len = 0;
    uhdr->cksum = 0;
    *uhdr = net::hton(*uhdr);

    // ip header
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
    iph->src_ip = _local_ei.ip_addr;
    iph->dst_ip = _remote_ei.ip_addr;
    *iph = net::hton(*iph);

    // ethernet header
    auto eh = pkt.prepend_header<net::eth_hdr>();
    net::ethernet_address eth_src = _local_ei.eth_addr;
    net::ethernet_address eth_dst = _remote_ei.eth_addr;
    eh->dst_mac = eth_dst;
    eh->src_mac = eth_src;
    eh->eth_proto = uint16_t(net::eth_protocol_num::ipv4);
    *eh = net::hton(*eh);

    // set up the offload information here
    // I don't whether I really need this
    net::offload_info oi;
    oi.needs_csum = false;
    oi.protocol = net::ip_protocol_num::udp;
    pkt.set_offload_info(oi);

    pkt.linearize();
    assert(pkt.nr_frags() == 1);
    return pkt;
}

bool mica_client::is_valid(net::packet& p){
    auto eth_hdr = p.get_header<net::eth_hdr>();
    auto ip_hdr = p.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
    auto udp_hdr = p.get_header<net::udp_hdr>(sizeof(net::eth_hdr) + sizeof(net::ip_hdr));
    auto rbh = p.get_header<RequestBatchHeader>();

    if (p.len() < sizeof(RequestBatchHeader)) {
        // printf("too short packet length\n");
        return false;
    }

    if (net::ntoh(eth_hdr->eth_proto) != uint16_t(net::eth_protocol_num::ipv4)) {
        // printf("invalid network layer protocol\n");
        return false;
    }

    if (ip_hdr->ihl != 5 && ip_hdr->ver != 4) {
        // printf("invalid IP layer protocol\n");
        return false;
    }

    if (ip_hdr->id != 0 || ip_hdr->frag != 0) {
        // printf("ignoring fragmented IP packet\n");
        return false;
    }


    if (net::ntoh(ip_hdr->len) != p.len() - sizeof(net::eth_hdr)) {
        // printf("invalid IP packet length\n");
        return false;
    }

    if (ip_hdr->ip_proto != (uint8_t)net::ip_protocol_num::udp) {
        // printf("invalid transport layer protocol\n");
        return false;
    }

    if (net::ntoh(udp_hdr->len) != p.len() - sizeof(net::eth_hdr) - sizeof(net::ip_hdr)) {
        // printf("invalid UDP datagram length\n");
        return false;
    }

    if(udp_hdr->cksum != 0 || ip_hdr->csum != 0){
        // printf("We only accept packet with zero checksums\n");
        return false;
    }

    if (rbh->magic != 0x78 && rbh->magic != 0x79) {
        printf("invalid magic\n");
        return false;
    }

    return true;
}

bool mica_client::is_response(net::packet& p) const {
    auto rbh = p.get_header<RequestBatchHeader>();
    return rbh->magic == 0x79;
}

} // namespace nestar
