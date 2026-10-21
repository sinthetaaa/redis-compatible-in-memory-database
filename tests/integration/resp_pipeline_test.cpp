#include "redisdb/net/Connection.hpp"
#include "redisdb/net/UniqueFd.hpp"
#include "redisdb/protocol/RespFramedInput.hpp"
#include "redisdb/protocol/RespPipeline.hpp"

#include <fcntl.h>
#include <sys/socket.h>

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using redisdb::net::Connection;
using redisdb::net::UniqueFd;

using redisdb::protocol::ParseStatus;
using redisdb::protocol::PipelineStop;
using redisdb::protocol::RespFramedInput;
using redisdb::protocol::RespPipeline;
using redisdb::protocol::RespValue;

namespace {

void check(bool okay, const char* message) {
    if (!okay) {
        throw std::runtime_error(message);
    }
}

void nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);

    check(flags >= 0, "fcntl get failed");
    check(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "fcntl nonblocking failed");
}

struct SocketPair {
    UniqueFd server;
    UniqueFd client;

    SocketPair() {
        int fds[2] = {-1, -1};

        check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");

        server.reset(fds[0]);
        client.reset(fds[1]);

        nonblocking(server.get());
        nonblocking(client.get());
    }
};

struct Fixture {
    SocketPair sockets;
    Connection connection;

    explicit Fixture(Connection::Limits limits = {})
        : sockets(), connection(std::move(sockets.server), 101, limits) {}

    void deliver(std::string_view bytes) {
        check(!bytes.empty(), "empty test write");

        std::size_t sent = 0;

        while (sent < bytes.size()) {
            const auto n =
                ::send(sockets.client.get(), bytes.data() + sent, bytes.size() - sent, 0);

            check(n > 0, "test send failed");

            sent += static_cast<std::size_t>(n);
        }

        std::size_t received = 0;

        while (received < bytes.size()) {
            const auto r = connection.receive_once(bytes.size() - received);

            check(r.bytes_read > 0, "test receive failed");

            received += r.bytes_read;
        }
    }

    std::string flush(std::size_t count) {
        const auto sent = connection.flush_once(count);

        check(sent.bytes_written == count, "output flush count mismatch");

        std::string result(count, '\0');

        std::size_t offset = 0;

        while (offset < count) {
            const auto n = ::recv(sockets.client.get(), result.data() + offset, count - offset, 0);

            check(n > 0, "test response receive failed");

            offset += static_cast<std::size_t>(n);
        }

        return result;
    }
};

RespPipeline::Limits options(std::size_t frames, std::size_t max_reply) {
    RespPipeline::Limits limits;

    limits.max_frames_per_drain = frames;
    limits.max_reply_bytes = max_reply;

    return limits;
}

void test_peek_is_non_consuming() {
    Fixture fixture;
    RespFramedInput framed;

    fixture.deliver("+OK\r\n");

    const auto result = framed.peek(fixture.connection);

    check(result.status == ParseStatus::Complete, "peek did not decode");

    check(result.consumed_bytes == 5, "peek length mismatch");

    check(fixture.connection.input() == "+OK\r\n", "peek consumed input");

    const auto consumed = framed.poll(fixture.connection);

    check(consumed.status == ParseStatus::Complete, "original poll no longer works");

    check(fixture.connection.input().empty(), "poll did not consume frame");
}

void test_coalesced_fifo_replies() {
    Fixture fixture;
    RespPipeline pipeline;

    fixture.deliver(":1\r\n:2\r\n:3\r\n");

    std::size_t calls = 0;

    const auto result = pipeline.drain(fixture.connection, [&calls](const RespValue& request) {
        ++calls;
        return request;
    });

    check(result.stop == PipelineStop::NeedMore, "coalesced input did not drain");

    check(result.frames_processed == 3, "incorrect pipeline frame count");

    check(calls == 3, "reply callback count mismatch");

    check(result.bytes_consumed == 12, "coalesced input byte accounting mismatch");

    check(fixture.connection.input().empty(), "coalesced requests were not consumed");

    check(fixture.connection.pending_output_bytes() == 12, "incorrect queued reply length");

    check(fixture.flush(12) == ":1\r\n:2\r\n:3\r\n", "FIFO response ordering violated");
}

