#include "redisdb/commands/ArityValidator.hpp"

#include <stdexcept>

namespace redisdb::commands {

ArityValidationResult validate_arity(const Command& command, const ArityRule& rule) {
    if (rule.group_size == 0 ||
        (rule.maximum_arguments.has_value() && *rule.maximum_arguments < rule.minimum_arguments)) {
        throw std::invalid_argument("invalid command argument-count rule");
    }

    const std::size_t count = command.arguments().size();
    if (count < rule.minimum_arguments) {
        return {ArityValidationError::TooFewArguments};
    }
    if (rule.maximum_arguments.has_value() && count > *rule.maximum_arguments) {
        return {ArityValidationError::TooManyArguments};
    }
    if ((count - rule.minimum_arguments) % rule.group_size != 0) {
        return {ArityValidationError::InvalidArgumentGroup};
    }
    return {ArityValidationError::None};
}

} // namespace redisdb::commands
