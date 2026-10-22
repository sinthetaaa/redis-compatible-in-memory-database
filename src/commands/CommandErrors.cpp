#include "redisdb/commands/CommandErrors.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace redisdb::commands {

protocol::RespValue unknown_command_reply(const Command& command) {
    constexpr std::size_t MaxDisplayedBytes = 48;
    constexpr char Hex[] = "0123456789ABCDEF";

    const std::string& name = command.name();
    const std::size_t count = std::min(name.size(), MaxDisplayedBytes);

    std::string message = "ERR unknown command '";

    for (std::size_t index = 0; index < count; ++index) {
        const auto byte = static_cast<unsigned char>(name[index]);

        if (byte == '\\' || byte == '\'') {
            message.push_back('\\');
            message.push_back(static_cast<char>(byte));
        } else if (byte >= 0x20 && byte <= 0x7E) {
            message.push_back(static_cast<char>(byte));
        } else {
            message += "\\x";
            message.push_back(Hex[byte >> 4]);
            message.push_back(Hex[byte & 0x0F]);
        }
    }

    if (name.size() > MaxDisplayedBytes) {
        message += "...";
    }

    message += "'";

    return protocol::RespValue::error(std::move(message));
}

} // namespace redisdb::commands