void test_fairness_budget() {
    Fixture fixture;
    RespPipeline pipeline(options(2, 16));

    fixture.deliver(":1\r\n:2\r\n:3\r\n:4\r\n:5\r\n");

    std::size_t calls = 0;

    const auto echo = [&calls](const RespValue& request) {
        ++calls;
        return request;
    };

    const auto first = pipeline.drain(fixture.connection, echo);

    check(first.stop == PipelineStop::BudgetExhausted, "first fairness budget not reached");

    check(first.frames_processed == 2 && fixture.connection.input() == ":3\r\n:4\r\n:5\r\n",
          "first fairness slice invalid");

    const auto second = pipeline.drain(fixture.connection, echo);

    check(second.stop == PipelineStop::BudgetExhausted, "second fairness budget not reached");

    check(second.frames_processed == 2 && fixture.connection.input() == ":5\r\n",
          "second fairness slice invalid");

    const auto third = pipeline.drain(fixture.connection, echo);

    check(third.stop == PipelineStop::NeedMore && third.frames_processed == 1,
          "final fairness slice incorrect");

    check(calls == 5, "fairness duplicated work");

    check(fixture.flush(20) == ":1\r\n:2\r\n:3\r\n:4\r\n:5\r\n", "fairness reordered replies");
}

void test_fragmentation_and_resume() {
    Fixture fixture;
    RespPipeline pipeline;

    const auto echo = [](const RespValue& request) { return request; };

    fixture.deliver("+HEL");

    const auto first = pipeline.drain(fixture.connection, echo);

    check(first.stop == PipelineStop::NeedMore && first.frames_processed == 0,
          "incomplete scalar was consumed");

    fixture.deliver("LO\r\n:2\r\n$4\r\nab");

    const auto second = pipeline.drain(fixture.connection, echo);

    check(second.stop == PipelineStop::NeedMore && second.frames_processed == 2,
          "mixed incomplete batch handled incorrectly");

    check(fixture.connection.input() == "$4\r\nab", "partial bulk payload was lost");

    fixture.deliver("cd\r\n");

    const auto third = pipeline.drain(fixture.connection, echo);

    check(third.stop == PipelineStop::NeedMore && third.frames_processed == 1,
          "fragmented final bulk did not complete");

    const std::string expected = "+HELLO\r\n:2\r\n$4\r\nabcd\r\n";

    check(fixture.flush(expected.size()) == expected, "fragmented pipeline replies not ordered");
}

void test_binary_payload() {
    Fixture fixture;
    RespPipeline pipeline;

    std::string binary("\0\r\n", 3);
    binary.push_back(static_cast<char>(0xff));

    std::string frame("$4\r\n");
    frame.append(binary);
    frame.append("\r\n");

    fixture.deliver(frame);

    const auto result =
        pipeline.drain(fixture.connection, [](const RespValue& request) { return request; });

    check(result.frames_processed == 1 && result.stop == PipelineStop::NeedMore,
          "binary frame not processed");

    check(fixture.flush(frame.size()) == frame, "binary pipeline reply corrupted");
}

void test_output_backpressure_and_resume() {
    Fixture fixture(Connection::Limits{64 * 1024, 32});

    RespPipeline pipeline(options(16, 8));

    fixture.connection.queue_output(std::string(25, 'q'));

    fixture.deliver(":9\r\n");

    std::size_t calls = 0;

    const auto echo = [&calls](const RespValue& request) {
        ++calls;
        return request;
    };

    const auto blocked = pipeline.drain(fixture.connection, echo);

    check(blocked.stop == PipelineStop::Backpressured, "output pressure not recognized");

    check(blocked.frames_processed == 0 && blocked.bytes_consumed == 0 && calls == 0,
          "callback ran without output capacity");

    check(fixture.connection.input() == ":9\r\n", "blocked request was consumed");

    check(fixture.flush(25) == std::string(25, 'q'), "existing output changed while blocked");

    const auto resumed = pipeline.drain(fixture.connection, echo);

    check(resumed.stop == PipelineStop::NeedMore && resumed.frames_processed == 1 && calls == 1,
          "request not resumed exactly once");

    check(fixture.connection.input().empty(), "resumed request bytes remain");

    check(fixture.flush(4) == ":9\r\n", "resumed response incorrect");
}

