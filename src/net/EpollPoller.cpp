#include "redisdb/net/EpollPoller.hpp"

#if defined(__linux__)

#include <sys/epoll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace redisdb::net {

namespace {

[[noreturn]] void throw_errno(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

[[nodiscard]] std::uint32_t native_interest(EventMask interest) noexcept {
    std::uint32_t flags = EPOLLRDHUP;

    if (contains_all(interest, EventMask::Read)) {
        flags |= EPOLLIN;
    }

    if (contains_all(interest, EventMask::Write)) {
        flags |= EPOLLOUT;
    }

    // Deliberately no EPOLLET: level-triggered mode.
    return flags;
}

[[nodiscard]] epoll_event make_event(EventMask interest, RegistrationToken token) noexcept {
    epoll_event event{};

    event.events = native_interest(interest);
    event.data.u64 = token;

    return event;
}

[[nodiscard]] EventMask portable_events(std::uint32_t native) noexcept {
    EventMask flags = EventMask::None;

    if ((native & EPOLLIN) != 0U) {
        flags |= EventMask::Read;
    }

    if ((native & EPOLLOUT) != 0U) {
        flags |= EventMask::Write;
    }

    if ((native & EPOLLERR) != 0U) {
        flags |= EventMask::Error;
    }

    if ((native & (EPOLLHUP | EPOLLRDHUP)) != 0U) {
        flags |= EventMask::Hangup;
    }

    return flags;
}

} // namespace

EpollPoller::EpollPoller() {
    const int fd = ::epoll_create1(EPOLL_CLOEXEC);

    if (fd == -1) {
        throw_errno("epoll_create1");
    }

    epoll_fd_.reset(fd);
}

void EpollPoller::add(int fd, EventMask interest, RegistrationToken token) {
    if (fd < 0 || !valid_interest(interest) || token == InvalidToken) {
        throw std::invalid_argument("invalid epoll registration");
    }

    if (registrations_.contains(fd)) {
        throw std::logic_error("descriptor is already registered");
    }

    for (const auto& [registered_fd, entry] : registrations_) {
        (void)registered_fd;

        if (entry.token == token) {
            throw std::logic_error("duplicate live registration token");
        }
    }

    // Allocate bookkeeping before modifying kernel
    // state, so allocation failure cannot leave a
    // successful kernel registration untracked.
    const auto [it, inserted] = registrations_.emplace(fd, Registration{interest, token});

    if (!inserted) {
        throw std::logic_error("descriptor is already registered");
    }

    epoll_event event = make_event(interest, token);

    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &event) == -1) {
        const int error = errno;

        registrations_.erase(it);

        throw std::system_error(error, std::generic_category(), "epoll_ctl ADD");
    }
}

void EpollPoller::modify(int fd, EventMask interest) {
    if (!valid_interest(interest)) {
        throw std::invalid_argument("invalid epoll interest mask");
    }

    const auto it = registrations_.find(fd);

    if (it == registrations_.end()) {
        throw std::logic_error("descriptor is not registered");
    }

    if (it->second.interest == interest) {
        return;
    }

    epoll_event event = make_event(interest, it->second.token);

    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &event) == -1) {
        throw_errno("epoll_ctl MOD");
    }

    it->second.interest = interest;
}

void EpollPoller::remove(int fd) {
    const auto it = registrations_.find(fd);

    if (it == registrations_.end()) {
        return;
    }

    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr) == -1) {
        throw_errno("epoll_ctl DEL");
    }

    registrations_.erase(it);
}

std::vector<ReadyEvent> EpollPoller::wait(std::chrono::milliseconds timeout) {
    if (!valid_wait_timeout(timeout)) {
        throw std::invalid_argument("invalid epoll wait timeout");
    }

    constexpr std::size_t BatchSize = 64;

    std::array<epoll_event, BatchSize> events{};

    const bool infinite = timeout == std::chrono::milliseconds{-1};

    const auto started = std::chrono::steady_clock::now();

    for (;;) {
        int delay_ms = -1;

        if (!infinite) {
            if (timeout.count() == 0) {
                delay_ms = 0;
            } else {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);

                if (elapsed >= timeout) {
                    return {};
                }

                const auto remaining = timeout - elapsed;

                const auto bounded = std::min(
                    remaining.count(),
                    static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max()));

                delay_ms = static_cast<int>(bounded);
            }
        }

        const int count =
            ::epoll_wait(epoll_fd_.get(), events.data(), static_cast<int>(events.size()), delay_ms);

        if (count > 0) {
            std::vector<ReadyEvent> ready;

            ready.reserve(static_cast<std::size_t>(count));

            for (int index = 0; index < count; ++index) {
                const auto& event = events[static_cast<std::size_t>(index)];

                ready.push_back({event.data.u64, portable_events(event.events)});
            }

            return ready;
        }

        if (count == 0) {
            // A finite timeout larger than INT_MAX
            // may require multiple epoll_wait calls.
            if (!infinite && timeout.count() > std::numeric_limits<int>::max()) {
                continue;
            }

            return {};
        }

        if (errno != EINTR) {
            throw_errno("epoll_wait");
        }

        // Retry interrupted calls. For finite waits,
        // remaining time is recalculated above.
    }
}

} // namespace redisdb::net

#endif // defined(__linux__)
