#include "redisdb/net/Connection.hpp"
#include "redisdb/net/UniqueFd.hpp"
#include "redisdb/protocol/RespFramedInput.hpp"

#include <fcntl.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using redisdb::net::Connection;
using redisdb::net::UniqueFd;

using redisdb::protocol::ParseResult;
using redisdb::protocol::ParseStatus;
using redisdb::protocol::RespFramedInput;
using redisdb::protocol::RespParser;
using redisdb::protocol::RespValue;

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void make_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);

    check(flags >= 0, "fcntl get flags failed");

    check(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "fcntl set nonblocking failed");
}

struct SocketPair {
    UniqueFd first;
    UniqueFd second;

    SocketPair() {
        int raw[2] = {-1, -1};

        check(::socketpair(AF_UNIX, SOCK_STREAM, 0, raw) == 0, "socketpair creation failed");

        first.reset(raw[0]);
        second.reset(raw[1]);

        make_nonblocking(first.get());
        make_nonblocking(second.get());
    }
};

struct Fixture {
    SocketPair sockets;
    Connection connection;

    explicit Fixture(Connection::Limits limits = Connection::Limits{})
        : sockets(), connection(std::move(sockets.first), 101, limits) {}

    void send_bytes(std::string_view bytes) {
        check(!bytes.empty(), "empty test send");

        const auto sent = ::send(sockets.second.get(), bytes.data(), bytes.size(), 0);

        check(sent == static_cast<ssize_t>(bytes.size()), "test socket send failed");
    }

    void receive_bytes(std::size_t count) {
        const auto result = connection.receive_once(count);

        check(result.bytes_read == count, "test socket read count mismatch");
    }

    ParseResult deliver(const RespFramedInput& reader, std::string_view bytes) {
        send_bytes(bytes);
        receive_bytes(bytes.size());

        return reader.poll(connection);
    }
};

void expect_complete(const ParseResult& result, const RespValue& expected, std::size_t consumed) {
    check(result.status == ParseStatus::Complete, "expected complete RESP2 frame");

    check(result.value.has_value(), "complete frame has no decoded value");

    check(*result.value == expected, "decoded frame value mismatch");

    check(result.consumed_bytes == consumed, "incorrect consumed-byte count");

    check(result.error.empty(), "complete frame has unexpected error");
}

void expect_need_more(const ParseResult& result) {
    check(result.status == ParseStatus::NeedMore, "expected incomplete RESP2 frame");

    check(!result.value.has_value(), "incomplete frame exposed value");

    check(result.consumed_bytes == 0, "incomplete frame consumed bytes");

    check(result.error.empty(), "incomplete frame has unexpected error");
}

void expect_invalid(const ParseResult& result) {
    check(result.status == ParseStatus::Invalid, "expected invalid RESP2 frame");

    check(!result.value.has_value(), "invalid frame exposed value");

    check(result.consumed_bytes == 0, "invalid frame consumed bytes");

    check(!result.error.empty(), "invalid frame has no error reason");
}

void test_empty_buffer() {
    Fixture fixture;
    const RespFramedInput reader;

    expect_need_more(reader.poll(fixture.connection));

    check(fixture.connection.input().empty(), "empty input unexpectedly changed");
}

void test_simple_frame_byte_by_byte() {
    Fixture fixture;
    const RespFramedInput reader;

    const std::string frame = "+OK\r\n";

    for (std::size_t index = 0; index < frame.size(); ++index) {
        const std::string_view fragment(frame.data() + index, 1);

        const auto result = fixture.deliver(reader, fragment);

        if (index + 1 < frame.size()) {
            expect_need_more(result);

            check(fixture.connection.input() == std::string_view(frame.data(), index + 1),
                  "partial input was not retained");
        } else {
            expect_complete(result, RespValue::simple_string("OK"), frame.size());
        }
    }

    check(fixture.connection.input().empty(), "completed frame not consumed");
}

void test_binary_bulk_byte_by_byte() {
    Fixture fixture;
    const RespFramedInput reader;

    std::string binary("\0\r\n", 3);
    binary.push_back(static_cast<char>(0xff));

    std::string frame("$4\r\n");
    frame.append(binary);
    frame.append("\r\n");

    for (std::size_t index = 0; index < frame.size(); ++index) {
        const auto result = fixture.deliver(reader, std::string_view(frame.data() + index, 1));

        if (index + 1 < frame.size()) {
            expect_need_more(result);
        } else {
            expect_complete(result, RespValue::bulk_string(binary), frame.size());
        }
    }

    check(fixture.connection.input().empty(), "binary frame bytes were retained");
}