void test_invalid_after_completed_request() {
    Fixture fixture;
    RespPipeline pipeline;

    fixture.deliver(":8\r\n:oops\r\n");

    std::size_t calls = 0;

    const auto result = pipeline.drain(fixture.connection, [&calls](const RespValue& request) {
        ++calls;
        return request;
    });

    check(result.stop == PipelineStop::Invalid && !result.error.empty(),
          "malformed pipelined frame not detected");

    check(result.frames_processed == 1 && calls == 1, "valid prefix not processed exactly once");

    check(fixture.connection.input() == ":oops\r\n", "malformed suffix not retained");

    check(fixture.flush(4) == ":8\r\n", "earlier valid reply lost or reordered");
}

void test_oversized_reply_is_not_consumed() {
    Fixture fixture;
    RespPipeline pipeline(options(16, 8));

    fixture.deliver(":3\r\n");

    bool threw = false;

    try {
        (void)pipeline.drain(fixture.connection, [](const RespValue&) {
            return RespValue::bulk_string(std::string(16, 'x'));
        });
    } catch (const std::length_error&) {
        threw = true;
    }

    check(threw, "oversized reply was accepted");

    check(fixture.connection.input() == ":3\r\n", "failed reply consumed request");

    check(fixture.connection.pending_output_bytes() == 0, "failed reply modified output");
}

void test_callback_failure_preserves_bytes() {
    Fixture fixture;
    RespPipeline pipeline;

    fixture.deliver(":5\r\n");

    bool threw = false;

    try {
        (void)pipeline.drain(fixture.connection, [](const RespValue&) -> RespValue {
            throw std::runtime_error("simulated reply failure");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }

    check(threw, "callback exception was swallowed");

    check(fixture.connection.input() == ":5\r\n", "failed callback consumed input");

    check(fixture.connection.pending_output_bytes() == 0, "failed callback queued output");
}

void test_capacity_configuration() {
    Fixture fixture(Connection::Limits{64 * 1024, 4});

    RespPipeline pipeline(options(16, 8));

    fixture.deliver(":1\r\n");

    bool threw = false;

    try {
        (void)pipeline.drain(fixture.connection, [](const RespValue& request) { return request; });
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    check(threw, "incompatible output cap accepted");

    check(fixture.connection.input() == ":1\r\n", "configuration error consumed input");
}

void test_independent_connections() {
    Fixture first;
    Fixture second;

    RespPipeline pipeline;

    const auto echo = [](const RespValue& request) { return request; };

    first.deliver("+HEL");

    check(pipeline.drain(first.connection, echo).stop == PipelineStop::NeedMore,
          "first client fragment not retained");

    second.deliver(":7\r\n");

    check(pipeline.drain(second.connection, echo).frames_processed == 1,
          "second client frame not processed");

    check(first.connection.input() == "+HEL", "first client input changed");

    first.deliver("LO\r\n");

    check(pipeline.drain(first.connection, echo).frames_processed == 1,
          "first client request not completed");

    check(first.flush(8) == "+HELLO\r\n", "first client reply incorrect");

    check(second.flush(4) == ":7\r\n", "second client reply incorrect");
}

void test_empty_input() {
    Fixture fixture;
    RespPipeline pipeline;

    const auto result =
        pipeline.drain(fixture.connection, [](const RespValue& request) { return request; });

    check(result.stop == PipelineStop::NeedMore && result.frames_processed == 0,
          "empty input produced a reply");
}

} // namespace

int main() {
    try {
        test_peek_is_non_consuming();
        test_coalesced_fifo_replies();
        test_fairness_budget();
        test_fragmentation_and_resume();
        test_binary_payload();
        test_output_backpressure_and_resume();
        test_invalid_after_completed_request();
        test_oversized_reply_is_not_consumed();
        test_callback_failure_preserves_bytes();
        test_capacity_configuration();
        test_independent_connections();
        test_empty_input();

        std::cout << "RESP2 pipeline integration tests passed.\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RESP2 pipeline integration failed: " << error.what() << '\n';

        return 1;
    }
}
