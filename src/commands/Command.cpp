#include "redisdb/commands/Command.hpp"

#include "redisdb/protocol/RespValue.hpp"

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace redisdb::commands {
namespace {

void uppercase_ascii(std::string& name) {
    for (char& byte : name) {
        if (byte >= 'a' && byte <= 'z') {
            byte = static_cast<char>(byte - ('a' - 'A'));
        }
    }
}

CommandDecodeResult failure(CommandDecodeError error) { return {std::nullopt, error}; }

} // namespace

Command::Command(std::string name, std::vector<std::string> arguments)
    : name_(std::move(name)), arguments_(std::move(arguments)) {}

CommandDecodeResult decode_command(const protocol::RespValue& request) {
    const auto* array = std::get_if<protocol::Array>(&request.storage());
    if (array == nullptr) {
        return failure(CommandDecodeError::NotArray);
    }

    if (array->elements.empty()) {
        return failure(CommandDecodeError::EmptyArray);
    }

    const auto* verb = std::get_if<protocol::BulkString>(&array->elements.front().storage());
    if (verb == nullptr) {
        return failure(CommandDecodeError::VerbNotBulk);
    }
    if (verb->bytes.empty()) {
        return failure(CommandDecodeError::EmptyVerb);
    }

    std::vector<std::string> arguments;
    arguments.reserve(array->elements.size() - 1);

    for (std::size_t index = 1; index < array->elements.size(); ++index) {
        const auto* bulk = std::get_if<protocol::BulkString>(&array->elements[index].storage());
        if (bulk == nullptr) {
            return failure(CommandDecodeError::NonBulkArgument);
        }
        arguments.push_back(bulk->bytes);
    }

    std::string name = verb->bytes;
    uppercase_ascii(name);
    return {Command(std::move(name), std::move(arguments)), CommandDecodeError::None};
}

} // namespace redisdb::commands
