#ifndef _ASYNC_FLOW_UTIL_HH
#define _ASYNC_FLOW_UTIL_HH

#include <cstdint>
#include <assert.h>

namespace netstar{

#define ENABLE_ASSERTION
#define ASYNC_FLOW_DEBUG

void async_flow_assert(bool boolean_expr) {
#ifdef ENABLE_ASSERTION
    assert(boolean_expr);
#endif
}

template <typename... Args>
void async_flow_debug(const char* fmt, Args&&... args) {
#ifdef ASYNC_FLOW_DEBUG
    print(fmt, std::forward<Args>(args)...);
#endif
}

enum class af_side : bool {
    client=true,
    server=false
};

enum class af_send_recv : bool {
    send=true,
    recv=false
};

namespace internal {

using event_storage_type = uint16_t;

template<typename Enum>
class filtered_events;
template<typename Enum>
class generated_events;
template<typename Enum>
class registered_events;

template<typename Enum>
class filtered_events {
    friend class registered_events<Enum>;
    using est = event_storage_type;
    est _filtered_events;
public:
    filtered_events(est fe)
        : _filtered_events(fe){
    }
public:
    bool on_event(Enum ev) const {
        assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = 1 << static_cast<est>(ev);
        return (_filtered_events&mask) != 0;
    }

    bool on_close_event() const {
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        return (_filtered_events&mask) != 0;
    }

    bool no_event() const {
        return _filtered_events == 0;
    }

    static filtered_events make_close_event() {
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        return filtered_events(mask);
    }
};

template<typename Enum>
class generated_events {
    friend class registered_events<Enum>;
    using est = event_storage_type;
    est _generated_events;
public:
    generated_events() {
        _generated_events = 0;
    }

public:
    void event_happen(Enum ev){
        assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = 1 << static_cast<est>(ev);
        _generated_events |= mask;
    }

    void close_event_happen(){
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        _generated_events |= mask;
    }

    void clear(){
        _generated_events = 0;
    }
};

template<typename Enum>
class registered_events {
    using est = event_storage_type;
    est _registered_events;
public:
    registered_events() {
        unregister_all_events();
    }
public:
    void register_event(Enum ev) {
        assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = 1 << static_cast<est>(ev);
        _registered_events |= mask;
    }

    void unregister_event(Enum ev) {
        assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = ~(1 << static_cast<est>(ev));
        _registered_events &= mask;
    }

    void unregister_all_events(){
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        _registered_events = mask;
    }

    filtered_events<Enum> filter(generated_events<Enum> ge){
        return filtered_events<Enum>(_registered_events & ge._generated_events);
    }
};

} // namespace internal

} // namespace netstar

#endif
