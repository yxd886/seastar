#ifndef _ASYNC_FLOW_IMPL_BASE_HH
#define _ASYNC_FLOW_IMPL_BASE_HH

#include "net/packet.hh"

#include "core/future.hh"

#include "netstar/af/async_flow_util.hh"

namespace netstar {

namespace internal {

using namespace seastar;

template<typename Ppr, typename Whatever>
struct async_flow_impl_base {
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;

    unsigned _pkts_in_pipeline; // records number of the packets injected into the pipeline.
    bool _initial_context_destroyed;

    af_work_unit<Ppr>& get_work_unit(bool is_client);
    void internal_packet_forward(net::packet pkt, bool is_client, bool is_send);
    void close_ppr_and_remove_flow_key(af_work_unit<Ppr>& work_unit);
    void send_packet_out(net::packet pkt, bool is_client);
    void handle_packet_recv(net::packet pkt, bool is_client);
    bool check_is_client(uint8_t direction);

    filtered_events<EventEnumType> preprocess_packet(
            af_work_unit<Ppr>& working_unit, net::packet& pkt, bool is_send) {
        generated_events<EventEnumType> ge =
                is_send ?
                working_unit.ppr.handle_packet_send(pkt) :
                working_unit.ppr.handle_packet_recv(pkt);
        filtered_events<EventEnumType> fe =
                is_send ?
                working_unit.send_events.filter(ge) :
                working_unit.recv_events.filter(ge);

        if(fe.on_close_event()) {
            async_flow_debug("async_flow_impl_base: Close preprocessor and remove flow key.\n");
            close_ppr_and_remove_flow_key(working_unit);
        }
        return fe;
    }

    void preprocess_and_forward(net::packet pkt, bool is_client, bool is_send) {
        auto& working_unit = get_work_unit(is_client);
        auto ge = is_send ? working_unit.ppr.handle_packet_send(pkt) :
                            working_unit.ppr.handle_packet_recv(pkt);
        if(ge.on_close_event()){
            async_flow_debug("async_flow_impl_base: Close preprocessor and remove flow key.\n");
            close_ppr_and_remove_flow_key(working_unit);
        }
        internal_packet_forward(std::move(pkt), is_client, is_send);
    }

    void action_after_packet_handle(af_work_unit<Ppr>& working_unit,
                                    net::packet pkt,
                                    bool is_client, bool is_send) {
        if(working_unit.loop_fn != nullptr) {
            if(!working_unit.cur_context) {
                async_flow_assert(working_unit.buffer_q.empty());
                auto fe = preprocess_packet(working_unit, pkt, is_send);
                if(fe.no_event()) {
                    internal_packet_forward(std::move(pkt), is_client, is_send);
                }
                else{
                    working_unit.cur_context.emplace(std::move(pkt), fe, is_send);
                    invoke_async_loop(working_unit, is_client);
                }
            }
            else{
                working_unit.buffer_q.emplace_back(std::move(pkt), is_send);
            }
        }
        else{
            preprocess_and_forward(std::move(pkt), is_client, is_send);
        }
    }

    // Critical functions for controlling the loop
    void close_async_loop(bool is_client) {
        auto& working_unit = get_work_unit(is_client);
        working_unit.loop_fn = nullptr;
        while(!working_unit.buffer_q.empty()) {
            auto& next_pkt = working_unit.buffer_q.front();
            preprocess_and_forward(std::move(next_pkt.pkt), is_client, next_pkt.is_send);
            working_unit.buffer_q.pop_front();
        }
        working_unit.async_loop_quit_pr->set_value();
        working_unit.async_loop_quit_pr = {};
    }

