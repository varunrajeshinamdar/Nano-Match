#pragma once

#include <charconv>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace nanomatch::tools {

/// Parses `--flag value` pairs. Usage errors throw std::invalid_argument, which tools report along
/// with their usage text.
class Flags {
  public:
    Flags(std::span<const char* const> arguments, std::initializer_list<std::string_view> known) {
        for (std::size_t i = 0; i < arguments.size(); i += 2) {
            const std::string_view flag = arguments[i];
            bool is_known = false;
            for (const std::string_view candidate : known) {
                is_known = is_known || candidate == flag;
            }
            if (!is_known) {
                throw std::invalid_argument{std::format("unknown option '{}'", flag)};
            }
            if (i + 1 >= arguments.size()) {
                throw std::invalid_argument{std::format("{}: missing value", flag)};
            }
            values_[flag] = arguments[i + 1];
        }
    }

    [[nodiscard]] std::optional<std::string_view> optional(std::string_view flag) const {
        const auto found = values_.find(flag);
        return found == values_.end() ? std::nullopt
                                      : std::optional<std::string_view>{found->second};
    }

    [[nodiscard]] std::string_view required(std::string_view flag) const {
        const std::optional<std::string_view> value = optional(flag);
        if (!value) {
            throw std::invalid_argument{std::format("{} is required", flag)};
        }
        return *value;
    }

    template <typename Integer>
    [[nodiscard]] Integer required_integer(std::string_view flag) const {
        const std::string_view text = required(flag);
        Integer value{};
        const char* const end = text.data() + text.size();
        const auto [parsed_to, error] = std::from_chars(text.data(), end, value);
        if (error != std::errc{} || parsed_to != end) {
            throw std::invalid_argument{std::format("{}: invalid value '{}'", flag, text)};
        }
        return value;
    }

  private:
    std::map<std::string_view, std::string_view> values_;
};

}  // namespace nanomatch::tools
