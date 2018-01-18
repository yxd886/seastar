#ifndef _DUMMY_DEVICE_HH
#define _DUMMY_DEVICE_HH

#include "net/net.hh"
#include "net/native-stack.hh"

#include "core/future.hh"

#include "netstar/port.hh"

namespace netstar {

namespace internal {

class dummy_qp : public seastar::net::qp {
private:
    port& _port;

public:
    explicit dummy_qp(port* pt, unsigned qid)
        : seastar::net::qp(false, std::string("dummynet_dev_")+std::to_string(pt->get_dev_id()), qid)
        , _port(*pt){}

    virtual seastar::future<> send(seastar::net::packet p) override {
        abort();
    }

    virtual uint32_t send(seastar::circular_buffer<seastar::net::packet>& p) override {
        auto size = p.size();
        while(!p.empty()) {
            _port.send_seastar_packet(std::move(p.front()));
            p.pop_front();
        }
        return size;
    }
};

class dummy_device : public seastar::net::device {
private:
    seastar::net::device* _concrete_dev;

public:
    dummy_device(seastar::net::device* concrete_dev)
        : _concrete_dev(concrete_dev) {}

    virtual seastar::net::ethernet_address hw_address() override {
        return _concrete_dev->hw_address();
    }

    virtual seastar::net::hw_features hw_features() override {
        return _concrete_dev->hw_features();
    }

    virtual const seastar::rss_key_type& rss_key() const override {
        return _concrete_dev->rss_key();
    }

    virtual uint16_t hw_queues_count() override {
        return _concrete_dev->hw_queues_count();
    }

    virtual std::unique_ptr<seastar::net::qp>
    init_local_queue(boost::program_options::variables_map opts, uint16_t qid) override {
        abort();
        return std::make_unique<dummy_qp>(nullptr, 0);
    }

    virtual unsigned hash2qid(uint32_t hash) override {
        return _concrete_dev->hash2qid(hash);
    }

    virtual unsigned hash2cpu(uint32_t hash) override {
        return _concrete_dev->hash2cpu(hash);
    }
};

class multi_stack {
    dummy_qp _qp;
    std::unique_ptr<seastar::net::native_network_stack> _stack_ptr;

public:
    explicit multi_stack(std::shared_ptr<seastar::net::device> dummy_dev, port* p,
                         std::string ipv4_addr, std::string gw_addr, std::string netmask)
        : _qp(p, seastar::engine().cpu_id()) {
        dummy_dev->update_local_queue(&_qp);
        _stack_ptr = std::make_unique<seastar::net::native_network_stack>(
                std::move(dummy_dev), ipv4_addr, gw_addr, netmask);
    }

    seastar::future<> stop() {
        return seastar::make_ready_future();
    }

    seastar::net::network_stack* get_stack() {
        return _stack_ptr.get();
    }
};

} // namespace internal

} // namespace netstar

#endif // _DUMMY_DEVICE_HH