    void loop_fn_post_handler(bool is_client, af_action action) {
        auto& working_unit = get_work_unit(is_client);
        auto& context = working_unit.cur_context.value();

        if(action == af_action::forward || action == af_action::close_forward) {
            internal_packet_forward(std::move(context.pkt), is_client, context.is_send);
        }

        if(action == af_action::drop || action == af_action::close_drop) {
            _pkts_in_pipeline -= 1;
        }

        working_unit.cur_context = {};

        if(action == af_action::close_drop || action == af_action::close_forward) {
            close_async_loop(is_client);
            return;
        }

        while(!working_unit.buffer_q.empty()) {
            auto& next_pkt = working_unit.buffer_q.front();
            auto fe = preprocess_packet(working_unit,
                    next_pkt.pkt, next_pkt.is_send);
            if(fe.no_event()) {
                internal_packet_forward(std::move(next_pkt.pkt),
                                        is_client,
                                        next_pkt.is_send);
                working_unit.buffer_q.pop_front();
            }
            else{
                working_unit.cur_context.emplace(std::move(next_pkt.pkt),
                                                 fe,
                                                 next_pkt.is_send);
                working_unit.buffer_q.pop_front();
                invoke_async_loop(working_unit, is_client);
                return;
            }
        }

        if(working_unit.ppr_close == true) {
            _pkts_in_pipeline += 1;
            working_unit.cur_context.emplace(net::packet::make_null_packet(),
                                             filtered_events<EventEnumType>::make_close_event(),
                                             true);
            invoke_async_loop(working_unit, is_client);
        }
    }

    // invoke_async_loop can be as simple as this:
    // working_unit.loop_fn().then([this, is_client](af_action action){
    //    loop_fn_post_handler(is_client, action);
    // })
    // However, this will not handle exceptions thrown when executing
    // loop_fn. In order to catch the exception, we replace the
    // implementation with this.
    void invoke_async_loop(af_work_unit<Ppr>& working_unit, bool is_client) {
        working_unit.loop_fn().then_wrapped([this, is_client](auto&& f){
            try {
                auto action = f.get0();
                this->loop_fn_post_handler(is_client, action);
            }
            catch(...){
                this->_initial_context_destroyed = false;
                auto& working_unit = this->get_work_unit(is_client);
                working_unit.cur_context = {};
                working_unit.loop_fn = nullptr;
                while(!working_unit.buffer_q.empty()) {
                    working_unit.buffer_q.pop_front();
                }
                working_unit.async_loop_quit_pr->set_exception(std::current_exception());
                working_unit.async_loop_quit_pr = {};
            }
        });
    }

    // Interfaces used by a manager and the preprocessor.
    void destroy_initial_context() {
        _initial_context_destroyed = true;
    }

    void handle_packet_send(net::packet pkt, uint8_t direction) {
        async_flow_debug("async_flow_impl: handle_packet_send is called\n");

        bool is_client = check_is_client(direction);
        auto& working_unit = get_work_unit(is_client);

        if( _pkts_in_pipeline >= Ppr::async_flow_config::max_event_context_queue_size ||
             working_unit.ppr_close ||
             !_initial_context_destroyed) {
            // Unconditionally drop the packet.
            return;
        }

        _pkts_in_pipeline += 1;

        action_after_packet_handle(working_unit, std::move(pkt),
                                   is_client, true);
    }

    void ppr_passive_close(bool is_client){
        auto& working_unit = get_work_unit(is_client);
        close_ppr_and_remove_flow_key(working_unit);

        if(working_unit.loop_fn != nullptr && !working_unit.cur_context) {
            _pkts_in_pipeline += 1;
            working_unit.cur_context.emplace(net::packet::make_null_packet(),
                                             filtered_events<EventEnumType>::make_close_event(),
                                             true);
            invoke_async_loop(working_unit, is_client);
        }
    }

    // Async loop initialization sequences after acquring the
    // initial packet context.
    void event_registration (bool is_client, bool is_send, EventEnumType ev) {
        auto& working_unit = get_work_unit(is_client);
        auto& events = is_send ? working_unit.send_events : working_unit.recv_events;
        events.register_event(ev);
    }

    void event_unregistration (bool is_client, bool is_send, EventEnumType ev) {
        auto& working_unit = get_work_unit(is_client);
        auto& events = is_send ? working_unit.send_events : working_unit.recv_events;
        events.unregister_event(ev);
    }

    future<> run_async_loop(bool is_client, std::function<future<af_action>()> fn) {
        auto& working_unit = get_work_unit(is_client);

        async_flow_assert(working_unit.loop_fn==nullptr && !working_unit.cur_context && !working_unit.async_loop_quit_pr);

        working_unit.loop_fn = std::move(fn);
        working_unit.async_loop_quit_pr = promise<>();
        return working_unit.async_loop_quit_pr->get_future();
    }
};

} // namespace internal

} // namespace netstar

#endif
