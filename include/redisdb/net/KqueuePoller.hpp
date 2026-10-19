#pragma once

#include "redisdb/net/EventPoller.hpp"
#include "redisdb/net/UniqueFd.hpp"

#if defined(__APPLE__)

#include <unordered_map>

namespace redisdb::net {

// Level-triggered macOS kqueue implementation.
//
// Single-threaded API, like EventPoller.
//
// Owns the kqueue descriptor but not registered FDs.
// Unregister sockets before closing them.
class KqueuePoller final : public EventPoller {
  public:
    KqueuePoller();

    ~KqueuePoller() override = default;

    KqueuePoller(const KqueuePoller&) = delete;
    KqueuePoller& operator=(const KqueuePoller&) = delete;

    void add(int fd, EventMask interest, RegistrationToken token) override;

    void modify(int fd, EventMask interest) override;

    void remove(int fd) override;

    [[nodiscard]] std::vector<ReadyEvent> wait(std::chrono::milliseconds timeout) override;

    // Diagnostic borrowed handle. Do not close it.
    [[nodiscard]] int native_handle() const noexcept { return queue_.get(); }

  private:
    struct Registration {
        EventMask interest;
        RegistrationToken token;
    };

    void ensure_healthy() const;

    // Applies both read/write filter states.
    void apply_interest(int fd, EventMask interest, RegistrationToken token);

    // Removes both filters. ENOENT is allowed.
    void delete_filters(int fd);

    // Best-effort rollback for failed add().
    void discard_filters(int fd) noexcept;

    UniqueFd queue_;
    std::unordered_map<int, Registration> registrations_;

    // If rollback of a failed modification also fails,
    // do not continue with unknown kernel state.
    bool healthy_ = true;
};

} // namespace redisdb::net

#endif // defined(__APPLE__)
