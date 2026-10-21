#include "redisdb/protocol/RespPipeline.hpp"

#include "redisdb/net/Connection.hpp"

#include <stdexcept>
#include <utility>

namespace redisdb::protocol {

RespPipeline::RespPipeline() : RespPipeline(Limits{}) {}

RespPipeline::RespPipeline(Limits limits)
    : limits_(limits), framed_(limits_.parser),
      serializer_(RespSerializer::Limits{limits_.max_reply_bytes, 65536, 32}) {
    if (limits_.max_frames_per_drain == 0 || limits_.max_reply_bytes == 0) {
        throw std::invalid_argument("pipeline limits must be positive");
    }
}

PipelineResult RespPipeline::drain(net::Connection& connection,
                                   const PipelineReplyFactory& reply_factory) const {
    if (!reply_factory) {
        throw std::invalid_argument("pipeline requires a reply factory");
    }

    const std::size_t output_limit = connection.max_output_bytes();

    if (limits_.max_reply_bytes > output_limit) {
        throw std::invalid_argument("maximum pipeline reply exceeds connection output cap");
    }

    PipelineResult result{};

    for (std::size_t index = 0; index < limits_.max_frames_per_drain; ++index) {
        // Peek without consuming input. This permits
        // atomic request-to-output admission.
        ParseResult parsed = framed_.peek(connection);

        if (parsed.status == ParseStatus::NeedMore) {
            result.stop = PipelineStop::NeedMore;
            return result;
        }

        if (parsed.status == ParseStatus::Invalid) {
            result.stop = PipelineStop::Invalid;
            result.error = std::move(parsed.error);
            return result;
        }

        if (!parsed.value.has_value() || parsed.consumed_bytes == 0 ||
            parsed.consumed_bytes > connection.input().size()) {
            throw std::logic_error("RESP2 parser violated Complete contract");
        }

        const std::size_t pending = connection.pending_output_bytes();

        if (pending > output_limit) {
            throw std::logic_error("connection output exceeded its cap");
        }

        // Every possible serialized reply is at
        // most max_reply_bytes because serializer_
        // independently enforces this limit.
        //
        // Reserve that full capacity BEFORE invoking
        // the callback, rather than executing work
        // and discovering afterwards that its reply
        // cannot be queued.
        if (output_limit - pending < limits_.max_reply_bytes) {
            result.stop = PipelineStop::Backpressured;
            return result;
        }

        RespValue reply = reply_factory(*parsed.value);

        std::string encoded = serializer_.serialize(reply);

        // Output admission is guaranteed by the
        // reservation check above, provided nobody
        // mutates this Connection concurrently.
        connection.queue_output(encoded);

        // Commit request consumption only after
        // its response has been safely queued.
        connection.consume_input(parsed.consumed_bytes);

        ++result.frames_processed;
        result.bytes_consumed += parsed.consumed_bytes;
    }

    // More input may remain. The caller must arrange
    // a later drain even without a new socket read.
    result.stop = PipelineStop::BudgetExhausted;
    return result;
}

} // namespace redisdb::protocol
