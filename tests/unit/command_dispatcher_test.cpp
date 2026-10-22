#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/protocol/RespValue.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using redisdb::commands::Command;
using redisdb::commands::CommandDispatcher;
using redisdb::commands::CommandHandler;
using redisdb::commands::CommandRoute;
using redisdb::commands::ConnectionDisposition;
using redisdb::commands::decode_command;
using redisdb::commands::DispatchError;
using redisdb::commands::DispatchResult;
using redisdb::commands::HandlerResult;
using redisdb::protocol::BulkString;
using redisdb::protocol::RespValue;

static_assert(std::is_copy_constructible_v<CommandDispatcher>);
static_assert(std::is_move_constructible_v<CommandDispatcher>);

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Command decode(std::string verb, std::vector<std::string> arguments = {}) {
    std::vector<RespValue> parts;
    parts.reserve(arguments.size() + 1);
    parts.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& value : arguments) {
        parts.push_back(RespValue::bulk_string(std::move(value)));
    }
    const auto decoded = decode_command(RespValue::array(std::move(parts)));
    check(decoded.command.has_value(), "fixture must decode into owned Command");
    return *decoded.command;
}

const HandlerResult& handled(const DispatchResult& result) {
    check(result.error == DispatchError::None, "handled routing returned wrong error");
    check(result.handled.has_value(), "handled routing omitted result");
    return *result.handled;
}

void unknown(const DispatchResult& result) {
    check(result.error == DispatchError::UnknownCommand, "unknown routing returned wrong error");
    check(!result.handled.has_value(), "unknown routing fabricated a response");
}

const std::string& bulk(const HandlerResult& result) {
    const auto* value = std::get_if<BulkString>(&result.reply.storage());
    check(value != nullptr, "handler response must be a bulk string");
    return value->bytes;
}

CommandHandler stub(std::string text, int& calls) {
    return [text = std::move(text), &calls](const Command&) {
        ++calls;
        return HandlerResult{RespValue::bulk_string(text)};
    };
}

void test_empty_registry() {
    const CommandDispatcher dispatcher({});
    const Command command = decode("anything");
    for (int i = 0; i < 3; ++i) {
        unknown(dispatcher.dispatch(command));
    }
}

void test_ascii_canonical_routing_once() {
    int calls = 0;
    const Command command = decode("pInG");
    check(command.name() == "PING", "decoder must normalize verb before dispatch");
    const Command* received = nullptr;
    const CommandDispatcher dispatcher({CommandRoute{"ping", [&calls, &received](const Command& c) {
                                                         ++calls;
                                                         received = &c;
                                                         return HandlerResult{
                                                             RespValue::integer(17)};
                                                     }}});
    const auto result = dispatcher.dispatch(command);
    check(handled(result).reply == RespValue::integer(17), "stub reply was changed");
    check(calls == 1, "matching handler not called exactly once");
    check(received == &command, "dispatcher copied or replaced command reference");
}

void test_multiple_independent_routes() {
    for (bool reverse : {false, true}) {
        int first = 0;
        int second = 0;
        std::vector<CommandRoute> routes;
        if (reverse) {
            routes.push_back({"second", stub("two", second)});
            routes.push_back({"first", stub("one", first)});
        } else {
            routes.push_back({"first", stub("one", first)});
            routes.push_back({"second", stub("two", second)});
        }
        const CommandDispatcher dispatcher(std::move(routes));
        const auto result = dispatcher.dispatch(decode("FIRST"));
        check(bulk(handled(result)) == "one", "wrong route selected");
        check(first == 1 && second == 0, "unmatched route executed");
        const auto second_result = dispatcher.dispatch(decode("SECOND"));
        check(bulk(handled(second_result)) == "two", "second route did not execute");
        check(first == 1 && second == 1, "routing depended on insertion order");
    }
}

void test_binary_arguments_unmodified() {
    std::string binary("\0\r\n", 3);
    binary.push_back(static_cast<char>(0x80));
    binary.push_back(static_cast<char>(0xff));
    const std::vector<std::string> expected{"", binary, "same", "same"};
    const Command command = decode("bInArY", expected);
    int calls = 0;
    const CommandDispatcher dispatcher(
        {CommandRoute{"BINARY", [&](const Command& value) {
                          ++calls;
                          check(&value == &command, "dispatcher passed a copied command");
                          check(value.arguments() == expected, "binary arguments changed");
                          check(value.arguments().data() == command.arguments().data(),
                                "argument array was copied");
                          return HandlerResult{RespValue::bulk_string(value.arguments()[1])};
                      }}});
    const auto result = dispatcher.dispatch(command);
    check(bulk(handled(result)) == binary, "binary handler reply changed");
    check(calls == 1, "binary handler called wrong number of times");
    check(command.arguments() == expected, "input arguments were mutated");
}

void test_owned_handler_reply() {
    int calls = 0;
    std::string original("ab\0cd", 5);
    original.push_back(static_cast<char>(0xff));
    const CommandDispatcher dispatcher(
        {CommandRoute{"own", [&](const Command&) {
                          ++calls;
                          std::string temporary = original;
                          return HandlerResult{RespValue::bulk_string(std::move(temporary))};
                      }}});
    const auto reply = dispatcher.dispatch(decode("OWN"));
    check(bulk(handled(reply)) == original, "owned reply did not survive temporary destruction");
    original.assign("overwritten");
    check(bulk(handled(reply)) == std::string("ab\0cd\xff", 6),
          "reply incorrectly borrowed external temporary storage");
    check(calls == 1, "owned reply handler was repeated");
}

