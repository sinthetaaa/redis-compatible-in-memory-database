#pragma once

#include "redisdb/net/EventPoller.hpp"
#include "redisdb/net/UniqueFd.hpp"

#if defined(__linux__)

#include <unordered_map>

namespace redisdb::net {

// Level-triggered Linux epoll implementation.
//
// Owns only its epoll descriptor. Registered sockets
// remain owned by their caller.
//
// Single-threaded, matching EventPoller.
// Unregister descriptors before closing them.
class EpollPoller final : public EventPoller {
  public:
    EpollPoller();

    ~EpollPoller() override = default;

    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;

    void add(int fd, EventMask interest, RegistrationToken token) override;

    void modify(int fd, EventMask interest) override;

    void remove(int fd) override;

    [[nodiscard]] std::vector<ReadyEvent> wait(std::chrono::milliseconds timeout) override;

    // Borrowed descriptor for diagnostics only.
    [[nodiscard]] int native_handle() const noexcept { return epoll_fd_.get(); }

  private:
    struct Registration {
        EventMask interest;
        RegistrationToken token;
    };

    UniqueFd epoll_fd_;

    std::unordered_map<int, Registration> registrations_;
};

} // namespace redisdb::net

#endif // defined(__linux__)