void test_fragmented_nested_array() {
    Fixture fixture;
    const RespFramedInput reader;

    const std::string chunks[] = {"*2\r", "\n$3\r\nG", "ET\r\n$3\r\n", "key\r\n"};

    std::size_t total = 0;

    for (std::size_t index = 0; index < 4; ++index) {
        const std::string_view chunk = chunks[index];
        total += chunk.size();

        const auto result = fixture.deliver(reader, chunk);

        if (index < 3) {
            expect_need_more(result);
        } else {
            expect_complete(
                result,
                RespValue::array({RespValue::bulk_string("GET"), RespValue::bulk_string("key")}),
                total);
        }
    }

    check(fixture.connection.input().empty(), "array frame remained buffered");
}

void test_exact_single_frame_consumption() {
    Fixture fixture;
    const RespFramedInput reader;

    const std::string first = ":1\r\n";
    const std::string second = "$3\r\nabc\r\n";

    fixture.send_bytes(first + second);

    fixture.receive_bytes(first.size() + second.size());

    const auto first_result = reader.poll(fixture.connection);

    expect_complete(first_result, RespValue::integer(1), first.size());

    check(fixture.connection.input() == second, "trailing frame was consumed");

    // A second explicit call extracts the remainder.
    // Automatic pipelining belongs to Phase 2E.
    const auto second_result = reader.poll(fixture.connection);

    expect_complete(second_result, RespValue::bulk_string("abc"), second.size());

    check(fixture.connection.input().empty(), "second frame was not consumed");
}

void test_invalid_frame_retention() {
    Fixture fixture;
    const RespFramedInput reader;

    const std::string malformed = ":oops\r\n";

    const auto result = fixture.deliver(reader, malformed);

    expect_invalid(result);

    check(fixture.connection.input() == malformed, "invalid input was unexpectedly consumed");

    // The caller decides whether to close the
    // connection. The adapter does not do so.
    check(fixture.connection.is_open(), "adapter closed a connection");
}

void test_partial_frame_then_eof() {
    Fixture fixture;
    const RespFramedInput reader;

    const std::string fragment = "$4\r\nab";

    expect_need_more(fixture.deliver(reader, fragment));

    fixture.sockets.second.reset();

    const auto eof = fixture.connection.receive_once(128);

    check(eof.peer_closed, "stream EOF not detected");

    check(fixture.connection.received_eof(), "connection EOF state missing");

    check(fixture.connection.input() == fragment, "partial EOF buffer was corrupted");

    expect_need_more(reader.poll(fixture.connection));

    // The owning ConnectionManager performs
    // cleanup after EOF. The adapter does not
    // synthesize a completed RESP2 value.
}

void test_exact_buffer_limit() {
    Fixture fixture(Connection::Limits{32, 64});

    const RespFramedInput reader(RespParser::Limits{32, 32, 64, 8, 16});

    const std::string payload(25, 'x');

    const std::string frame = "$25\r\n" + payload + "\r\n";

    check(frame.size() == 32, "test frame length is not 32");

    const auto result = fixture.deliver(reader, frame);

    expect_complete(result, RespValue::bulk_string(payload), 32);

    check(fixture.connection.input().empty(), "exact-limit frame not consumed");

    check(fixture.connection.is_open(), "valid exact-limit frame closed socket");
}

void test_declared_length_above_limit() {
    Fixture fixture(Connection::Limits{32, 64});

    const RespFramedInput reader(RespParser::Limits{32, 32, 64, 8, 16});

    const std::string declaration = "$26\r\n";

    expect_invalid(fixture.deliver(reader, declaration));

    check(fixture.connection.input() == declaration, "invalid declaration consumed input");
}

void test_independent_client_buffers() {
    Fixture first;
    Fixture second;

    const RespFramedInput reader;

    expect_need_more(first.deliver(reader, "+HEL"));

    expect_complete(second.deliver(reader, ":7\r\n"), RespValue::integer(7), 4);

    check(first.connection.input() == "+HEL", "first client input was modified");

    check(second.connection.input().empty(), "second client input remained");

    expect_complete(first.deliver(reader, "LO\r\n"), RespValue::simple_string("HELLO"), 8);

    check(first.connection.input().empty(), "first client final frame remained");
}

} // namespace

int main() {
    try {
        test_empty_buffer();
        test_simple_frame_byte_by_byte();
        test_binary_bulk_byte_by_byte();
        test_fragmented_nested_array();
        test_exact_single_frame_consumption();
        test_invalid_frame_retention();
        test_partial_frame_then_eof();
        test_exact_buffer_limit();
        test_declared_length_above_limit();
        test_independent_client_buffers();

        std::cout << "RESP2 framed input tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RESP2 framed input test failed: " << error.what() << '\n';

        return 1;
    }
}
