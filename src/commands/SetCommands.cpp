#include "redisdb/commands/SetCommands.hpp"

#include "redisdb/commands/ArityValidator.hpp"
#include "redisdb/commands/StringCommands.hpp"
#include "redisdb/storage/Database.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace redisdb::commands {
namespace {
using protocol::RespValue;
constexpr std::size_t MaxReplyBytes = 8192;

template <typename Action> HandlerResult guarded(Action&& action) {
    try {
        return action();
    } catch (const storage::WrongTypeError& error) {
        return {RespValue::error(error.what())};
    }
}

HandlerResult bad_arity(const char* name) {
    return {
        RespValue::error(std::string("ERR wrong number of arguments for '") + name + "' command")};
}

HandlerResult add(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{2, std::nullopt, 1}).valid()) {
        return bad_arity("sadd");
    }
    const auto& args = cmd.arguments();
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i].size() > MaxStringValueBytes) {
            return {RespValue::error("ERR set member exceeds the 8176-byte reply-safe limit")};
        }
    }
    const std::vector<std::string> members(args.begin() + 1, args.end());
    return guarded([&]() -> HandlerResult {
        const auto before = db.set_cardinality(args[0]);
        const auto max = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
        if (before > max || members.size() > max - before) {
            return {RespValue::error("ERR set cardinality exceeds supported range")};
        }
        const auto added = db.set_add(args[0], members);
        return {RespValue::integer(static_cast<std::int64_t>(added))};
    });
}

HandlerResult remove(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{2, std::nullopt, 1}).valid()) {
        return bad_arity("srem");
    }
    const auto& args = cmd.arguments();
    const std::vector<std::string> members(args.begin() + 1, args.end());
    return guarded([&]() -> HandlerResult {
        const auto count = db.set_cardinality(args[0]);
        if (count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            return {RespValue::error("ERR set cardinality exceeds supported range")};
        }
        return {RespValue::integer(static_cast<std::int64_t>(db.set_remove(args[0], members)))};
    });
}

HandlerResult is_member(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{2, 2, 1}).valid()) {
        return bad_arity("sismember");
    }
    return guarded([&]() -> HandlerResult {
        return {
            RespValue::integer(db.set_contains(cmd.arguments()[0], cmd.arguments()[1]) ? 1 : 0)};
    });
}

HandlerResult cardinality(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{1, 1, 1}).valid()) {
        return bad_arity("scard");
    }
    return guarded([&]() -> HandlerResult {
        const auto count = db.set_cardinality(cmd.arguments()[0]);
        if (count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            return {RespValue::error("ERR set cardinality exceeds supported range")};
        }
        return {RespValue::integer(static_cast<std::int64_t>(count))};
    });
}

HandlerResult members(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{1, 1, 1}).valid()) {
        return bad_arity("smembers");
    }
    return guarded([&]() -> HandlerResult {
        auto items = db.set_members(cmd.arguments()[0], MaxReplyBytes);
        if (!items) {
            return {RespValue::error("ERR SMEMBERS response exceeds the 8192-byte limit")};
        }
        std::vector<RespValue> reply;
        reply.reserve(items->size());
        for (auto& member : *items) {
            reply.push_back(RespValue::bulk_string(std::move(member)));
        }
        return {RespValue::array(std::move(reply))};
    });
}

HandlerResult multi_is_member(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{2, std::nullopt, 1}).valid()) {
        return bad_arity("smismember");
    }
    const auto& args = cmd.arguments();
    const std::size_t count = args.size() - 1;
    return guarded([&]() -> HandlerResult {
        // Verify the type before considering a too-large output array.
        (void)db.set_cardinality(args[0]);
        const std::size_t header_bytes = 3 + std::to_string(count).size();
        if (header_bytes > MaxReplyBytes || count > (MaxReplyBytes - header_bytes) / 4) {
            return {RespValue::error("ERR SMISMEMBER response exceeds the 8192-byte limit")};
        }
        std::vector<RespValue> reply;
        reply.reserve(count);
        for (std::size_t i = 1; i < args.size(); ++i) {
            reply.push_back(RespValue::integer(db.set_contains(args[0], args[i]) ? 1 : 0));
        }
        return {RespValue::array(std::move(reply))};
    });
}
} // namespace

std::vector<CommandRoute> set_routes(storage::Database& db) {
    return {
        {"SADD", [&db](const Command& cmd) { return add(db, cmd); }},
        {"SREM", [&db](const Command& cmd) { return remove(db, cmd); }},
        {"SISMEMBER", [&db](const Command& cmd) { return is_member(db, cmd); }},
        {"SCARD", [&db](const Command& cmd) { return cardinality(db, cmd); }},
        {"SMEMBERS", [&db](const Command& cmd) { return members(db, cmd); }},
        {"SMISMEMBER", [&db](const Command& cmd) { return multi_is_member(db, cmd); }},
    };
}

} // namespace redisdb::commands
