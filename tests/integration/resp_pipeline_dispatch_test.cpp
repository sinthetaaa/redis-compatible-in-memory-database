#include "redisdb/net/ConnectionManager.hpp"
#include "redisdb/protocol/RespPipeline.hpp"

#if defined(__APPLE__)
#include "redisdb/net/KqueuePoller.hpp"
using NativePoller = redisdb::net::KqueuePoller;
#elif defined(__linux__)
#include "redisdb/net/EpollPoller.hpp"
using NativePoller = redisdb::net::EpollPoller;
#else
#error "Native TCP pipeline test requires macOS or Linux"
#endif

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using redisdb::net::Connection;
using redisdb::net::ConnectionManager;
using redisdb::net::TcpListener;
using redisdb::net::UniqueFd;

using redisdb::protocol::PipelineStop;
using redisdb::protocol::RespPipeline;
using redisdb::protocol::RespValue;

using namespace std::chrono_literals;

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

UniqueFd connect_loopback(std::uint16_t port) {
    const int raw = ::socket(AF_INET, SOCK_STREAM, 0);

    check(raw >= 0, "client socket creation failed");

    UniqueFd client(raw);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    check(::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                    static_cast<socklen_t>(sizeof(address))) == 0,
          "loopback connect failed");

    const int flags = ::fcntl(client.get(), F_GETFL);

    check(flags >= 0, "fcntl get failed");

    check(::fcntl(client.get(), F_SETFL, flags | O_NONBLOCK) == 0, "fcntl nonblocking failed");

    return client;
}

void send_all(int fd, std::string_view bytes) {
    std::size_t offset = 0;

    while (offset < bytes.size()) {
        const auto sent = ::send(fd, bytes.data() + offset, bytes.size() - offset, 0);

        check(sent > 0, "client send failed");

        offset += static_cast<std::size_t>(sent);
    }
}

