#include <array>
#include <forward_list>
#include <functional>
#include <iostream>
#include <type_traits>
#include <memory>

#ifndef TYPESAFEEVENTEMITTER_EVENTEMITTER_HPP
#define TYPESAFEEVENTEMITTER_EVENTEMITTER_HPP


// This is the event emitter class.
template<typename EventsType>
struct EventEmitter {
private:
    // We use the typename... here to delay type resolution, since
    // during compilation this class does not know about the real type of EventsType.
    static_assert(std::is_enum_v<typename EventsType::EVENTS>, "EventsType must have an enum called EVENTS.");
    using EVENTS = typename EventsType::EVENTS;

    // This provides the uniform base class so that EventEmitter can store instances in an array.
    struct HandlerStorageBase {
        virtual ~HandlerStorageBase() = default;
    };

    // This HandlerStorage is actually EventEmitter<EventsType>::HandlerStorage. IE, it's a different class for each
    // EventEmitter type, and holds function pointers to the handlers to invoke.

    template<EVENTS T>
    struct HandlerStorage : public HandlerStorageBase {
        using EventArgsType = typename EventsType::template Event<T>::Args;
        using HandlerType = std::function<void(const EventArgsType &)>;

        HandlerStorage(HandlerType handler) : handler_(handler) {}

        ~HandlerStorage() override = default;

        HandlerType handler_;
    };

public:
    EventEmitter() = default;

    template<EVENTS T>
    void Subscribe(const typename HandlerStorage<T>::HandlerType handler) {
        handlers_[static_cast<std::size_t>(T)] =
                std::make_unique<HandlerStorage<T>>(handler);
    }

    // We already know the real type here, so handle the casting and invoke the stored function pointer.
    template<EVENTS T>
    void Emit(const typename HandlerStorage<T>::EventArgsType &args) {
        reinterpret_cast<HandlerStorage<T> *>(handlers_[static_cast<std::size_t>(T)].get())->handler_(args);
    }

    std::array<std::unique_ptr<HandlerStorageBase>,
               static_cast<std::size_t>(EVENTS::NUM_EVENT_TYPES)>
            handlers_;
};

#endif//TYPESAFEEVENTEMITTER_EVENTEMITTER_HPP
