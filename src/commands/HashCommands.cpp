#include "redisdb/commands/HashCommands.hpp"

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

HandlerResult set(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{3, std::nullopt, 2}).valid()) {
        return bad_arity("hset");
    }
    const auto& args = cmd.arguments();
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i].size() > MaxStringValueBytes) {
            return {
                RespValue::error("ERR hash field or value exceeds the 8176-byte reply-safe limit")};
        }
    }
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve((args.size() - 1) / 2);
    for (std::size_t i = 1; i < args.size(); i += 2) {
        pairs.emplace_back(args[i], args[i + 1]);
    }
    return guarded([&]() -> HandlerResult {
        const auto added = db.hash_set(args[0], pairs);
        return {RespValue::integer(static_cast<std::int64_t>(added))};
    });
}

HandlerResult get(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{2, 2, 1}).valid()) {
        return bad_arity("hget");
    }
    return guarded([&]() -> HandlerResult {
        auto value = db.hash_get(cmd.arguments()[0], cmd.arguments()[1]);
        if (!value) {
            return {RespValue::null_bulk_string()};
        }
        if (value->size() > MaxStringValueBytes) {
            return {RespValue::error("ERR HGET value exceeds the 8176-byte reply-safe limit")};
        }
        return {RespValue::bulk_string(std::move(*value))};
    });
}

HandlerResult exists(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{2, 2, 1}).valid()) {
        return bad_arity("hexists");
    }
    return guarded([&]() -> HandlerResult {
        return {RespValue::integer(db.hash_exists(cmd.arguments()[0], cmd.arguments()[1]) ? 1 : 0)};
    });
}

HandlerResult length(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{1, 1, 1}).valid()) {
        return bad_arity("hlen");
    }
    return guarded([&]() -> HandlerResult {
        const auto count = db.hash_length(cmd.arguments()[0]);
        if (count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            return {RespValue::error("ERR hash length exceeds supported range")};
        }
        return {RespValue::integer(static_cast<std::int64_t>(count))};
    });
}

HandlerResult remove(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{2, std::nullopt, 1}).valid()) {
        return bad_arity("hdel");
    }
    const auto& args = cmd.arguments();
    const std::vector<std::string> fields(args.begin() + 1, args.end());
    return guarded([&]() -> HandlerResult {
        const auto removed = db.hash_delete(args[0], fields);
        return {RespValue::integer(static_cast<std::int64_t>(removed))};
    });
}

HandlerResult multi_get(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{2, std::nullopt, 1}).valid()) {
        return bad_arity("hmget");
    }
    const auto& args = cmd.arguments();
    const std::size_t count = args.size() - 1;
    return guarded([&]() -> HandlerResult {
        // Check the type even if the request's response cannot fit in a frame.
        (void)db.hash_length(args[0]);
        if (count > MaxReplyBytes / 5) {
            return {RespValue::error("ERR HMGET response exceeds the 8192-byte limit")};
        }
        std::size_t total = 3 + std::to_string(count).size();
        std::vector<RespValue> result;
        result.reserve(count);
        for (std::size_t i = 1; i < args.size(); ++i) {
            auto value = db.hash_get(args[0], args[i]);
            if (!value) {
                if (5 > MaxReplyBytes - total) {
                    return {RespValue::error("ERR HMGET response exceeds the 8192-byte limit")};
                }
                total += 5; // $-1\r\n
                result.push_back(RespValue::null_bulk_string());
                continue;
            }
            const auto bytes = value->size();
            const auto overhead = 5 + std::to_string(bytes).size();
            if (bytes > MaxStringValueBytes || overhead > MaxReplyBytes - total ||
                bytes > MaxReplyBytes - total - overhead) {
                return {RespValue::error("ERR HMGET response exceeds the 8192-byte limit")};
            }
            total += overhead + bytes;
            result.push_back(RespValue::bulk_string(std::move(*value)));
        }
        return {RespValue::array(std::move(result))};
    });
}

HandlerResult get_all(storage::Database& db, const Command& cmd) {
    if (!validate_arity(cmd, ArityRule{1, 1, 1}).valid()) {
        return bad_arity("hgetall");
    }
    return guarded([&]() -> HandlerResult {
        auto items = db.hash_items(cmd.arguments()[0], MaxReplyBytes);
        if (!items) {
            return {RespValue::error("ERR HGETALL response exceeds the 8192-byte limit")};
        }
        std::vector<RespValue> result;
        result.reserve(items->size() * 2);
        for (auto& [field, value] : *items) {
            result.push_back(RespValue::bulk_string(std::move(field)));
            result.push_back(RespValue::bulk_string(std::move(value)));
        }
        return {RespValue::array(std::move(result))};
    });
}

} // namespace

std::vector<CommandRoute> hash_routes(storage::Database& db) {
    return {
        {"HSET", [&db](const Command& c) { return set(db, c); }},
        {"HGET", [&db](const Command& c) { return get(db, c); }},
        {"HEXISTS", [&db](const Command& c) { return exists(db, c); }},
        {"HLEN", [&db](const Command& c) { return length(db, c); }},
        {"HDEL", [&db](const Command& c) { return remove(db, c); }},
        {"HMGET", [&db](const Command& c) { return multi_get(db, c); }},
        {"HGETALL", [&db](const Command& c) { return get_all(db, c); }},
    };
}

} // namespace redisdb::commands
