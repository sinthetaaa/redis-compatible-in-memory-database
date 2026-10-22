#include "redisdb/commands/ArityValidator.hpp"
#include "redisdb/commands/CommandDispatcher.hpp"
#include "redisdb/protocol/RespValue.hpp"

#include <cstddef>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using redisdb::commands::ArityRule;
using redisdb::commands::ArityValidationError;
using redisdb::commands::ArityValidationResult;
using redisdb::commands::Command;
using redisdb::commands::CommandDispatcher;
using redisdb::commands::CommandRoute;
using redisdb::commands::decode_command;
using redisdb::commands::DispatchError;
using redisdb::commands::HandlerResult;
using redisdb::commands::validate_arity;
using redisdb::protocol::RespValue;

static_assert(std::is_nothrow_copy_constructible_v<ArityValidationResult>);

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Command make_command(std::string verb, std::vector<std::string> args = {}) {
    std::vector<RespValue> values;
    values.reserve(args.size() + 1);
    values.push_back(RespValue::bulk_string(std::move(verb)));
    for (auto& arg : args) {
        values.push_back(RespValue::bulk_string(std::move(arg)));
    }
    auto decoded = decode_command(RespValue::array(std::move(values)));
    check(decoded.error == redisdb::commands::CommandDecodeError::None &&
              decoded.command.has_value(),
          "fixture decoder failed");
    return std::move(*decoded.command);
}

Command with_count(std::size_t count, std::string verb = "TEST") {
    return make_command(std::move(verb), std::vector<std::string>(count, "value"));
}

void expect_error(const Command& command, const ArityRule& rule, ArityValidationError expected) {
    const ArityValidationResult result = validate_arity(command, rule);
    check(result.error == expected, "incorrect argument-count validation error");
    check(result.valid() == (expected == ArityValidationError::None),
          "valid() does not agree with the error code");
}

void expect_count(std::size_t count, const ArityRule& rule, ArityValidationError expected) {
    const Command command = with_count(count);
    expect_error(command, rule, expected);
}

void expect_invalid_rule(const Command& command, const ArityRule& rule) {
    bool invalid_argument_caught = false;
    try {
        (void)validate_arity(command, rule);
    } catch (const std::invalid_argument&) {
        invalid_argument_caught = true;
    }
    check(invalid_argument_caught, "invalid arity rule did not throw invalid_argument");
}

void test_exact_zero_and_one() {
    const ArityRule exactly_zero{0, 0, 1};
    expect_count(0, exactly_zero, ArityValidationError::None);
    expect_count(1, exactly_zero, ArityValidationError::TooManyArguments);
    const ArityRule exactly_one{1, 1, 1};
    expect_count(0, exactly_one, ArityValidationError::TooFewArguments);
    expect_count(1, exactly_one, ArityValidationError::None);
    expect_count(2, exactly_one, ArityValidationError::TooManyArguments);
}

void test_optional_range_and_lower_bounds() {
    const ArityRule zero_or_one{0, 1, 1};
    expect_count(0, zero_or_one, ArityValidationError::None);
    expect_count(1, zero_or_one, ArityValidationError::None);
    expect_count(2, zero_or_one, ArityValidationError::TooManyArguments);
    const ArityRule at_least_two{2, std::nullopt, 1};
    expect_count(0, at_least_two, ArityValidationError::TooFewArguments);
    expect_count(1, at_least_two, ArityValidationError::TooFewArguments);
    expect_count(2, at_least_two, ArityValidationError::None);
    expect_count(8, at_least_two, ArityValidationError::None);
    expect_count(0, ArityRule{}, ArityValidationError::None);
    expect_count(3, ArityRule{}, ArityValidationError::None);
}

void test_closed_range_and_equal_endpoints() {
    const ArityRule range{2, 4, 1};
    expect_count(1, range, ArityValidationError::TooFewArguments);
    expect_count(2, range, ArityValidationError::None);
    expect_count(3, range, ArityValidationError::None);
    expect_count(4, range, ArityValidationError::None);
    expect_count(5, range, ArityValidationError::TooManyArguments);
    const ArityRule exact_three_grouped{3, 3, 2};
    expect_count(2, exact_three_grouped, ArityValidationError::TooFewArguments);
    expect_count(3, exact_three_grouped, ArityValidationError::None);
    expect_count(4, exact_three_grouped, ArityValidationError::TooManyArguments);
}

void test_grouped_arguments() {
    const ArityRule pairs{2, std::nullopt, 2};
    for (std::size_t n : {0U, 1U}) {
        expect_count(n, pairs, ArityValidationError::TooFewArguments);
    }
    for (std::size_t n : {2U, 4U, 6U}) {
        expect_count(n, pairs, ArityValidationError::None);
    }
    for (std::size_t n : {3U, 5U}) {
        expect_count(n, pairs, ArityValidationError::InvalidArgumentGroup);
    }
    const ArityRule odd_after_one{1, 7, 2};
    for (std::size_t n : {1U, 3U, 5U, 7U}) {
        expect_count(n, odd_after_one, ArityValidationError::None);
    }
    for (std::size_t n : {2U, 4U, 6U}) {
        expect_count(n, odd_after_one, ArityValidationError::InvalidArgumentGroup);
    }
}