void receive_available(int fd, std::string& output) {
    char buffer[1024];

    for (;;) {
        const auto received = ::recv(fd, buffer, sizeof(buffer), 0);

        if (received > 0) {
            output.append(buffer, static_cast<std::size_t>(received));
            continue;
        }

        if (received == 0) {
            return;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        throw std::runtime_error("client recv failed");
    }
}

RespPipeline::Limits pipeline_limits(std::size_t frames, std::size_t reply_bytes) {
    RespPipeline::Limits limits;
    limits.max_frames_per_drain = frames;
    limits.max_reply_bytes = reply_bytes;
    return limits;
}

struct Fixture {
    NativePoller poller;
    ConnectionManager manager;
    RespPipeline pipeline;

    std::size_t processed = 0;
    std::size_t callback_count = 0;
    std::size_t need_more_count = 0;
    std::size_t deferred_count = 0;
    std::size_t pressure_count = 0;

    Fixture(ConnectionManager::Limits manager_limits = {},
            RespPipeline::Limits protocol_limits = pipeline_limits(2, 8))
        : poller(), manager(TcpListener::bind_loopback(0), poller, manager_limits),
          pipeline(protocol_limits) {
        manager.set_resumable_input_handler([this](Connection& connection) {
            ++callback_count;

            const auto result =
                pipeline.drain(connection, [](const RespValue& request) { return request; });

            processed += result.frames_processed;

            using Action = ConnectionManager::InputAction;

            switch (result.stop) {
            case PipelineStop::NeedMore:
                ++need_more_count;
                return Action::AwaitRead;

            case PipelineStop::BudgetExhausted:
                ++deferred_count;
                return Action::Continue;

            case PipelineStop::Backpressured:
                ++pressure_count;
                return Action::AwaitWrite;

            case PipelineStop::Closing:
                throw std::logic_error("echo test pipeline must not request a graceful close");

            case PipelineStop::Invalid:
                throw std::runtime_error("invalid RESP2 pipeline input");
            }

            throw std::runtime_error("unknown pipeline status");
        });
    }

    void accept_clients(std::size_t expected) {
        for (int attempt = 0; attempt < 30 && manager.connection_count() < expected; ++attempt) {
            (void)manager.poll_once(25ms);
        }

        check(manager.connection_count() == expected, "clients were not accepted");
    }

    std::string collect(int fd, std::size_t expected) {
        std::string bytes;

        for (int attempt = 0; attempt < 120 && bytes.size() < expected; ++attempt) {
            (void)manager.poll_once(10ms);
            receive_available(fd, bytes);
        }

        check(bytes.size() == expected, "expected TCP reply bytes not received");

        return bytes;
    }
};

void test_fair_deferred_processing() {
    Fixture fixture;

    auto client = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(1);

    const std::string frames = ":1\r\n:2\r\n:3\r\n:4\r\n:5\r\n";

    send_all(client.get(), frames);

    const auto first = fixture.manager.poll_once(100ms);

    check(first.bytes_received == frames.size(), "initial TCP batch was not received");

    check(fixture.processed == 2, "initial fairness budget was exceeded");

    check(fixture.deferred_count == 1, "deferred continuation was not scheduled");

    check(fixture.collect(client.get(), frames.size()) == frames,
          "deferred TCP replies lost FIFO order");

    check(fixture.processed == 5, "deferred requests not processed exactly once");

    check(fixture.manager.connection_count() == 1, "healthy pipeline connection closed");
}

void test_output_backpressure_recovery() {
    ConnectionManager::Limits limits;
    limits.connection.max_output_bytes = 32;
    limits.max_write_per_event = 8;

    Fixture fixture(limits, pipeline_limits(16, 8));

    auto client = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(1);

    std::string frames;

    for (int index = 0; index < 12; ++index) {
        frames += ":1\r\n";
    }

    send_all(client.get(), frames);

    check(fixture.collect(client.get(), frames.size()) == frames,
          "backpressured replies were lost or reordered");

    check(fixture.pressure_count > 0, "output backpressure was never exercised");

    check(fixture.processed == 12, "backpressured requests duplicated or dropped");

    check(fixture.manager.connection_count() == 1, "backpressured healthy client was closed");
}

void test_fragmented_pipeline() {
    Fixture fixture;

    auto client = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(1);

    send_all(client.get(), "+HEL");

    (void)fixture.manager.poll_once(100ms);

    check(fixture.processed == 0, "incomplete TCP request was processed");

    send_all(client.get(), "LO\r\n:7\r\n");

    const std::string expected = "+HELLO\r\n:7\r\n";

    check(fixture.collect(client.get(), expected.size()) == expected,
          "fragmented TCP pipeline failed");

    check(fixture.processed == 2, "fragmented requests not processed exactly once");
}

void test_half_close_with_deferred_frames() {
    Fixture fixture;

    auto client = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(1);

    const std::string frames = ":1\r\n:2\r\n:3\r\n:4\r\n:5\r\n:6\r\n";

    send_all(client.get(), frames);

    check(::shutdown(client.get(), SHUT_WR) == 0, "client half-close failed");

    check(fixture.collect(client.get(), frames.size()) == frames,
          "half-closed client lost queued replies");

    for (int attempt = 0; attempt < 60 && fixture.manager.connection_count() != 0; ++attempt) {
        (void)fixture.manager.poll_once(10ms);
    }

    check(fixture.processed == 6, "EOF lost or duplicated a request");

    check(fixture.manager.connection_count() == 0, "half-closed connection not cleaned up");
}

void test_malformed_client_isolation() {
    Fixture fixture;

    auto bad = connect_loopback(fixture.manager.listener_port());

    auto healthy = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(2);

    send_all(bad.get(), ":oops\r\n");

    for (int attempt = 0; attempt < 30 && fixture.manager.connection_count() != 1; ++attempt) {
        (void)fixture.manager.poll_once(10ms);
    }

    check(fixture.manager.connection_count() == 1, "malformed TCP client not isolated");

    send_all(healthy.get(), ":9\r\n");

    check(fixture.collect(healthy.get(), 4) == ":9\r\n",
          "healthy peer failed after malformed client");
}

// Phase 2F.3B: exercise hardened RESP2 parsing through native TCP dispatch,
// not only through the isolated parser unit tests.
void test_early_bulk_terminator_peer_isolation_2f() {
    // A binary bulk echo is nine wire bytes; reserve more than the default eight.
    Fixture fixture({}, pipeline_limits(2, 16));

    auto bad = connect_loopback(fixture.manager.listener_port());
    auto healthy = connect_loopback(fixture.manager.listener_port());
    fixture.accept_clients(2);

    // A healthy peer has a legitimate incomplete binary bulk frame.
    send_all(healthy.get(), "$3\r\nA");
    for (int attempt = 0; attempt < 30 && fixture.need_more_count == 0; ++attempt) {
        (void)fixture.manager.poll_once(10ms);
    }
    check(fixture.need_more_count > 0 && fixture.processed == 0,
          "healthy fragmented binary frame was processed prematurely");

    // This frame must be invalid as soon as X appears, without waiting for LF.
    send_all(bad.get(), "$3\r\nabcX");
    for (int attempt = 0; attempt < 30 && fixture.manager.connection_count() != 1; ++attempt) {
        (void)fixture.manager.poll_once(10ms);
    }
    check(fixture.manager.connection_count() == 1, "early-invalid bulk client was not isolated");
    check(fixture.processed == 0, "malformed bulk executed a reply callback");

    const std::string tail("\0B\r\n", 4);
    send_all(healthy.get(), tail);
    const std::string expected = std::string("$3\r\nA") + tail;
    check(fixture.collect(healthy.get(), expected.size()) == expected,
          "healthy binary frame was corrupted by malformed peer");
    check(fixture.processed == 1 && fixture.manager.connection_count() == 1,
          "healthy binary frame not processed exactly once");

    send_all(healthy.get(), ":9\r\n");
    check(fixture.collect(healthy.get(), 4) == ":9\r\n" && fixture.processed == 2,
          "healthy peer could not process subsequent request");
}

void test_noncanonical_lengths_peer_isolation_2f() {
    constexpr std::string_view invalid_frames[] = {"$-01\r\n", "*-01\r\n",     "$-0\r\n",
                                                   "*00\r\n",  "$01\r\nx\r\n", "*01\r\n:1\r\n"};

    for (std::string_view invalid : invalid_frames) {
        Fixture fixture;
        auto bad = connect_loopback(fixture.manager.listener_port());
        auto healthy = connect_loopback(fixture.manager.listener_port());
        fixture.accept_clients(2);

        send_all(bad.get(), invalid);
        for (int attempt = 0; attempt < 30 && fixture.manager.connection_count() != 1; ++attempt) {
            (void)fixture.manager.poll_once(10ms);
        }
        check(fixture.manager.connection_count() == 1,
              "noncanonical length client was not isolated");
        check(fixture.processed == 0, "noncanonical length executed a callback");

        send_all(healthy.get(), ":7\r\n");
        check(fixture.collect(healthy.get(), 4) == ":7\r\n",
              "healthy peer affected by a noncanonical length");
        check(fixture.processed == 1 && fixture.manager.connection_count() == 1,
              "healthy peer not processed once after invalid length");
    }
}

void test_valid_prefix_then_invalid_client_isolation_2f() {
    Fixture fixture;
    auto bad = connect_loopback(fixture.manager.listener_port());
    auto healthy = connect_loopback(fixture.manager.listener_port());
    fixture.accept_clients(2);

    // One valid command-like frame precedes an invalid frame in the same batch.
    // The fail-fast close policy does NOT guarantee delivery of the first reply:
    // it may still be queued when the bad connection is closed. Do not assert it.
    send_all(bad.get(), ":1\r\n$3\r\nabcX");
    for (int attempt = 0; attempt < 30 && fixture.manager.connection_count() != 1; ++attempt) {
        (void)fixture.manager.poll_once(10ms);
    }
    check(fixture.manager.connection_count() == 1,
          "valid-prefix/malformed-suffix client was not closed");
    check(fixture.processed == 1, "valid prefix must run once; malformed suffix must never run");

    // Poll again to ensure a stale/deferred bad-client token cannot replay work.
    for (int attempt = 0; attempt < 3; ++attempt) {
        (void)fixture.manager.poll_once(0ms);
    }
    check(fixture.processed == 1, "closed malformed client replayed a prefix request");

    send_all(healthy.get(), ":8\r\n");
    check(fixture.collect(healthy.get(), 4) == ":8\r\n",
          "healthy peer could not reply after malformed pipeline");
    check(fixture.processed == 2 && fixture.manager.connection_count() == 1,
          "malformed batch contaminated another client");
}

} // namespace

int main() {
    try {
        test_fair_deferred_processing();
        test_output_backpressure_recovery();
        test_fragmented_pipeline();
        test_half_close_with_deferred_frames();
        test_malformed_client_isolation();
        test_early_bulk_terminator_peer_isolation_2f();
        test_noncanonical_lengths_peer_isolation_2f();
        test_valid_prefix_then_invalid_client_isolation_2f();

        std::cout << "Native TCP pipeline dispatch tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native TCP pipeline dispatch failed: " << error.what() << '\n';

        return 1;
    }
}
