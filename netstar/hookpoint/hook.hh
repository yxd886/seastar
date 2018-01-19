#ifndef _HOOK_HH
#define _HOOK_HH

#include "netstar/port_manager.hh"
#include "netstar/stack/stack_manager.hh"

namespace netstar{

enum class hook_type {
    dummy
};

class hook {
protected:
    port* _p;
    std::function<seastar::future<> (rte_packet)> _recv_func;
    std::experimental::optional<seastar::subscription<rte_packet>> _sub;

    void start_receving() {
        // Move or copy?
        _sub.emplace(_p->receive(_recv_func));
    }

public:
    explicit hook(port* p)
        : _p(p)
        , _recv_func(nullptr) {}

    virtual ~hook() {}

    // Each hook point has a default receive function to receive rte_packet
    // from the port. This interface method can be used to replace this receive
    // function.
    virtual void update_port_recv_func(std::function<seastar::future<> (rte_packet)> new_func) = 0;

    // Check whether the hook point has been correctly configured.
    // If so, the hook point is started by calling start_receving() and return true.
    // If not, the hook point is not started and return false.
    virtual bool check_and_start() = 0;
};

namespace internal {

template<typename T>
struct hook_point_launcher {
    static_assert(std::is_base_of<hook, T>::value, "The type parameter is not derived from hook.\n");
    using hook_shard = shard_container_trait<T>;

    template <typename... Args>
    static seastar::future<std::vector<hook*>> launch(Args&&... args) {
        auto vec = std::make_shared<std::vector<T*>>(seastar::smp::count, nullptr);
        auto shard_sptr = std::make_shared<hook_shard::shard_t>();

        return shard_sptr->start(std::forward<Args>(args)...).then([shard_sptr, vec]{
            return shard_sptr->invoke_on_all(&hook_shard::instance_t::save_container_ptr,
                                             vec.get());
        }).then([shard_sptr]{
            return shard_sptr->stop();
        }).then([shard_sptr, vec]{
            std::vector<hook*> ret;
            for(auto derived_ptr : *vec) {
                ret.push_back(derived_ptr);
            }
            return std::move(ret);
        });
    }
};


} // namespace internal

class hook_manager {
    std::vector<std::vector<hook*>> _hooks;
    std::vector<seastar::promise<>> _prs;
    std::vector<unsigned> _port_ids;

public:
    seastar::future<> add_hook_point(hook_type type, unsigned port_id) {
        unsigned which_one = _prs.size();
        _prs.emplace_back();
        _port_ids.push_back(port_id);

        switch(type) {
        case hook_type::dummy: {
            internal::hook_point_launcher<internal::dummy_hook>::launch(port_id).then(
                    [this, which_one](auto&& vec){
                _hooks.push_back(std::move(vec));
                _prs.at(which_one).set_value();
            });
            break;
        }
        default:
            break;
        }

        return _prs.at(which_one).get_future();
    }
};

}

#endif
