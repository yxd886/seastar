#ifndef _AF_EVENT_HH
#define _AF_EVENT_HH

#include <cstdint>
#include <assert.h>

namespace netstar{

namespace internal{

using event_storage_type = uint32_t;

} // namespace internal

template<typename Enum>
class generated_events {
    using est = internal::event_storage_type;
    est _events;
public:
    generated_events(est events)
        : _events(events) {
    }

    bool empty(){
        return _events == 0;
    }

    template<Enum EvT> bool on_event() {
        static_assert((static_cast<uint8_t>(EvT) < sizeof(est)*8),
                      "event_type must be smaller than 64.\n");
        uint64_t mask = 1 << EvT;
        return (_events&mask) != 0;
    }
};

template<typename Enum>
class registered_events {
    using est = internal::event_storage_type;
    est _registered_events;
    est _generated_events;
public:
    registered_events()
        : _registered_events(0)
        , _generated_events(0) {
    }
public:
    // registration/unregistration pair
    template<Enum EvT> void register_event() {
        static_assert((static_cast<uint8_t>(EvT) < sizeof(est)*8),
                      "event_type must be smaller than 64.\n");
        uint64_t mask = 1 << EvT;

        // Use an assertion to prevent re-registering the
        // same type of events.
        assert((_registered_events & mask) == 0);
        _registered_events |= mask;
    }
    template<Enum EvT> void unregister_event() {
        static_assert((static_cast<uint8_t>(EvT) < sizeof(est)*8),
                      "event_type must be smaller than 64.\n");
        uint64_t mask = ~(1 << EvT);
        _registered_events &= mask;
    }
public:
    // generate an new event
    template<Enum EvT> void new_event(){
        static_assert((static_cast<uint8_t>(EvT) < sizeof(est)*8),
                      "event_type must be smaller than 64.\n");
        uint64_t mask = 1 << EvT;
        _generated_events |= mask;
    }
public:
    // generate registered, interested events, one shot
    // call, clear _generated_events after the call.
    generated_events<Enum> generate_events(){
        generated_events<Enum> e(_registered_events&_generated_events);
        _generated_events = 0;
        return e;
    }
};

} // netstar

#endif
