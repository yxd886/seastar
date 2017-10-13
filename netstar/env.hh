#ifndef _ENV_HH
#define _ENV_HH

#include <vector>
#include <experimental/optional>
#include "core/reactor.hh"
#include "core/apply.hh"
#include "core/do_with.hh"
#include <boost/iterator/counting_iterator.hpp>

using namespace seastar;
using std::vector;

namespace netstar{

class no_per_core_obj : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "per-core object does not exist";
    }
};

template<class T>
class per_core{
    vector<T*> _reactor_saved_objects;
public:
    // default constructor and deconstructors
    explicit per_core(){}
    ~per_core(){}

    // move/copy constructor/assignment are all deleted
    per_core(const per_core& other) = delete;
    per_core(per_core&& other)  = delete;
    per_core& operator=(const per_core& other) = delete;
    per_core& operator=(per_core&& other) = delete;

    template <typename... Args>
    future<> start(Args&&... args){
        _reactor_saved_objects.resize(smp::count);
        for(auto& obj : _reactor_saved_objects){
            obj = nullptr;
        }
        return parallel_for_each(boost::irange<unsigned>(0, _reactor_saved_objects.size()),
            [this, args = std::make_tuple(std::forward<Args>(args)...)] (unsigned c) mutable {
                return smp::submit_to(c, [this, args] () mutable {
                    apply([this] (Args... args) {
                        init_reactor_saved_object(std::forward<Args>(args)...);
                    } ,args);
                });
        }).then_wrapped([this] (future<> f) {
            try {
                f.get();
                return make_ready_future<>();
            } catch (...) {
                return this->stop().then([e = std::current_exception()] () mutable {
                    std::rethrow_exception(e);
                });
            }
        });
    }

    template <typename... Args>
    future<> start_on(unsigned core_id, Args&&... args){
        _reactor_saved_objects.resize(smp::count);
        for(auto& obj : _reactor_saved_objects){
            obj = nullptr;
        }

        return smp::submit_to(c, [this, args = std::make_tuple(std::forward<Args>(args)...)] () mutable {
            apply([this] (Args... args) {
                init_reactor_saved_object(std::forward<Args>(args)...);
            } ,args);
        }).then_wrapped([this] (future<> f) {
            try {
                f.get();
                return make_ready_future<>();
            } catch (...) {
                return this->stop().then([e = std::current_exception()] () mutable {
                    std::rethrow_exception(e);
                });
            }
        });
    }

    future<> stop() {
        return parallel_for_each(boost::irange<unsigned>(0, _reactor_saved_objects.size()), [this] (unsigned c) mutable {
            return smp::submit_to(c, [this] () mutable {
                auto local_obj = _reactor_saved_objects[engine().cpu_id()];
                if(!local_obj){
                    return make_ready_future<>();
                }
                return local_obj->stop();
            });
        });
    }

    template <typename Func>
    future<> invoke_on_all(Func&& func) {
        static_assert(std::is_same<futurize_t<std::result_of_t<Func(T&)>>, future<>>::value,
                      "invoke_on_all()'s func must return void or future<>");
        return parallel_for_each(boost::irange<unsigned>(0, _reactor_saved_objects.size()), [this, &func] (unsigned c) {
            return smp::submit_to(c, [this, func] {
                auto local_obj = this->get_obj(engine().cpu_id());
                return func(*local_obj);
            });
        });
    }

    template<typename... Args>
    future<> invoke_on_all(future<> (T::*func)(Args...), Args... args){
        return parallel_for_each(boost::irange<unsigned>(0, _reactor_saved_objects.size()), [this, func, args...](unsigned c){
            return smp::submit_to(c, [this, func, args...]{
                auto local_obj = this->get_obj(engine().cpu_id());
                return ((*local_obj).*func)(args...);
            });
        });
    }

    template<typename... Args>
    future<> invoke_on_all(void (T::*func)(Args...), Args... args){
        return parallel_for_each(boost::irange<unsigned>(0, _reactor_saved_objects.size()), [this, func, args...](unsigned c){
            return smp::submit_to(c, [this, func, args...]{
                auto local_obj = this->get_obj(engine().cpu_id());
                return ((*local_obj).*func)(args...);
            });
        });
    }

    template <typename Func>
    future<> invoke_on(unsigned core, Func&& func) {
        static_assert(std::is_same<futurize_t<std::result_of_t<Func(T&)>>, future<>>::value,
                      "invoke_on_all()'s func must return void or future<>");
        return smp::submit_to(core, [this, func] {
            auto local_obj = this->get_obj(engine().cpu_id());
            return func(*local_obj);
        });
    }

    template <typename Ret, typename... FuncArgs, typename... Args, typename FutureRet = futurize_t<Ret>>
    FutureRet
    invoke_on(unsigned id, Ret (T::*func)(FuncArgs...), Args&&... args) {
        using futurator = futurize<Ret>;
        return smp::submit_to(id, [this, func, args = std::make_tuple(std::forward<Args>(args)...)] () mutable {
            auto local_obj = this->get_obj(engine().cpu_id());
            return futurator::apply(std::mem_fn(func), std::tuple_cat(std::make_tuple<>(local_obj), std::move(args)));
        });
    }

    T* get_obj(unsigned core_id){
        auto ret = _reactor_saved_objects[core_id];
        if(!ret){
            throw no_per_core_obj();
        }
        return ret;
    }

private:
    template<typename... Args>
    void init_reactor_saved_object(Args&&... args){
        std::unique_ptr<T> obj = std::make_unique<T>(std::forward<Args>(args)...);
        _reactor_saved_objects[engine().cpu_id()] = obj.get();
        engine().at_destroy([obj = std::move(obj)] {});
    }
};

} // namespace netstar

#endif
