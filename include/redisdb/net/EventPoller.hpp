#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace redisdb::net {

// Interest masks describe requested notifications.
//
// Error and Hangup are output-only notification flags.
// They must not be supplied to add() or modify().
enum class EventMask : std::uint8_t {
    None = 0,
    Read = 1U << 0U,
    Write = 1U << 1U,
    Error = 1U << 2U,
    Hangup = 1U << 3U
};

[[nodiscard]] constexpr EventMask operator|(EventMask lhs, EventMask rhs) noexcept {
    return static_cast<EventMask>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr EventMask operator&(EventMask lhs, EventMask rhs) noexcept {
    return static_cast<EventMask>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

constexpr EventMask& operator|=(EventMask& lhs, EventMask rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

[[nodiscard]] constexpr bool contains_all(EventMask value, EventMask flags) noexcept {
    return (value & flags) == flags;
}

[[nodiscard]] constexpr bool valid_interest(EventMask interest) noexcept {
    return interest == EventMask::Read || interest == EventMask::Write ||
           interest == (EventMask::Read | EventMask::Write);
}

// 0 is reserved as an invalid registration token.
//
// A caller must assign a fresh nonzero token for every
// registration lifetime. A removed token must not be
// reused for a later registration, even if the OS
// reuses the same numerical file descriptor.
using RegistrationToken = std::uint64_t;

inline constexpr RegistrationToken InvalidToken = 0;

// Reports readiness by registration identity, not FD.
//
// Error and Hangup can be reported independently of
// the requested Read/Write interest.
//
// One wait() result may contain more than one event
// carrying the same token. Consumers must tolerate
// repeated notification entries.
struct ReadyEvent {
    RegistrationToken token = InvalidToken;
    EventMask events = EventMask::None;
};

// Contract for native readiness-based I/O pollers.
//
// Does NOT own registered file descriptors.
// The caller must unregister an FD before closing it.
//
// All methods are called from one event-loop thread.
// Concurrent calls are outside this interface contract.
//
// add():
//   Register a valid FD with a nonzero unique token.
//   Interest must contain Read, Write, or both.
//   Duplicate FD registration is invalid.
//
// modify():
//   Update an existing FD's interest mask.
//   The registration token must remain unchanged.
//
// remove():
//   Unregister an FD without closing it.
//   Removing an already-absent FD is idempotent.
//
// wait():
//   Return readiness events or an empty vector on
//   timeout. A timeout of zero polls immediately.
//   A positive timeout is finite.
//   -1ms requests an indefinite wait.
//   Other negative timeout values are invalid.
//
// Invalid parameters and registration misuse should
// result in appropriate standard exceptions.
// Native system-call failures should propagate as
// std::system_error.
//
// A removed registration may still have a stale
// event in an already-returned batch. Consumers
// must validate tokens against live connections.
//
// Backend-specific behavior is implemented later.
class EventPoller {
  public:
    virtual ~EventPoller() = default;

    EventPoller() = default;

    EventPoller(const EventPoller&) = delete;
    EventPoller& operator=(const EventPoller&) = delete;

    virtual void add(int fd, EventMask interest, RegistrationToken token) = 0;

    virtual void modify(int fd, EventMask interest) = 0;

    virtual void remove(int fd) = 0;

    [[nodiscard]] virtual std::vector<ReadyEvent> wait(std::chrono::milliseconds timeout) = 0;
};

[[nodiscard]] constexpr bool valid_wait_timeout(std::chrono::milliseconds timeout) noexcept {
    return timeout.count() >= 0 || timeout == std::chrono::milliseconds{-1};
}

} // namespace redisdb::net
