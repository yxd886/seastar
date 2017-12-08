#ifndef _ASYNC_FLOW_SAFE_HH
#define _ASYNC_FLOW_SAFE_HH

#include "core/gate.hh"

#include "netstar/af/async_flow.hh"

using namespace seastar;

namespace netstar {

template<typename Ppr>
class async_flow_safe {
    lw_shared_ptr<client_async_flow<Ppr>> _client;
    lw_shared_ptr<server_async_flow<Ppr>> _server;
    lw_shared_ptr<gate> _g;

    async_flow_safe(client_async_flow<Ppr>&& client,
                    server_async_flow<Ppr>&& server)
        : _client(make_lw_shared(std::move(client)))
        , _server(make_lw_shared(std::move(server)))
        , _g(make_lw_shared(gate())){
    }


};

}


#endif // _ASYNC_FLOW_SAFE_HH
