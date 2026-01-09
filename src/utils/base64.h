#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <span>

[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> data);

[[nodiscard]] std::string base64_encode(const std::vector<std::uint8_t>& data);

[[nodiscard]] std::vector<std::uint8_t> base64_decode(std::string_view encoded);