#include "redisdb/net/ConnectionManager.hpp"
#include "redisdb/protocol/RespFramedInput.hpp"

#if defined(__APPLE__)
#include "redisdb/net/KqueuePoller.hpp"
using NativePoller = redisdb::net::KqueuePoller;
#elif defined(__linux__)
#include "redisdb/net/EpollPoller.hpp"
using NativePoller = redisdb::net::EpollPoller;
#else
#error "Native RESP2 dispatch test requires macOS or Linux"
#endif

#include <arpa/inet.h>
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
#include <vector>

using redisdb::net::Connection;
using redisdb::net::ConnectionManager;
using redisdb::net::RegistrationToken;
using redisdb::net::TcpListener;
using redisdb::net::UniqueFd;

using redisdb::protocol::ParseStatus;
using redisdb::protocol::RespFramedInput;
using redisdb::protocol::RespParser;
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

    check(raw >= 0, "TCP client socket creation failed");

    UniqueFd client(raw);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    check(::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                    static_cast<socklen_t>(sizeof(address))) == 0,
          "TCP loopback connection failed");

    return client;
}

void send_all(int fd, std::string_view data) {
    check(!data.empty(), "test attempted empty send");

    std::size_t offset = 0;

    while (offset < data.size()) {
        const auto sent = ::send(fd, data.data() + offset, data.size() - offset, 0);

        check(sent > 0, "TCP client send failed");

        offset += static_cast<std::size_t>(sent);
    }
}

struct Fixture {
    NativePoller poller;
    ConnectionManager manager;
    RespFramedInput framed;

    std::vector<std::pair<RegistrationToken, RespValue>> completed;

    std::size_t need_more = 0;
    std::size_t invalid = 0;

    explicit Fixture(ConnectionManager::Limits connection_limits = {},
                     RespParser::Limits parser_limits = {64 * 1024, 64 * 1024, 65536, 32,
                                                         64 * 1024})
        : poller(), manager(TcpListener::bind_loopback(0), poller, connection_limits),
          framed(parser_limits) {
        manager.set_input_handler([this](Connection& connection) {
            auto result = framed.poll(connection);

            if (result.status == ParseStatus::NeedMore) {
                ++need_more;
                return;
            }

            if (result.status == ParseStatus::Invalid) {
                ++invalid;

                // The ConnectionManager catches
                // this handler failure and removes
                // only the offending connection.
                throw std::runtime_error("invalid RESP2 frame");
            }

            check(result.value.has_value(), "Complete has no value");

            completed.emplace_back(connection.token(), std::move(*result.value));
        });
    }

    void accept_clients(std::size_t expected) {
        for (int attempt = 0; attempt < 20 && manager.connection_count() < expected; ++attempt) {
            (void)manager.poll_once(100ms);
        }

        check(manager.connection_count() == expected, "TCP clients not accepted");
    }

    ConnectionManager::DispatchResult receive(std::size_t expected) {
        ConnectionManager::DispatchResult total{};

        for (int attempt = 0; attempt < 30 && total.bytes_received < expected; ++attempt) {
            const auto result = manager.poll_once(100ms);

            total.bytes_received += result.bytes_received;

            total.closed += result.closed;
            total.handler_failures += result.handler_failures;
        }

        check(total.bytes_received == expected, "TCP byte count mismatch");

        return total;
    }
};

void test_fragmented_tcp_array() {
    Fixture fixture;

    auto client = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(1);

    const std::vector<std::string> chunks = {"*2\r\n$3\r\nG", "ET\r\n$3\r\nk", "ey\r\n"};

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        send_all(client.get(), chunks[i]);

        fixture.receive(chunks[i].size());

        if (i + 1 < chunks.size()) {
            check(fixture.completed.empty(), "incomplete TCP array was delivered");
        }
    }

    check(fixture.completed.size() == 1, "completed TCP request not delivered once");

    check(fixture.completed.front().second ==
              RespValue::array({RespValue::bulk_string("GET"), RespValue::bulk_string("key")}),
          "fragmented TCP array decoded incorrectly");

    const auto token = fixture.completed.front().first;

    const Connection* connection = fixture.manager.find(token);

    check(connection != nullptr && connection->input().empty(),
          "completed TCP request not consumed");

    check(fixture.need_more >= 2, "partial TCP requests not retained");
}

void test_bytewise_binary_tcp_payload() {
    Fixture fixture;

    auto client = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(1);

    std::string binary("\0\r\n", 3);
    binary.push_back(static_cast<char>(0xff));

    std::string frame("$4\r\n");
    frame.append(binary);
    frame.append("\r\n");

    for (std::size_t i = 0; i < frame.size(); ++i) {
        send_all(client.get(), std::string_view(frame.data() + i, 1));

        fixture.receive(1);

        if (i + 1 < frame.size()) {
            check(fixture.completed.empty(), "partial binary TCP frame completed");
        }
    }

    check(fixture.completed.size() == 1, "binary TCP payload not decoded exactly once");

    check(fixture.completed.front().second == RespValue::bulk_string(binary),
          "binary TCP payload corrupted");
}

