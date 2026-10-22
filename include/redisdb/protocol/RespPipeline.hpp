#pragma once

#include "redisdb/protocol/RespFramedInput.hpp"
#include "redisdb/protocol/RespSerializer.hpp"

#include <cstddef>
#include <functional>
#include <string>

namespace redisdb::net {
class Connection;
}

namespace redisdb::protocol {

// One reply per complete request.
//
// The reply factory is a protocol-level test hook,
// not a Redis command executor.
//
// Request bytes are consumed only after their
// corresponding serialized reply is queued.
//
// The caller must close the client if the factory,
// serializer, or output queue throws. Retrying after
// an exceptional factory outcome could repeat an
// external side effect.
using PipelineReplyFactory = std::function<RespValue(const RespValue&)>;

enum class PipelineStop { NeedMore, BudgetExhausted, Backpressured, Invalid, Closing };

struct PipelineResult {
    PipelineStop stop = PipelineStop::NeedMore;
    std::size_t frames_processed = 0;
    std::size_t bytes_consumed = 0;
    std::string error;
};

// Bounded, synchronous, FIFO pipeline processing.
//
// No sockets are read or written here.
// No independent input buffer is allocated.
//
// Native event-loop scheduling and resumption after
// output pressure belong to Phase 2E.1b.
class RespPipeline final {
  public:
    struct Limits {
        std::size_t max_frames_per_drain = 16;
        std::size_t max_reply_bytes = 8 * 1024;

        RespParser::Limits parser{64 * 1024, 64 * 1024, 65536, 32, 64 * 1024};
    };

    RespPipeline();

    explicit RespPipeline(Limits limits);

    [[nodiscard]] PipelineResult drain(net::Connection& connection,
                                       const PipelineReplyFactory& reply_factory) const;

  private:
    Limits limits_;
    RespFramedInput framed_;
    RespSerializer serializer_;
};

} // namespace redisdb::protocol
