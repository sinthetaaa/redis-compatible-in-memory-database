#pragma once

#include <optional>
#include <string>
#include <vector>

namespace redisdb::protocol {
class RespValue;
}

namespace redisdb::commands {

struct CommandDecodeResult;

// An owned, binary-safe command request. No socket or RESP2 buffer is borrowed.
// Instances returned by decode_command always have a nonempty, ASCII-uppercase name.
class Command final {
  public:
    Command(const Command&) = default;
    Command(Command&&) noexcept = default;
    Command& operator=(const Command&) = default;
    Command& operator=(Command&&) noexcept = default;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<std::string>& arguments() const noexcept { return arguments_; }

    [[nodiscard]] bool operator==(const Command&) const = default;

  private:
    Command(std::string name, std::vector<std::string> arguments);

    std::string name_;
    std::vector<std::string> arguments_;

    friend CommandDecodeResult decode_command(const protocol::RespValue& request);
};

enum class CommandDecodeError {
    None,
    NotArray,
    EmptyArray,
    VerbNotBulk,
    EmptyVerb,
    NonBulkArgument,
};

struct CommandDecodeResult {
    std::optional<Command> command;
    CommandDecodeError error = CommandDecodeError::None;
};

// Structural conversion only: no dispatch, arity checks, replies or I/O.
// Client-shape failures are represented as typed results, not exceptions.
[[nodiscard]] CommandDecodeResult decode_command(const protocol::RespValue& request);

} // namespace redisdb::commands
