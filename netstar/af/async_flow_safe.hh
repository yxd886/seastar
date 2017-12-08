#ifndef _ASYNC_FLOW_SAFE_HH
#define _ASYNC_FLOW_SAFE_HH

#include "core/gate.hh"
#include "core/future.hh"

#include "netstar/af/async_flow.hh"

using namespace seastar;

namespace netstar {

template<typename Ppr>
class async_flow_safe {
    using EventEnumType = typename Ppr::EventEnumType;

    lw_shared_ptr<client_async_flow<Ppr>> _client;
    lw_shared_ptr<server_async_flow<Ppr>> _server;
    lw_shared_ptr<gate> _g;
public:
    async_flow_safe(client_async_flow<Ppr>&& client,
                    server_async_flow<Ppr>&& server)
        : _client(make_lw_shared(std::move(client)))
        , _server(make_lw_shared(std::move(server)))
        , _g(make_lw_shared(gate())){
    }

    void register_client_events(af_send_recv sr, EventEnumType ev) {
        _client->register_events(sr, ev);
    }

    void unregister_client_events(af_send_recv sr, EventEnumType ev) {
        _client->unregister_events(sr, ev);
    }

    void register_server_events(af_send_recv sr, EventEnumType ev) {
        _server->register_events(sr, ev);
    }

    void unregister_server_events(af_send_recv sr, EventEnumType ev) {
        _server->unregister_events(sr, ev);
    }

    template<typename LoopFunc>
    void run_client_async_loop(LoopFunc&& func) {
        using futurator = futurize<std::result_of_t<LoopFunc(client_async_flow<Ppr>&)>>;
        static_assert(std::is_same<typename futurator::type, future<af_action>>::value, "bad_signature");

        std::function<future<af_action>()> loop_fn = [client = _client.get(), func=std::forward<LoopFunc>(func)](){
            using futurator = futurize<std::result_of_t<LoopFunc(client_async_flow<Ppr>&)>>;
            return  futurator::apply(func, (*client));
        };

        _g->enter();
        _client->run_async_loop(std::move(loop_fn)).then([client = _client, g = _g](){
            g->leave();
        });
    }

    template<typename LoopFunc>
    void run_server_async_loop(LoopFunc&& func) {
        using futurator = futurize<std::result_of_t<LoopFunc(server_async_flow<Ppr>&)>>;
        static_assert(std::is_same<typename futurator::type, future<af_action>>::value, "bad_signature");

        std::function<future<af_action>()> loop_fn = [server = _server.get(), func=std::forward<LoopFunc>(func)](){
            using futurator = futurize<std::result_of_t<LoopFunc(server_async_flow<Ppr>&)>>;
            return  futurator::apply(func, (*server));
        };

        _g->enter();
        _server->run_async_loop(std::move(loop_fn)).then([server = _server, g = _g](){
            g->leave();
        });
    }
};

}


#endif // _ASYNC_FLOW_SAFE_HH
