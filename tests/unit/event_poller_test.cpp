#include "redisdb/net/EventPoller.hpp"
#include "redisdb/net/UniqueFd.hpp"

#include <fcntl.h>
#include <sys/socket.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

using redisdb::net::EventMask;
using redisdb::net::EventPoller;
using redisdb::net::ReadyEvent;
using redisdb::net::RegistrationToken;
using redisdb::net::UniqueFd;

using namespace std::chrono_literals;

static_assert(std::is_abstract_v<EventPoller>);
static_assert(std::has_virtual_destructor_v<EventPoller>);
static_assert(!std::is_copy_constructible_v<EventPoller>);

static_assert(redisdb::net::valid_interest(EventMask::Read));

static_assert(redisdb::net::valid_interest(EventMask::Read | EventMask::Write));

static_assert(!redisdb::net::valid_interest(EventMask::Error));

static_assert(!redisdb::net::valid_interest(EventMask::None));

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Callable>
void expect_exception(Callable&& action, const char* message) {
    bool caught = false;

    try {
        std::forward<Callable>(action)();
    } catch (const Exception&) {
        caught = true;
    }

    check(caught, message);
}

// Test-only implementation of the interface.
//
// It does not use kqueue, epoll or kernel readiness.
// Events are injected explicitly by the tests.
class FakePoller final : public EventPoller {
  public:
    struct Registration {
        EventMask interest;
        RegistrationToken token;
    };

