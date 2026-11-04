#pragma once

#include <concepts>
#include <format>
#include <string>
#include <string_view>

namespace nanomatch::tools {

/// Builds one JSON object. Tools only ever emit flat records of numbers and short strings, so this
/// is enough and avoids a dependency.
class JsonObject {
  public:
    JsonObject& add(std::string_view key, std::string_view value) {
        std::string escaped;
        for (const char character : value) {
            if (character == '"' || character == '\\') {
                escaped += '\\';
            }
            escaped += character;
        }
        return add_raw(key, std::format("\"{}\"", escaped));
    }

    JsonObject& add(std::string_view key, const char* value) {
        return add(key, std::string_view{value});
    }

    JsonObject& add(std::string_view key, bool value) {
        return add_raw(key, value ? "true" : "false");
    }

    template <typename Number>
        requires(std::integral<Number> || std::floating_point<Number>)
    JsonObject& add(std::string_view key, Number value) {
        return add_raw(key, std::format("{}", value));
    }

    JsonObject& add(std::string_view key, const JsonObject& value) {
        return add_raw(key, value.str());
    }

    [[nodiscard]] std::string str() const { return "{" + body_ + "}"; }

  private:
    JsonObject& add_raw(std::string_view key, std::string_view value) {
        if (!body_.empty()) {
            body_ += ", ";
        }
        body_ += std::format("\"{}\": {}", key, value);
        return *this;
    }

    std::string body_;
};

}  // namespace nanomatch::tools