void test_disposition_forwarding() {
    const CommandDispatcher dispatcher({
        {"KEEP", [](const Command&) { return HandlerResult{RespValue::integer(1)}; }},
        {"CLOSE",
         [](const Command&) {
             return HandlerResult{RespValue::integer(2), ConnectionDisposition::CloseAfterReply};
         }},
    });
    check(handled(dispatcher.dispatch(decode("KEEP"))).disposition ==
              ConnectionDisposition::KeepOpen,
          "default KeepOpen disposition changed");
    check(handled(dispatcher.dispatch(decode("CLOSE"))).disposition ==
              ConnectionDisposition::CloseAfterReply,
          "CloseAfterReply metadata was ignored");
}

void test_unusual_unknown_command_verbs() {
    int calls = 0;
    const CommandDispatcher dispatcher({CommandRoute{"known", stub("matched", calls)}});
    std::string high("K", 1);
    high.push_back(static_cast<char>(0xff));
    for (const auto& verb :
         std::vector<std::string>{"UNKNOWN", "WITH SPACE", "\r\n", high, std::string("N\0UL", 4)}) {
        const Command command = decode(verb);
        unknown(dispatcher.dispatch(command));
    }
    check(calls == 0, "unknown input invoked a registered handler");
}

void expect_invalid(std::vector<CommandRoute> routes) {
    bool threw = false;
    try {
        const CommandDispatcher ignored(std::move(routes));
        (void)ignored;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "invalid route configuration was accepted");
}

void test_printable_punctuation_registration() {
    int calls = 0;
    const CommandDispatcher dispatcher({CommandRoute{"a-b:c!", stub("punctuation", calls)}});
    const auto result = dispatcher.dispatch(decode("A-b:C!"));
    check(bulk(handled(result)) == "punctuation", "printable ASCII punctuation was rejected");
    check(calls == 1, "punctuation route handler was not called once");
}

void test_invalid_registry_names_and_empty_handlers() {
    int calls = 0;
    for (const auto& name : std::vector<std::string>{
             "", "bad name", " bad", "bad ", "a\tb", "a\rb", "a\nb", "\x7f",
             std::string(1, static_cast<char>(0x80)), std::string(1, static_cast<char>(0xff))}) {
        expect_invalid({CommandRoute{name, stub("unused", calls)}});
    }
    expect_invalid({CommandRoute{"valid", CommandHandler{}}});
    check(calls == 0, "invalid registry executed a handler");
}

void test_duplicate_names() {
    int calls = 0;
    expect_invalid(
        {CommandRoute{"dupe", stub("a", calls)}, CommandRoute{"dupe", stub("b", calls)}});
    expect_invalid(
        {CommandRoute{"miXeD", stub("a", calls)}, CommandRoute{"MIXED", stub("b", calls)}});
    check(calls == 0, "duplicate handler was executed during construction");
}

void test_exception_propagation_without_retry() {
    int throws = 0;
    const CommandDispatcher dispatcher({CommandRoute{"throw", [&](const Command&) -> HandlerResult {
                                                         ++throws;
                                                         throw std::runtime_error(
                                                             "intentional handler failure");
                                                     }}});
    for (int i = 1; i <= 2; ++i) {
        bool caught = false;
        try {
            (void)dispatcher.dispatch(decode("THROW"));
        } catch (const std::runtime_error& e) {
            caught = std::string_view(e.what()) == "intentional handler failure";
        }
        check(caught && throws == i, "handler exception suppressed, converted or retried");
    }
    unknown(dispatcher.dispatch(decode("NOT_REGISTERED")));
    check(throws == 2, "unknown dispatch retried failed handler");
}

void test_repeated_dispatch_has_no_hidden_state() {
    int alpha = 0;
    int beta = 0;
    const CommandDispatcher dispatcher(
        {CommandRoute{"a", stub("A", alpha)}, CommandRoute{"b", stub("B", beta)}});
    const Command command = decode("A", {"x", "x"});
    for (int i = 1; i <= 32; ++i) {
        check(bulk(handled(dispatcher.dispatch(command))) == "A", "routing changed across calls");
        check(alpha == i && beta == 0, "registry selected wrong handler");
        unknown(dispatcher.dispatch(decode("OTHER")));
    }
    check(command.arguments() == std::vector<std::string>({"x", "x"}),
          "repeated dispatch mutated original command");
}

} // namespace

int main() {
    try {
        test_empty_registry();
        test_ascii_canonical_routing_once();
        test_multiple_independent_routes();
        test_binary_arguments_unmodified();
        test_owned_handler_reply();
        test_disposition_forwarding();
        test_unusual_unknown_command_verbs();
        test_printable_punctuation_registration();
        test_invalid_registry_names_and_empty_handlers();
        test_duplicate_names();
        test_exception_propagation_without_retry();
        test_repeated_dispatch_has_no_hidden_state();
        std::cout << "PASS: isolated command dispatcher registration, binary routing and typed "
                     "outcomes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
