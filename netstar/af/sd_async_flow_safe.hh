#ifndef _SD_ASYNC_FLOW_SAFE_HH
#define _SD_ASYNC_FLOW_SAFE_HH

#include "core/gate.hh"
#include "core/future.hh"

#include "netstar/af/sd_async_flow.hh"

using namespace seastar;

namespace netstar {

template<typename Ppr>
class sd_async_flow_safe {
    using EventEnumType = typename Ppr::EventEnumType;

    lw_shared_ptr<sd_async_flow<Ppr>> _client;
    lw_shared_ptr<gate> _g;
public:
    sd_async_flow_safe(sd_async_flow<Ppr>&& client)
        : _client(make_lw_shared(std::move(client)))
        , _g(make_lw_shared(gate())){
    }

    void register_events(EventEnumType ev) {
        _client->register_events(ev);
    }

    void unregister_events(EventEnumType ev) {
        _client->unregister_events(ev);
    }

    template<typename LoopFunc>
    void run_async_loop(LoopFunc&& func) {
        using futurator = futurize<std::result_of_t<LoopFunc(sd_async_flow<Ppr>&)>>;
        static_assert(std::is_same<typename futurator::type, future<af_action>>::value, "bad_signature");

        std::function<future<af_action>()> loop_fn = [client = _client.get(), func=std::forward<LoopFunc>(func)](){
            using futurator = futurize<std::result_of_t<LoopFunc(sd_async_flow<Ppr>&)>>;
            return  futurator::apply(func, (*client));
        };

        _g->enter();
        _client->run_async_loop(std::move(loop_fn)).then([client = _client, g = _g](){
            g->leave();
        });
    }

    future<> on_quit() {
        return _g->close();
    }
};

}


#endif // _ASYNC_FLOW_SAFE_HH
