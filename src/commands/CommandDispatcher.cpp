#include "redisdb/commands/CommandDispatcher.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace redisdb::commands {
namespace {

std::string canonical_registration_name(std::string name) {
    if (name.empty()) {
        throw std::invalid_argument("command registration name must not be empty");
    }
    for (char& byte : name) {
        const auto value = static_cast<unsigned char>(byte);
        if (value < 0x21U || value > 0x7eU) {
            throw std::invalid_argument("command registration name must be printable ASCII");
        }
        if (byte >= 'a' && byte <= 'z') {
            byte = static_cast<char>(byte - ('a' - 'A'));
        }
    }
    return name;
}

} // namespace

CommandDispatcher::CommandDispatcher(std::vector<CommandRoute> routes) {
    registry_.reserve(routes.size());
    for (auto& route : routes) {
        std::string name = canonical_registration_name(std::move(route.name));
        if (!route.handler) {
            throw std::invalid_argument("command registration handler must not be empty");
        }
        const bool inserted = registry_.emplace(std::move(name), std::move(route.handler)).second;
        if (!inserted) {
            throw std::invalid_argument("duplicate command registration");
        }
    }
}

DispatchResult CommandDispatcher::dispatch(const Command& command) const {
    const auto match = registry_.find(command.name());
    if (match == registry_.end()) {
        return {std::nullopt, DispatchError::UnknownCommand};
    }
    return {std::optional<HandlerResult>{match->second(command)}, DispatchError::None};
}

} // namespace redisdb::commands
