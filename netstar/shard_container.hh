#ifndef _SHARD_CONTAINER_HH
#define _SHARD_CONTAINER_HH

#include <memory>

#include "core/future.hh"
#include "core/reactor.hh"

namespace netstar {

namespace internal {

template<typename T>
struct shard_container {
    T* t;

    template <typename... Args>
    shard_container(Args&&... args) {
        auto obj = std::make_unique<T>(std::forward<Args>(args)...);
        t = obj.get();
        seastar::engine().at_destroy([obj = std::move(obj)] {});
    }

    seastar::future<> stop() {
        return seastar::make_ready_future<>();
    }

    T& get_contained() {
        return *t;
    }
};

} // namespace internal

} // namespace netstar

#endif
