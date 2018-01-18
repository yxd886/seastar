#ifndef _DUMMY_DEVICE_HH
#define _DUMMY_DEVICE_HH

#include "net/net.hh"

#include "core/future.hh"

#include "netstar/port.hh"

namespace netstar {

namespace internal {

class dummy_qp : public seastar::net::qp {
private:
    port& _port;

public:
    explicit dummy_qp(port* pt)
        : _port(*pt){}

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
        return std::make_unique<dummy_qp>(nullptr);
    }

    virtual unsigned hash2qid(uint32_t hash) override {
        return _concrete_dev->hash2qid(hash);
    }

    virtual unsigned hash2cpu(uint32_t hash) override {
        return _concrete_dev->hash2cpu(hash);
    }
};


} // namespace internal

} // namespace netstar

#endif // _DUMMY_DEVICE_HH
