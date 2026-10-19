#include "redisdb/net/KqueuePoller.hpp"

#if defined(__APPLE__)

#include <sys/event.h>
#include <sys/time.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace redisdb::net {

namespace {

static_assert(sizeof(RegistrationToken) <= sizeof(std::uintptr_t),
              "Registration tokens must fit in kqueue udata");

[[noreturn]] void throw_errno(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

// udata is an opaque value. No pointer is dereferenced.
[[nodiscard]] void* encode_token(RegistrationToken token) noexcept {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(token));
}

[[nodiscard]] RegistrationToken decode_token(void* opaque) noexcept {
    return static_cast<RegistrationToken>(reinterpret_cast<std::uintptr_t>(opaque));
}

[[nodiscard]] std::array<struct kevent, 2> interest_changes(int fd, EventMask interest,
                                                            RegistrationToken token) {
    std::array<struct kevent, 2> changes{};

    const auto read_flags = static_cast<unsigned short>(
        EV_ADD | EV_RECEIPT | (contains_all(interest, EventMask::Read) ? EV_ENABLE : EV_DISABLE));

    const auto write_flags = static_cast<unsigned short>(
        EV_ADD | EV_RECEIPT | (contains_all(interest, EventMask::Write) ? EV_ENABLE : EV_DISABLE));

    const auto ident = static_cast<std::uintptr_t>(fd);

    EV_SET(&changes[0], ident, EVFILT_READ, read_flags, 0, 0, encode_token(token));

    EV_SET(&changes[1], ident, EVFILT_WRITE, write_flags, 0, 0, encode_token(token));

    return changes;
}

[[nodiscard]] std::array<struct kevent, 2> delete_changes(int fd) {
    std::array<struct kevent, 2> changes{};

    const auto ident = static_cast<std::uintptr_t>(fd);
    const auto flags = static_cast<unsigned short>(EV_DELETE | EV_RECEIPT);

    EV_SET(&changes[0], ident, EVFILT_READ, flags, 0, 0, nullptr);

    EV_SET(&changes[1], ident, EVFILT_WRITE, flags, 0, 0, nullptr);

    return changes;
}

void submit_changes(int kq, const std::array<struct kevent, 2>& changes, bool allow_missing) {
    std::array<struct kevent, 2> receipts{};

    const int count = ::kevent(kq, changes.data(), 2, receipts.data(), 2, nullptr);

    if (count == -1) {
        throw_errno("kevent registration");
    }

    if (count != 2) {
        throw std::runtime_error("kqueue returned incomplete registration receipts");
    }

    for (const auto& receipt : receipts) {
        if ((receipt.flags & EV_ERROR) == 0) {
            throw std::runtime_error("kqueue receipt missing EV_ERROR");
        }

        if (receipt.data == 0) {
            continue;
        }

        if (allow_missing && receipt.data == ENOENT) {
            continue;
        }

        throw std::system_error(static_cast<int>(receipt.data), std::generic_category(),
                                "kevent filter registration");
    }
}

[[nodiscard]] timespec to_timespec(std::chrono::milliseconds duration) {
    timespec result{};

    result.tv_sec = static_cast<time_t>(duration.count() / 1000);

    result.tv_nsec = static_cast<long>((duration.count() % 1000) * 1000000);

    return result;
}

} // namespace

KqueuePoller::KqueuePoller() {
    const int raw_fd = ::kqueue();

    if (raw_fd == -1) {
        throw_errno("kqueue");
    }

    queue_.reset(raw_fd);

    const int flags = ::fcntl(queue_.get(), F_GETFD);

    if (flags == -1) {
        throw_errno("fcntl(F_GETFD) kqueue");
    }

    if (::fcntl(queue_.get(), F_SETFD, flags | FD_CLOEXEC) == -1) {
        throw_errno("fcntl(F_SETFD) kqueue");
    }
}

void KqueuePoller::ensure_healthy() const {
    if (!healthy_) {
        throw std::logic_error("kqueue state is inconsistent after failed rollback");
    }
}

void KqueuePoller::apply_interest(int fd, EventMask interest, RegistrationToken token) {
    submit_changes(queue_.get(), interest_changes(fd, interest, token), false);
}