    void add(int fd, EventMask interest, RegistrationToken token) override {
        if (fd < 0 || !redisdb::net::valid_interest(interest) ||
            token == redisdb::net::InvalidToken) {
            throw std::invalid_argument("invalid registration parameters");
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

        registrations_.emplace(fd, Registration{interest, token});
    }

    void modify(int fd, EventMask interest) override {
        if (!redisdb::net::valid_interest(interest)) {
            throw std::invalid_argument("invalid interest mask");
        }

        const auto it = registrations_.find(fd);

        if (it == registrations_.end()) {
            throw std::logic_error("descriptor is not registered");
        }

        it->second.interest = interest;
    }

    void remove(int fd) override { registrations_.erase(fd); }

    [[nodiscard]] std::vector<ReadyEvent> wait(std::chrono::milliseconds timeout) override {
        if (!redisdb::net::valid_wait_timeout(timeout)) {
            throw std::invalid_argument("invalid wait timeout");
        }

        auto result = std::move(pending_);
        pending_.clear();

        return result;
    }

    [[nodiscard]] const Registration& registration(int fd) const { return registrations_.at(fd); }

    [[nodiscard]] bool contains(int fd) const { return registrations_.contains(fd); }

    void inject(ReadyEvent event) { pending_.push_back(event); }

  private:
    std::unordered_map<int, Registration> registrations_;

    std::vector<ReadyEvent> pending_;
};

void test_event_masks() {
    EventMask mask = EventMask::Read | EventMask::Write;

    check(redisdb::net::contains_all(mask, EventMask::Read), "Read flag missing");

    check(redisdb::net::contains_all(mask, EventMask::Write), "Write flag missing");

    check(!redisdb::net::contains_all(mask, EventMask::Error), "unexpected Error flag");

    mask |= EventMask::Hangup;

    check(redisdb::net::contains_all(mask, EventMask::Hangup), "Hangup flag missing");

    check(!redisdb::net::valid_interest(mask), "output flags must not be valid interest");

    check(redisdb::net::valid_wait_timeout(0ms), "zero timeout invalid");

    check(redisdb::net::valid_wait_timeout(25ms), "positive timeout invalid");

    check(redisdb::net::valid_wait_timeout(-1ms), "infinite timeout invalid");

    check(!redisdb::net::valid_wait_timeout(-2ms), "invalid negative timeout accepted");
}

void test_registration_lifecycle() {
    FakePoller poller;

    poller.add(10, EventMask::Read, 101);

    check(poller.contains(10), "registration missing");

    check(poller.registration(10).token == 101, "incorrect registration token");

    poller.modify(10, EventMask::Read | EventMask::Write);

    check(poller.registration(10).interest == (EventMask::Read | EventMask::Write),
          "modify did not update interest");

    check(poller.registration(10).token == 101, "modify changed registration token");

    poller.remove(10);

    check(!poller.contains(10), "remove failed");

    poller.remove(10);

    check(!poller.contains(10), "idempotent remove failed");
}

void test_invalid_registrations() {
    FakePoller poller;

    expect_exception<std::invalid_argument>([&] { poller.add(-1, EventMask::Read, 1); },
                                            "negative fd accepted");

    expect_exception<std::invalid_argument>([&] { poller.add(10, EventMask::None, 1); },
                                            "empty interest accepted");

    expect_exception<std::invalid_argument>([&] { poller.add(10, EventMask::Error, 1); },
                                            "error-only interest accepted");

    expect_exception<std::invalid_argument>([&] { poller.add(10, EventMask::Read, 0); },
                                            "invalid token accepted");

    poller.add(10, EventMask::Read, 11);

    expect_exception<std::logic_error>([&] { poller.add(10, EventMask::Write, 12); },
                                       "duplicate fd accepted");

    expect_exception<std::logic_error>([&] { poller.add(12, EventMask::Read, 11); },
                                       "duplicate active token accepted");

    expect_exception<std::logic_error>([&] { poller.modify(99, EventMask::Write); },
                                       "modify on absent fd accepted");

    expect_exception<std::invalid_argument>([&] { poller.modify(10, EventMask::Hangup); },
                                            "invalid modification accepted");

    check(poller.registration(10).interest == EventMask::Read,
          "failed operation changed registration");
}

void test_event_delivery_and_timeout() {
    FakePoller poller;

    poller.add(15, EventMask::Read, 55);

    check(poller.wait(0ms).empty(), "empty poll should return no events");

    poller.inject({55, EventMask::Read | EventMask::Hangup});

    const auto events = poller.wait(10ms);

    check(events.size() == 1, "expected one injected event");

    check(events[0].token == 55, "incorrect readiness token");

    check(redisdb::net::contains_all(events[0].events, EventMask::Read), "Read readiness missing");

    check(redisdb::net::contains_all(events[0].events, EventMask::Hangup),
          "Hangup readiness missing");

    check(poller.wait(-1ms).empty(), "fake wait should drain pending events");

    expect_exception<std::invalid_argument>([&] { (void)poller.wait(-2ms); },
                                            "invalid negative timeout accepted");
}

void test_stale_token_and_fd_reuse() {
    FakePoller poller;

    // Original connection owns numerical fd 20.
    poller.add(20, EventMask::Read, 1001);

    // Simulate readiness queued before removal.
    poller.inject({1001, EventMask::Read});

    poller.remove(20);

    // OS may now reuse the same fd number for
    // an entirely different connection.
    poller.add(20, EventMask::Write, 1002);

    poller.inject({1002, EventMask::Write});

    const auto events = poller.wait(0ms);

    check(events.size() == 2, "expected old and new events");

    const RegistrationToken live_token = poller.registration(20).token;

    std::size_t accepted = 0;
    std::size_t stale = 0;

    for (const ReadyEvent& event : events) {
        if (event.token == live_token) {
            ++accepted;
        } else {
            ++stale;
        }
    }

    check(accepted == 1, "current readiness not recognized");

    check(stale == 1, "stale readiness not rejected");

    check(live_token == 1002, "new registration identity incorrect");
}

void test_poller_does_not_own_fds() {
    int raw[2] = {-1, -1};

    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, raw) != 0) {
        throw std::runtime_error("socketpair creation failed");
    }

    UniqueFd first(raw[0]);
    UniqueFd second(raw[1]);

    {
        FakePoller poller;

        poller.add(first.get(), EventMask::Read, 2001);

        poller.remove(first.get());
    }

    check(::fcntl(first.get(), F_GETFD) != -1, "poller incorrectly closed registered fd");

    check(::fcntl(second.get(), F_GETFD) != -1, "poller affected unrelated fd");
}

} // namespace

int main() {
    try {
        test_event_masks();
        test_registration_lifecycle();
        test_invalid_registrations();
        test_event_delivery_and_timeout();
        test_stale_token_and_fd_reuse();
        test_poller_does_not_own_fds();

        std::cout << "EventPoller interface tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "EventPoller test failed: " << error.what() << '\n';

        return 1;
    }
}
