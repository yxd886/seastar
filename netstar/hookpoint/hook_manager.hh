#ifndef _HOOK_MANAGER_HH
#define _HOOK_MANAGER_HH

#include "netstar/hookpoint/dummy_hook.hh"

namespace netstar{

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

} // namespace netstar

#endif // _HOOK_MANAGER_HH