void KqueuePoller::delete_filters(int fd) {
    submit_changes(queue_.get(), delete_changes(fd), true);
}

void KqueuePoller::discard_filters(int fd) noexcept {
    try {
        delete_filters(fd);
    } catch (...) {
        healthy_ = false;
    }
}

void KqueuePoller::add(int fd, EventMask interest, RegistrationToken token) {
    ensure_healthy();

    if (fd < 0 || !valid_interest(interest) || token == InvalidToken) {
        throw std::invalid_argument("invalid kqueue registration");
    }

    if (registrations_.contains(fd)) {
        throw std::logic_error("descriptor is already registered");
    }

    for (const auto& [existing_fd, entry] : registrations_) {
        (void)existing_fd;

        if (entry.token == token) {
            throw std::logic_error("duplicate live registration token");
        }
    }

    const auto [it, inserted] = registrations_.emplace(fd, Registration{interest, token});

    if (!inserted) {
        throw std::logic_error("descriptor registration failed");
    }

    try {
        apply_interest(fd, interest, token);
    } catch (...) {
        discard_filters(fd);
        registrations_.erase(it);
        throw;
    }
}

void KqueuePoller::modify(int fd, EventMask interest) {
    ensure_healthy();

    if (!valid_interest(interest)) {
        throw std::invalid_argument("invalid kqueue interest mask");
    }

    const auto it = registrations_.find(fd);

    if (it == registrations_.end()) {
        throw std::logic_error("descriptor is not registered");
    }

    if (it->second.interest == interest) {
        return;
    }

    const EventMask previous = it->second.interest;

    try {
        apply_interest(fd, interest, it->second.token);
    } catch (...) {
        const std::exception_ptr original = std::current_exception();

        try {
            apply_interest(fd, previous, it->second.token);
        } catch (...) {
            healthy_ = false;
        }

        std::rethrow_exception(original);
    }

    it->second.interest = interest;
}

void KqueuePoller::remove(int fd) {
    ensure_healthy();

    const auto it = registrations_.find(fd);

    if (it == registrations_.end()) {
        return;
    }

    delete_filters(fd);
    registrations_.erase(it);
}

std::vector<ReadyEvent> KqueuePoller::wait(std::chrono::milliseconds timeout) {
    ensure_healthy();

    if (!valid_wait_timeout(timeout)) {
        throw std::invalid_argument("invalid kqueue wait timeout");
    }

    constexpr std::size_t BatchSize = 64;
    std::array<struct kevent, BatchSize> events{};

    const bool infinite = timeout == std::chrono::milliseconds{-1};

    const auto start = std::chrono::steady_clock::now();

    auto remaining = timeout;

    for (;;) {
        timespec interval{};

        const timespec* timeout_pointer = nullptr;

        if (!infinite) {
            interval = to_timespec(remaining);
            timeout_pointer = &interval;
        }

        const int count = ::kevent(queue_.get(), nullptr, 0, events.data(),
                                   static_cast<int>(events.size()), timeout_pointer);

        if (count >= 0) {
            std::vector<ReadyEvent> ready;
            ready.reserve(static_cast<std::size_t>(count));

            for (int index = 0; index < count; ++index) {
                const auto& event = events[static_cast<std::size_t>(index)];

                EventMask flags = EventMask::None;

                if (event.filter == EVFILT_READ) {
                    flags |= EventMask::Read;
                }

                if (event.filter == EVFILT_WRITE) {
                    flags |= EventMask::Write;
                }

                if ((event.flags & EV_EOF) != 0) {
                    flags |= EventMask::Hangup;
                }

                if ((event.flags & EV_ERROR) != 0) {
                    flags |= EventMask::Error;
                }

                ready.push_back({decode_token(event.udata), flags});
            }

            return ready;
        }

        if (errno != EINTR) {
            throw_errno("kevent wait");
        }

        if (infinite || timeout.count() == 0) {
            continue;
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;

        if (elapsed >= timeout) {
            return {};
        }

        remaining = std::chrono::duration_cast<std::chrono::milliseconds>(timeout - elapsed);
    }
}

} // namespace redisdb::net

#endif // defined(__APPLE__)
