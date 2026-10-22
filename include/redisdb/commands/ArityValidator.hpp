#pragma once

#include "redisdb/commands/Command.hpp"

#include <cstddef>
#include <optional>

namespace redisdb::commands {

// A caller-selected count-only policy; no command table or handler is
// installed.
struct ArityRule {
    std::size_t minimum_arguments = 0;
    std::optional<std::size_t> maximum_arguments;
    std::size_t group_size = 1;
};

enum class ArityValidationError {
    None,
    TooFewArguments,
    TooManyArguments,
    InvalidArgumentGroup,
};

struct ArityValidationResult {
    ArityValidationError error = ArityValidationError::None;

    [[nodiscard]] bool valid() const noexcept { return error == ArityValidationError::None; }
};

// Pure count-only check. Invalid rule configuration throws invalid_argument;
// client count mismatches return typed errors without RESP2 wire output.
[[nodiscard]] ArityValidationResult validate_arity(const Command& command, const ArityRule& rule);

} // namespace redisdb::commands
