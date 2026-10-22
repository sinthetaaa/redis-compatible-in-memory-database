#pragma once

#include "redisdb/commands/Command.hpp"
#include "redisdb/protocol/RespValue.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace redisdb::commands {

// Metadata only: the dispatcher never opens, writes to, or closes sockets.
enum class ConnectionDisposition { KeepOpen, CloseAfterReply };

struct HandlerResult {
    protocol::RespValue reply;
    ConnectionDisposition disposition = ConnectionDisposition::KeepOpen;
};

using CommandHandler = std::function<HandlerResult(const Command&)>;

struct CommandRoute {
    std::string name;
    CommandHandler handler;
};

enum class DispatchError { None, UnknownCommand };

struct DispatchResult {
    std::optional<HandlerResult> handled;
    DispatchError error = DispatchError::None;
};

// Explicit immutable-after-construction routing table; no built-ins or I/O.
class CommandDispatcher final {
  public:
    explicit CommandDispatcher(std::vector<CommandRoute> routes);

    [[nodiscard]] DispatchResult dispatch(const Command& command) const;

  private:
    std::unordered_map<std::string, CommandHandler> registry_;
};

} // namespace redisdb::commands
