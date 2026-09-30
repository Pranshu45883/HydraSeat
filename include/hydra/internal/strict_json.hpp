#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace hydra::internal::json {

struct Number {
    std::string text;
};

struct Value {
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> value;
};

struct ParseOptions {
    unsigned maximumDepth{48u};
    std::size_t maximumNodes{65536u};
};

class ParseError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {

class Parser final {
public:
    Parser(std::string_view text, ParseOptions options)
        : text_(text), options_(options) {}

    Value parse() {
        skip();
        auto value = parseValue(0u);
        skip();
        if (position_ != text_.size()) syntax("trailing content");
        return value;
    }

private:
    [[noreturn]] void syntax(std::string message) const {
        throw ParseError(std::move(message) + " at byte " + std::to_string(position_));
    }

    void skip() {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) syntax("unexpected character");
    }

    void literal(std::string_view expected) {
        if (text_.substr(position_, expected.size()) != expected) {
            syntax("invalid literal");
        }
        position_ += expected.size();
    }

    std::uint32_t hex4() {
        std::uint32_t value = 0u;
        for (unsigned index = 0u; index < 4u; ++index) {
            if (position_ >= text_.size()) syntax("truncated unicode escape");
            const char ch = text_[position_++];
            value <<= 4u;
            if (ch >= '0' && ch <= '9') value += static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') value += static_cast<unsigned>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') value += static_cast<unsigned>(ch - 'A' + 10);
            else syntax("invalid unicode escape");
        }
        return value;
    }

    static void appendUtf8(std::string& output, std::uint32_t codePoint) {
        if (codePoint <= 0x7fu) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else if (codePoint <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        }
    }

    std::string parseString() {
        expect('"');
        std::string output;
        while (position_ < text_.size()) {
            const auto raw = static_cast<unsigned char>(text_[position_++]);
            if (raw == static_cast<unsigned char>('"')) return output;
            if (raw < 0x20u) syntax("control character in string");
            if (raw != static_cast<unsigned char>('\\')) {
                output.push_back(static_cast<char>(raw));
                continue;
            }
            if (position_ >= text_.size()) syntax("truncated escape");
            const char escape = text_[position_++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t codePoint = hex4();
                if (codePoint >= 0xd800u && codePoint <= 0xdbffu) {
                    if (position_ + 2u > text_.size() || text_[position_] != '\\' ||
                        text_[position_ + 1u] != 'u') {
                        syntax("missing low surrogate");
                    }
                    position_ += 2u;
                    const std::uint32_t low = hex4();
                    if (low < 0xdc00u || low > 0xdfffu) syntax("invalid low surrogate");
                    codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) +
                                (low - 0xdc00u);
                } else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
                    syntax("unexpected low surrogate");
                }
                appendUtf8(output, codePoint);
                break;
            }
            default: syntax("invalid escape");
            }
        }
        syntax("unterminated string");
    }

    Number parseNumber() {
        const std::size_t start = position_;
        (void)consume('-');
        if (consume('0')) {
            if (position_ < text_.size() &&
                std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
                syntax("leading zero");
            }
        } else {
            if (position_ >= text_.size() || text_[position_] < '1' ||
                text_[position_] > '9') {
                syntax("invalid number");
            }
            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
                ++position_;
            }
        }
        if (position_ < text_.size() &&
            (text_[position_] == '.' || text_[position_] == 'e' ||
             text_[position_] == 'E')) {
            syntax("integer expected");
        }
        return {std::string(text_.substr(start, position_ - start))};
    }

    Value parseValue(unsigned depth) {
        if (depth > options_.maximumDepth) syntax("nesting too deep");
        if (nodeCount_ >= options_.maximumNodes) syntax("JSON node count exceeds limit");
        ++nodeCount_;
        skip();
        if (position_ >= text_.size()) syntax("value expected");
        const char current = text_[position_];
        if (current == '"') return Value{parseString()};
        if (current == '{') {
            ++position_;
            Value::Object object;
            skip();
            if (consume('}')) return Value{std::move(object)};
            for (;;) {
                skip();
                if (position_ >= text_.size() || text_[position_] != '"') {
                    syntax("object key expected");
                }
                auto key = parseString();
                skip();
                expect(':');
                auto value = parseValue(depth + 1u);
                if (!object.emplace(std::move(key), std::move(value)).second) {
                    syntax("duplicate object key");
                }
                skip();
                if (consume('}')) break;
                expect(',');
            }
            return Value{std::move(object)};
        }
        if (current == '[') {
            ++position_;
            Value::Array array;
            skip();
            if (consume(']')) return Value{std::move(array)};
            for (;;) {
                array.push_back(parseValue(depth + 1u));
                skip();
                if (consume(']')) break;
                expect(',');
            }
            return Value{std::move(array)};
        }
        if (text_.compare(position_, 4u, "true") == 0) {
            literal("true");
            return Value{true};
        }
        if (text_.compare(position_, 5u, "false") == 0) {
            literal("false");
            return Value{false};
        }
        if (text_.compare(position_, 4u, "null") == 0) {
            literal("null");
            return Value{nullptr};
        }
        if (current == '-' || std::isdigit(static_cast<unsigned char>(current)) != 0) {
            return Value{parseNumber()};
        }
        syntax("invalid value");
    }

    std::string_view text_;
    ParseOptions options_;
    std::size_t position_{0u};
    std::size_t nodeCount_{0u};
};

} // namespace detail

inline Value parse(std::string_view text, ParseOptions options = {}) {
    return detail::Parser(text, options).parse();
}

} // namespace hydra::internal::json