void test_independent_tcp_clients() {
    Fixture fixture;

    auto first = connect_loopback(fixture.manager.listener_port());

    auto second = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(2);

    send_all(first.get(), "+HEL");
    fixture.receive(4);

    check(fixture.completed.empty(), "partial first-client input completed");

    send_all(second.get(), ":7\r\n");
    fixture.receive(4);

    check(fixture.completed.size() == 1 &&
              fixture.completed.front().second == RespValue::integer(7),
          "second client's request not isolated");

    const auto second_token = fixture.completed.front().first;

    send_all(first.get(), "LO\r\n");
    fixture.receive(4);

    check(fixture.completed.size() == 2, "first client's request not completed");

    check(fixture.completed.back().second == RespValue::simple_string("HELLO"),
          "first client's buffered bytes were lost");

    check(fixture.completed.back().first != second_token, "independent clients shared identity");

    check(fixture.manager.connection_count() == 2, "healthy clients unexpectedly closed");
}

void test_invalid_client_isolation() {
    Fixture fixture;

    auto bad = connect_loopback(fixture.manager.listener_port());

    auto good = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(2);

    send_all(bad.get(), ":oops\r\n");

    const auto bad_result = fixture.receive(7);

    check(fixture.invalid == 1, "invalid request not identified");

    check(bad_result.handler_failures == 1 && bad_result.closed == 1,
          "invalid client was not isolated");

    check(fixture.manager.connection_count() == 1, "invalid input affected healthy client");

    send_all(good.get(), ":42\r\n");
    fixture.receive(5);

    check(fixture.completed.size() == 1 &&
              fixture.completed.front().second == RespValue::integer(42),
          "healthy client stopped after malformed peer");
}

void test_incomplete_eof_cleanup() {
    Fixture fixture;

    auto client = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(1);

    send_all(client.get(), "$4\r\nab");
    fixture.receive(6);

    check(fixture.completed.empty() && fixture.need_more > 0,
          "incomplete request incorrectly completed");

    client.reset();

    for (int attempt = 0; attempt < 20 && fixture.manager.connection_count() != 0; ++attempt) {
        (void)fixture.manager.poll_once(100ms);
    }

    check(fixture.manager.connection_count() == 0, "incomplete EOF client not cleaned up");

    check(fixture.completed.empty(), "incomplete EOF fabricated a request");
}

void test_exact_tcp_input_limit() {
    ConnectionManager::Limits limits;
    limits.connection.max_input_bytes = 32;
    limits.connection.max_output_bytes = 64;

    Fixture fixture(limits, RespParser::Limits{32, 32, 64, 8, 16});

    auto client = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(1);

    const std::string payload(25, 'x');

    const std::string frame = "$25\r\n" + payload + "\r\n";

    check(frame.size() == 32, "exact input test frame size mismatch");

    send_all(client.get(), std::string_view(frame.data(), 16));

    fixture.receive(16);

    check(fixture.completed.empty(), "half-frame unexpectedly completed");

    send_all(client.get(), std::string_view(frame.data() + 16, frame.size() - 16));

    fixture.receive(frame.size() - 16);

    check(fixture.completed.size() == 1 &&
              fixture.completed.front().second == RespValue::bulk_string(payload),
          "exact-limit frame was not completed");

    check(fixture.manager.connection_count() == 1, "valid exact-limit TCP frame closed client");

    const auto token = fixture.completed.front().first;

    const auto* connection = fixture.manager.find(token);

    check(connection != nullptr && connection->input().empty(), "exact-limit input not consumed");
}

void test_declared_length_rejection() {
    ConnectionManager::Limits limits;
    limits.connection.max_input_bytes = 32;
    limits.connection.max_output_bytes = 64;

    Fixture fixture(limits, RespParser::Limits{32, 32, 64, 8, 16});

    auto client = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(1);

    send_all(client.get(), "$26\r\n");

    const auto result = fixture.receive(5);

    check(fixture.invalid == 1 && result.handler_failures == 1 && result.closed == 1,
          "oversized declaration not rejected");

    check(fixture.manager.connection_count() == 0, "oversized request client remained active");
}

void test_coalesced_frames_remain_for_phase2e() {
    Fixture fixture;

    auto client = connect_loopback(fixture.manager.listener_port());

    fixture.accept_clients(1);

    const std::string bytes = ":1\r\n:2\r\n";

    send_all(client.get(), bytes);
    fixture.receive(bytes.size());

    check(fixture.completed.size() == 1 &&
              fixture.completed.front().second == RespValue::integer(1),
          "first coalesced frame not isolated");

    const auto token = fixture.completed.front().first;

    const auto* connection = fixture.manager.find(token);

    check(connection != nullptr && connection->input() == ":2\r\n",
          "trailing frame lost before Phase 2E");

    // Automatic draining and response ordering
    // will be implemented in Phase 2E.
}

} // namespace

int main() {
    try {
        test_fragmented_tcp_array();
        test_bytewise_binary_tcp_payload();
        test_independent_tcp_clients();
        test_invalid_client_isolation();
        test_incomplete_eof_cleanup();
        test_exact_tcp_input_limit();
        test_declared_length_rejection();
        test_coalesced_frames_remain_for_phase2e();

        std::cout << "Native TCP RESP2 fragmentation tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native TCP RESP2 fragmentation failed: " << error.what() << '\n';

        return 1;
    }
}