void test_precedence_over_group_alignment() {
    const ArityRule rule{2, 5, 2};
    expect_count(0, rule, ArityValidationError::TooFewArguments);
    expect_count(1, rule, ArityValidationError::TooFewArguments);
    expect_count(2, rule, ArityValidationError::None);
    expect_count(3, rule, ArityValidationError::InvalidArgumentGroup);
    expect_count(4, rule, ArityValidationError::None);
    expect_count(5, rule, ArityValidationError::InvalidArgumentGroup);
    expect_count(6, rule, ArityValidationError::TooManyArguments);
    expect_count(7, rule, ArityValidationError::TooManyArguments);
}

void test_invalid_configuration_precedes_client_errors() {
    for (std::size_t n : {0U, 2U, 6U}) {
        const Command command = with_count(n);
        expect_invalid_rule(command, ArityRule{0, std::nullopt, 0});
        expect_invalid_rule(command, ArityRule{2, 1, 1});
        expect_invalid_rule(command, ArityRule{4, 2, 0});
    }
}

void test_size_t_extremes_without_allocating_huge_arrays() {
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    const Command zero = with_count(0);
    const Command one = with_count(1);
    expect_error(zero, ArityRule{max, max, 1}, ArityValidationError::TooFewArguments);
    expect_error(one, ArityRule{max, std::nullopt, max}, ArityValidationError::TooFewArguments);
    expect_error(zero, ArityRule{0, max, max}, ArityValidationError::None);
    expect_error(one, ArityRule{0, max, max}, ArityValidationError::InvalidArgumentGroup);
    expect_error(zero, ArityRule{0, 0, max}, ArityValidationError::None);
    expect_error(one, ArityRule{0, 0, max}, ArityValidationError::TooManyArguments);
    expect_error(one, ArityRule{1, max, max}, ArityValidationError::None);
    expect_invalid_rule(one, ArityRule{1, 0, max});
}

void test_binary_and_empty_arguments_are_opaque() {
    std::string binary("\0\r\n", 3);
    binary.push_back(static_cast<char>(0x80));
    binary.push_back(static_cast<char>(0xff));
    const std::vector<std::string> original{"", binary, "repeat", "repeat"};
    const Command command = make_command("MiXeD", original);
    const Command copy = command;
    const std::string* data = command.arguments().data();
    for (std::size_t n = 0; n < 50; ++n) {
        expect_error(command, ArityRule{4, 4, 1}, ArityValidationError::None);
        expect_error(command, ArityRule{3, 5, 2}, ArityValidationError::InvalidArgumentGroup);
    }
    check(command == copy, "arity validator modified command or argument data");
    check(command.arguments() == original, "arity validator changed binary arguments");
    check(command.arguments().data() == data, "arity validator moved argument storage");
}

void test_name_and_decoder_are_independent() {
    const std::string high_verb = std::string("a\xff\0z", 4);
    const Command unusual = make_command(high_verb, {""});
    expect_error(unusual, ArityRule{1, 1, 1}, ArityValidationError::None);
    const Command conventional = make_command("PING", {""});
    expect_error(conventional, ArityRule{1, 1, 1}, ArityValidationError::None);
    check(unusual.name() != conventional.name(), "fixture names must differ");
    const auto bad = decode_command(RespValue::array({}));
    check(!bad.command.has_value() &&
              bad.error == redisdb::commands::CommandDecodeError::EmptyArray,
          "structural decode error changed");
}

void test_dispatcher_remains_unmodified() {
    const Command command = with_count(1, "MISSING");
    const CommandDispatcher dispatcher({CommandRoute{
        "KNOWN", [](const Command&) { return HandlerResult{RespValue::integer(1)}; }}});
    const auto result = dispatcher.dispatch(command);
    check(result.error == DispatchError::UnknownCommand && !result.handled.has_value(),
          "dispatcher unknown semantics changed");
    expect_error(command, ArityRule{2, 2, 1}, ArityValidationError::TooFewArguments);
}

void test_reentrancy_and_purity() {
    const Command command = with_count(5);
    const ArityRule rule{1, 9, 2};
    for (int i = 0; i < 1000; ++i) {
        expect_error(command, rule, ArityValidationError::None);
    }
    expect_error(command, ArityRule{1, 9, 3}, ArityValidationError::InvalidArgumentGroup);
    expect_error(command, rule, ArityValidationError::None);
}

} // namespace

int main() {
    try {
        test_exact_zero_and_one();
        test_optional_range_and_lower_bounds();
        test_closed_range_and_equal_endpoints();
        test_grouped_arguments();
        test_precedence_over_group_alignment();
        test_invalid_configuration_precedes_client_errors();
        test_size_t_extremes_without_allocating_huge_arrays();
        test_binary_and_empty_arguments_are_opaque();
        test_name_and_decoder_are_independent();
        test_dispatcher_remains_unmodified();
        test_reentrancy_and_purity();
        std::cout << "PASS: pure arity validator count/range/group policy and "
                     "isolation tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
