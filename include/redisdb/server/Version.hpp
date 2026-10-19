#pragma once

#include <string_view>

namespace redisdb::server {

[[nodiscard]] std::string_view version() noexcept;

} // namespace redisdb::server
