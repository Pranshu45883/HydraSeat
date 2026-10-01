#include "hydra/hidhide_session_backend.hpp"

#include "hydra/hardware_identity.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>

namespace hydra {
namespace {

constexpr std::size_t kMaximumDiagnosticBytes = 2048u;

std::string boundedDiagnostic(std::string value) {
    if (value.size() > kMaximumDiagnosticBytes) {
        value.resize(kMaximumDiagnosticBytes);
    }
    return value;
}

bool boundedText(std::wstring_view value) {
    return !value.empty() && value.size() <= kHidHideSessionMaxIdentifierChars &&
           value.find(L'\0') == std::wstring_view::npos;
}

std::wstring normalizeApplication(std::wstring_view value) {
    return hardware::normalizeDevicePath(value);
}

std::vector<std::wstring> normalizedUnique(
    const std::vector<std::wstring>& values,
    bool application) {
    std::vector<std::wstring> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        auto normalized = application
            ? normalizeApplication(value)
            : hardware::canonicalizeInstanceId(value);
        if (std::find(result.begin(), result.end(), normalized) == result.end()) {
            result.push_back(std::move(normalized));
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

void mergeUnique(std::vector<std::wstring>& destination,
                 const std::vector<std::wstring>& additions,
                 bool application) {
    destination = normalizedUnique(destination, application);
    const auto normalizedAdditions = normalizedUnique(additions, application);
    for (const auto& value : normalizedAdditions) {
        if (!std::binary_search(destination.begin(), destination.end(), value)) {
            destination.push_back(value);
            std::sort(destination.begin(), destination.end());
        }
    }
}

HidHideSessionSnapshot normalizedSnapshot(HidHideSessionSnapshot snapshot) {
    snapshot.blockedDeviceInstanceIds =
        normalizedUnique(snapshot.blockedDeviceInstanceIds, false);
    snapshot.allowedApplications =
        normalizedUnique(snapshot.allowedApplications, true);
    return snapshot;
}

bool sameSnapshot(const HidHideSessionSnapshot& lhs,
                  const HidHideSessionSnapshot& rhs) {
    return normalizedSnapshot(lhs) == normalizedSnapshot(rhs);
}

constexpr std::size_t kPhase3ManifestMaxBytes = 1024u * 1024u;
constexpr std::size_t kPhase3ProfileMaxBytes = 4u * 1024u * 1024u;
constexpr std::size_t kPhase3MetricsMaxBytes = 2u * 1024u * 1024u;
constexpr std::size_t kPhase3TraceMaxBytes = 512u * 1024u * 1024u;
constexpr std::size_t kPhase3TraceMaxLineBytes = 1024u * 1024u;
constexpr std::size_t kPhase3TraceMaxLines = 2'000'000u;
constexpr std::string_view kPhase3IsolationGuarantee =
    "diagnostic_route_only_native_os_input_not_suppressed";

struct EvidenceError final : std::runtime_error {
    EvidenceError(Phase3HardwareEvidenceStatus value, std::string message)
        : std::runtime_error(std::move(message)), status(value) {}
    Phase3HardwareEvidenceStatus status;
};

[[noreturn]] void evidenceFail(Phase3HardwareEvidenceStatus status,
                               std::string message) {
    throw EvidenceError(status, boundedDiagnostic(std::move(message)));
}

struct JsonNumber {
    std::string text;
};

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    std::variant<std::nullptr_t, bool, JsonNumber, std::string, Array, Object> value;
};

class StrictJsonParser final {
public:
    explicit StrictJsonParser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skipWhitespace();
        auto value = parseValue(0u);
        skipWhitespace();
        if (position_ != text_.size()) syntax("trailing content");
        return value;
    }

private:
    [[noreturn]] void syntax(std::string message) const {
        throw std::runtime_error(std::move(message) + " at byte " +
                                 std::to_string(position_));
    }

    void skipWhitespace() {
        while (position_ < text_.size()) {
            const char value = text_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
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

    std::uint32_t hex4() {
        std::uint32_t result = 0u;
        for (unsigned index = 0u; index < 4u; ++index) {
            if (position_ >= text_.size()) syntax("truncated unicode escape");
            const char ch = text_[position_++];
            result <<= 4u;
            if (ch >= '0' && ch <= '9') result += static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') result += static_cast<unsigned>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') result += static_cast<unsigned>(ch - 'A' + 10);
            else syntax("invalid unicode escape");
        }
        return result;
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

    JsonNumber parseNumber() {
        const std::size_t start = position_;
        (void)consume('-');
        if (consume('0')) {
            if (position_ < text_.size() && text_[position_] >= '0' &&
                text_[position_] <= '9') {
                syntax("leading zero");
            }
        } else {
            if (position_ >= text_.size() || text_[position_] < '1' ||
                text_[position_] > '9') {
                syntax("invalid number");
            }
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            if (position_ >= text_.size() || text_[position_] < '0' ||
                text_[position_] > '9') {
                syntax("invalid fraction");
            }
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < text_.size() &&
            (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() &&
                (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= text_.size() || text_[position_] < '0' ||
                text_[position_] > '9') {
                syntax("invalid exponent");
            }
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
        }
        return {std::string(text_.substr(start, position_ - start))};
    }

    JsonValue parseValue(unsigned depth) {
        if (depth > 64u) syntax("nesting too deep");
        if (++nodes_ > 100'000u) syntax("JSON node count exceeds limit");
        skipWhitespace();
        if (position_ >= text_.size()) syntax("value expected");
        const char current = text_[position_];
        if (current == '"') return JsonValue{parseString()};
        if (current == '{') {
            ++position_;
            JsonValue::Object object;
            skipWhitespace();
            if (consume('}')) return JsonValue{std::move(object)};
            for (;;) {
                skipWhitespace();
                if (position_ >= text_.size() || text_[position_] != '"') {
                    syntax("object key expected");
                }
                auto key = parseString();
                skipWhitespace();
                expect(':');
                auto value = parseValue(depth + 1u);
                if (!object.emplace(std::move(key), std::move(value)).second) {
                    syntax("duplicate object key");
                }
                skipWhitespace();
                if (consume('}')) break;
                expect(',');
            }
            return JsonValue{std::move(object)};
        }
        if (current == '[') {
            ++position_;
            JsonValue::Array array;
            skipWhitespace();
            if (consume(']')) return JsonValue{std::move(array)};
            for (;;) {
                array.push_back(parseValue(depth + 1u));
                skipWhitespace();
                if (consume(']')) break;
                expect(',');
            }
            return JsonValue{std::move(array)};
        }
        if (text_.compare(position_, 4u, "true") == 0) {
            literal("true");
            return JsonValue{true};
        }
        if (text_.compare(position_, 5u, "false") == 0) {
            literal("false");
            return JsonValue{false};
        }
        if (text_.compare(position_, 4u, "null") == 0) {
            literal("null");
            return JsonValue{nullptr};
        }
        if (current == '-' || (current >= '0' && current <= '9')) {
            return JsonValue{parseNumber()};
        }
        syntax("invalid value");
    }

    std::string_view text_;
    std::size_t position_{0u};
    std::size_t nodes_{0u};
};

const JsonValue::Object& jsonObject(const JsonValue& value, std::string_view label) {
    const auto* object = std::get_if<JsonValue::Object>(&value.value);
    if (object == nullptr) evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                                       std::string(label) + " must be an object");
    return *object;
}

const JsonValue::Array& jsonArray(const JsonValue& value, std::string_view label) {
    const auto* array = std::get_if<JsonValue::Array>(&value.value);
    if (array == nullptr) evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                                      std::string(label) + " must be an array");
    return *array;
}

const JsonValue& jsonRequired(const JsonValue::Object& object,
                              std::string_view key,
                              std::string_view label) {
    const auto found = object.find(std::string(key));
    if (found == object.end()) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " is missing required key " + std::string(key));
    }
    return found->second;
}

void jsonExactKeys(const JsonValue::Object& object,
                   std::initializer_list<std::string_view> keys,
                   std::string_view label) {
    std::set<std::string> expected;
    for (const auto key : keys) expected.emplace(key);
    if (object.size() != expected.size()) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " has unexpected or missing fields");
    }
    for (const auto& [key, _] : object) {
        if (!expected.contains(key)) {
            evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                         std::string(label) + " contains unexpected field " + key);
        }
    }
}

std::string jsonString(const JsonValue& value, std::string_view label,
                       std::size_t maxBytes = 32768u) {
    const auto* text = std::get_if<std::string>(&value.value);
    if (text == nullptr || text->empty() || text->size() > maxBytes ||
        text->find('\0') != std::string::npos) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " must be a nonempty bounded string");
    }
    return *text;
}

bool jsonBool(const JsonValue& value, std::string_view label) {
    const auto* boolean = std::get_if<bool>(&value.value);
    if (boolean == nullptr) evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                                        std::string(label) + " must be boolean");
    return *boolean;
}

bool jsonNull(const JsonValue& value) noexcept {
    return std::holds_alternative<std::nullptr_t>(value.value);
}

std::uint64_t jsonUint(const JsonValue& value, std::string_view label) {
    const auto* number = std::get_if<JsonNumber>(&value.value);
    if (number == nullptr || number->text.empty() || number->text.front() == '-' ||
        number->text.find_first_of(".eE") != std::string::npos) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " must be an unsigned integer");
    }
    std::uint64_t result = 0u;
    const auto parsed = std::from_chars(number->text.data(),
                                        number->text.data() + number->text.size(),
                                        result);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size()) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " is outside the uint64 range");
    }
    return result;
}

double jsonDouble(const JsonValue& value, std::string_view label) {
    const auto* number = std::get_if<JsonNumber>(&value.value);
    if (number == nullptr) evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                                       std::string(label) + " must be numeric");
    try {
        std::size_t consumed = 0u;
        const double result = std::stod(number->text, &consumed);
        if (consumed != number->text.size() || !std::isfinite(result)) throw std::runtime_error("invalid");
        return result;
    } catch (...) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " must be a finite number");
    }
}

std::wstring asciiWide(std::string_view value, std::string_view label) {
    if (value.empty() || value.size() > kHidHideSessionMaxIdentifierChars) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " must be nonempty and bounded");
    }
    std::wstring result;
    result.reserve(value.size());
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch > 0x7fu) {
            evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                         std::string(label) + " must use ASCII device identity characters");
        }
        result.push_back(static_cast<wchar_t>(ch));
    }
    return result;
}

bool validSha256(std::string_view value) noexcept {
    if (value.size() != 64u) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
               (ch >= 'A' && ch <= 'F');
    });
}

std::string normalizedSha256(std::string value) {
    if (!validSha256(value)) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     "P3-HW SHA-256 value must contain exactly 64 hexadecimal characters");
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch >= 'A' && ch <= 'F') return static_cast<char>(ch - 'A' + 'a');
        return static_cast<char>(ch);
    });
    return value;
}

class Sha256 final {
public:
    void update(const std::uint8_t* data, std::size_t size) {
        totalBytes_ += size;
        while (size != 0u) {
            const std::size_t take = std::min(size, buffer_.size() - bufferSize_);
            std::copy_n(data, take, buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_));
            bufferSize_ += take;
            data += take;
            size -= take;
            if (bufferSize_ == buffer_.size()) {
                transform(buffer_.data());
                bufferSize_ = 0u;
            }
        }
    }

    std::array<std::uint8_t, 32> finish() {
        const std::uint64_t bitLength = static_cast<std::uint64_t>(totalBytes_) * 8u;
        buffer_[bufferSize_++] = 0x80u;
        if (bufferSize_ > 56u) {
            while (bufferSize_ < 64u) buffer_[bufferSize_++] = 0u;
            transform(buffer_.data());
            bufferSize_ = 0u;
        }
        while (bufferSize_ < 56u) buffer_[bufferSize_++] = 0u;
        for (unsigned index = 0u; index < 8u; ++index) {
            buffer_[63u - index] = static_cast<std::uint8_t>(bitLength >> (index * 8u));
        }
        transform(buffer_.data());
        std::array<std::uint8_t, 32> digest{};
        for (std::size_t index = 0u; index < state_.size(); ++index) {
            digest[index * 4u] = static_cast<std::uint8_t>(state_[index] >> 24u);
            digest[index * 4u + 1u] = static_cast<std::uint8_t>(state_[index] >> 16u);
            digest[index * 4u + 2u] = static_cast<std::uint8_t>(state_[index] >> 8u);
            digest[index * 4u + 3u] = static_cast<std::uint8_t>(state_[index]);
        }
        return digest;
    }

private:
    static constexpr std::array<std::uint32_t, 64> kRound = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
    };

    static std::uint32_t rotateRight(std::uint32_t value, unsigned bits) noexcept {
        return (value >> bits) | (value << (32u - bits));
    }

    void transform(const std::uint8_t* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0u; index < 16u; ++index) {
            const std::size_t offset = index * 4u;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24u) |
                           (static_cast<std::uint32_t>(block[offset + 1u]) << 16u) |
                           (static_cast<std::uint32_t>(block[offset + 2u]) << 8u) |
                           static_cast<std::uint32_t>(block[offset + 3u]);
        }
        for (std::size_t index = 16u; index < 64u; ++index) {
            const auto s0 = rotateRight(words[index - 15u], 7u) ^
                            rotateRight(words[index - 15u], 18u) ^
                            (words[index - 15u] >> 3u);
            const auto s1 = rotateRight(words[index - 2u], 17u) ^
                            rotateRight(words[index - 2u], 19u) ^
                            (words[index - 2u] >> 10u);
            words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0u; index < 64u; ++index) {
            const auto s1 = rotateRight(e, 6u) ^ rotateRight(e, 11u) ^ rotateRight(e, 25u);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + choose + kRound[index] + words[index];
            const auto s0 = rotateRight(a, 2u) ^ rotateRight(a, 13u) ^ rotateRight(a, 22u);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u,
    };
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferSize_{0u};
    std::size_t totalBytes_{0u};
};

std::string digestHex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.reserve(64u);
    for (const auto value : digest) {
        output.push_back(kHex[value >> 4u]);
        output.push_back(kHex[value & 0x0fu]);
    }
    return output;
}

std::string sha256Bytes(std::string_view bytes) {
    Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return digestHex(hash.finish());
}

std::string readBoundedFile(const std::filesystem::path& path,
                            std::size_t maximumBytes,
                            std::string_view label) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        evidenceFail(Phase3HardwareEvidenceStatus::IoFailure,
                     std::string(label) + " file size query failed");
    }
    if (size > maximumBytes) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " exceeds the bounded evidence size");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        evidenceFail(Phase3HardwareEvidenceStatus::IoFailure,
                     std::string(label) + " could not be opened");
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty()) input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input.good() && !input.eof()) {
        evidenceFail(Phase3HardwareEvidenceStatus::IoFailure,
                     std::string(label) + " could not be read completely");
    }
    return bytes;
}

std::string sha256File(const std::filesystem::path& path,
                       std::size_t maximumBytes,
                       std::string_view label) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) evidenceFail(Phase3HardwareEvidenceStatus::Tampered,
                         std::string(label) + " is missing or unreadable");
    if (size > maximumBytes) evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                                         std::string(label) + " exceeds the bounded evidence size");
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) evidenceFail(Phase3HardwareEvidenceStatus::Tampered,
                                      std::string(label) + " could not be opened");
    Sha256 hash;
    std::array<char, 64u * 1024u> buffer{};
    std::size_t total = 0u;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            total += static_cast<std::size_t>(count);
            if (total > maximumBytes) evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                                                   std::string(label) + " exceeds the bounded evidence size");
            hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                        static_cast<std::size_t>(count));
        }
    }
    if (!input.eof()) evidenceFail(Phase3HardwareEvidenceStatus::Tampered,
                                   std::string(label) + " read failed");
    return digestHex(hash.finish());
}

std::uint64_t currentUnixSeconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0u;
}

std::string jsonStringAllowEmpty(const JsonValue& value,
                                 std::string_view label,
                                 std::size_t maxBytes = 32768u) {
    const auto* text = std::get_if<std::string>(&value.value);
    if (text == nullptr || text->size() > maxBytes ||
        text->find('\0') != std::string::npos) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " must be a bounded string");
    }
    return *text;
}

void jsonStringOrNull(const JsonValue& value, std::string_view label) {
    if (jsonNull(value)) return;
    (void)jsonString(value, label, 32768u);
}

std::filesystem::path utf8Path(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char raw : value) {
        encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(raw)));
    }
    return std::filesystem::path(encoded);
}

bool pathHasParentTraversal(const std::filesystem::path& path) {
    return std::any_of(path.begin(), path.end(), [](const auto& component) {
        return component == std::filesystem::path("..");
    });
}

bool pathStartsWith(const std::filesystem::path& path,
                    const std::filesystem::path& prefix) {
    auto pathIt = path.begin();
    for (auto prefixIt = prefix.begin(); prefixIt != prefix.end(); ++prefixIt, ++pathIt) {
        if (pathIt == path.end() || *pathIt != *prefixIt) return false;
    }
    return true;
}

std::filesystem::path resolveSessionArtifact(const std::filesystem::path& manifestPath,
                                             std::string_view relative,
                                             std::string_view label) {
    const auto candidate = utf8Path(relative);
    if (candidate.empty() || candidate.is_absolute() || pathHasParentTraversal(candidate)) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " must be a session-relative path without traversal");
    }
    std::error_code ec;
    const auto base = std::filesystem::weakly_canonical(manifestPath.parent_path(), ec);
    if (ec) evidenceFail(Phase3HardwareEvidenceStatus::IoFailure,
                         "P3-HW manifest directory could not be canonicalized");
    const auto resolved = std::filesystem::weakly_canonical(base / candidate, ec);
    if (ec || !pathStartsWith(resolved, base)) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(label) + " resolves outside the P3-HW session directory");
    }
    return resolved;
}

std::filesystem::path resolveProfilePath(const std::filesystem::path& manifestPath,
                                         std::string_view value) {
    auto candidate = utf8Path(value);
    if (!candidate.is_absolute()) candidate = manifestPath.parent_path() / candidate;
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) evidenceFail(Phase3HardwareEvidenceStatus::IoFailure,
                         "P3-HW profile path could not be canonicalized");
    return resolved;
}

void requireExpectedHash(const std::filesystem::path& path,
                         std::string_view expected,
                         std::size_t maximumBytes,
                         std::string_view label) {
    const auto observed = sha256File(path, maximumBytes, label);
    if (observed != normalizedSha256(std::string(expected))) {
        evidenceFail(Phase3HardwareEvidenceStatus::Tampered,
                     std::string(label) + " SHA-256 mismatch");
    }
}

bool asciiCasePrefix(std::string_view value, std::string_view prefix) noexcept {
    if (value.size() < prefix.size()) return false;
    for (std::size_t index = 0u; index < prefix.size(); ++index) {
        auto left = static_cast<unsigned char>(value[index]);
        auto right = static_cast<unsigned char>(prefix[index]);
        if (left >= 'A' && left <= 'Z') left = static_cast<unsigned char>(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = static_cast<unsigned char>(right - 'A' + 'a');
        if (left != right) return false;
    }
    return true;
}

struct OwnershipRow {
    std::string stableDeviceId;
    Phase3InputDeviceCategory category{Phase3InputDeviceCategory::Keyboard};
    std::uint32_t seatId{0u};

    bool operator==(const OwnershipRow&) const = default;
};

bool ownershipLess(const OwnershipRow& left, const OwnershipRow& right) {
    if (left.stableDeviceId != right.stableDeviceId) {
        return left.stableDeviceId < right.stableDeviceId;
    }
    if (left.category != right.category) {
        return static_cast<unsigned>(left.category) < static_cast<unsigned>(right.category);
    }
    return left.seatId < right.seatId;
}

bool nativeIdentityLess(const Phase3SeatDeviceIdentity& left,
                        const Phase3SeatDeviceIdentity& right) {
    if (left.stableDeviceId != right.stableDeviceId) return left.stableDeviceId < right.stableDeviceId;
    if (left.deviceInstanceId != right.deviceInstanceId) return left.deviceInstanceId < right.deviceInstanceId;
    if (left.category != right.category) {
        return static_cast<unsigned>(left.category) < static_cast<unsigned>(right.category);
    }
    return left.seatId < right.seatId;
}

Phase3InputDeviceCategory parseCategory(std::string_view value,
                                        std::string_view label) {
    if (value == "keyboard") return Phase3InputDeviceCategory::Keyboard;
    if (value == "mouse") return Phase3InputDeviceCategory::Mouse;
    evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                 std::string(label) + " must be keyboard or mouse");
}

std::string categoryPrefix(Phase3InputDeviceCategory category) {
    return category == Phase3InputDeviceCategory::Keyboard ? "Keyboard:" : "Mouse:";
}

std::vector<OwnershipRow> parseManifestOwnership(const JsonValue& value) {
    const auto& rows = jsonArray(value, "profile.expected_ownership");
    if (rows.size() < 4u || rows.size() > 64u) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     "profile.expected_ownership must contain between four and 64 rows");
    }
    std::vector<OwnershipRow> result;
    result.reserve(rows.size());
    for (const auto& rowValue : rows) {
        const auto& row = jsonObject(rowValue, "profile.expected_ownership row");
        jsonExactKeys(row, {"device_id", "category", "seat_id"},
                      "profile.expected_ownership row");
        OwnershipRow item;
        item.stableDeviceId = jsonString(jsonRequired(row, "device_id", "ownership"),
                                         "ownership.device_id", 1024u);
        item.category = parseCategory(
            jsonString(jsonRequired(row, "category", "ownership"), "ownership.category", 16u),
            "ownership.category");
        const auto seat = jsonUint(jsonRequired(row, "seat_id", "ownership"), "ownership.seat_id");
        if (seat == 0u || seat > std::numeric_limits<std::uint32_t>::max()) {
            evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                         "ownership.seat_id is outside the supported range");
        }
        item.seatId = static_cast<std::uint32_t>(seat);
        result.push_back(std::move(item));
    }
    std::sort(result.begin(), result.end(), ownershipLess);
    if (std::adjacent_find(result.begin(), result.end()) != result.end()) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     "profile.expected_ownership contains duplicate rows");
    }
    return result;
}

std::vector<Phase3SeatDeviceIdentity> parseManifestNativeScope(const JsonValue& value) {
    const auto& rows = jsonArray(value, "profile.native_hidhide_scope");
    if (rows.size() < 4u || rows.size() > kHidHideSessionMaxRequestedDevices) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     "profile.native_hidhide_scope must contain between four and sixteen rows");
    }
    std::vector<Phase3SeatDeviceIdentity> result;
    result.reserve(rows.size());
    for (const auto& rowValue : rows) {
        const auto& row = jsonObject(rowValue, "profile.native_hidhide_scope row");
        jsonExactKeys(row, {"device_id", "instance_id", "category", "seat_id"},
                      "profile.native_hidhide_scope row");
        const auto stable = jsonString(jsonRequired(row, "device_id", "native scope"),
                                       "native scope device_id", 1024u);
        const auto instance = jsonString(jsonRequired(row, "instance_id", "native scope"),
                                         "native scope instance_id", 1024u);
        const auto category = parseCategory(
            jsonString(jsonRequired(row, "category", "native scope"),
                       "native scope category", 16u),
            "native scope category");
        const auto seat = jsonUint(jsonRequired(row, "seat_id", "native scope"),
                                   "native scope seat_id");
        if (seat == 0u || seat > std::numeric_limits<std::uint32_t>::max()) {
            evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                         "native scope seat_id is outside the supported range");
        }
        const auto prefix = categoryPrefix(category);
        if (!asciiCasePrefix(stable, prefix) || stable.size() == prefix.size()) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "native scope stable device identity does not match its category prefix");
        }
        const auto derivedInstance = stable.substr(prefix.size());
        const auto canonicalManifest = hardware::canonicalizeInstanceId(asciiWide(instance, "native instance_id"));
        const auto canonicalDerived = hardware::canonicalizeInstanceId(asciiWide(derivedInstance, "derived native instance_id"));
        if (canonicalManifest != canonicalDerived) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "native scope instance_id does not match the stable device identity");
        }
        result.push_back(Phase3SeatDeviceIdentity{
            asciiWide(stable, "native stable device_id"),
            std::move(canonicalManifest),
            category,
            static_cast<std::uint32_t>(seat)});
    }
    std::sort(result.begin(), result.end(), nativeIdentityLess);
    if (std::adjacent_find(result.begin(), result.end()) != result.end()) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     "profile.native_hidhide_scope contains duplicate rows");
    }
    return result;
}

struct DerivedProfileEvidence {
    std::vector<OwnershipRow> ownership;
    std::vector<Phase3SeatDeviceIdentity> nativeScope;
};

DerivedProfileEvidence deriveCurrentProfileEvidence(const JsonValue& root) {
    const auto& profile = jsonObject(root, "current profile");
    if (jsonUint(jsonRequired(profile, "schema_version", "current profile"),
                 "current profile schema_version") != 2u) {
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     "current profile schema_version is not 2");
    }
    const auto& seats = jsonArray(jsonRequired(profile, "seats", "current profile"),
                                  "current profile seats");
    const auto& shareable = jsonArray(
        jsonRequired(profile, "shareable_resources", "current profile"),
        "current profile shareable_resources");
    std::set<std::pair<std::string, std::string>> shared;
    for (const auto& value : shareable) {
        const auto& object = jsonObject(value, "shareable resource");
        const auto type = jsonString(jsonRequired(object, "type", "shareable resource"),
                                     "shareable resource type", 64u);
        const auto id = jsonString(jsonRequired(object, "id", "shareable resource"),
                                   "shareable resource id", 1024u);
        if (type == "keyboard" || type == "mouse") shared.emplace(type, id);
    }

    struct ActiveSeat {
        std::uint32_t id{0u};
        const JsonValue::Object* object{nullptr};
    };
    std::vector<ActiveSeat> active;
    for (const auto& seatValue : seats) {
        const auto& seat = jsonObject(seatValue, "current profile seat");
        if (!jsonBool(jsonRequired(seat, "active", "current profile seat"),
                      "current profile seat.active")) {
            continue;
        }
        const auto seatId = jsonUint(jsonRequired(seat, "id", "current profile seat"),
                                     "current profile seat.id");
        if (seatId == 0u || seatId > std::numeric_limits<std::uint32_t>::max()) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "current profile active Seat id is invalid");
        }
        active.push_back({static_cast<std::uint32_t>(seatId), &seat});
    }
    std::sort(active.begin(), active.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    if (active.size() < 2u) {
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     "current profile has fewer than two active Seats");
    }
    for (std::size_t index = 1u; index < active.size(); ++index) {
        if (active[index - 1u].id == active[index].id) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "current profile has duplicate active Seat IDs");
        }
    }

    DerivedProfileEvidence result;
    std::map<std::pair<Phase3InputDeviceCategory, std::string>, std::uint32_t> owners;
    for (const auto& seat : active) {
        for (const auto category : {Phase3InputDeviceCategory::Keyboard,
                                    Phase3InputDeviceCategory::Mouse}) {
            const std::string property = category == Phase3InputDeviceCategory::Keyboard
                ? "keyboards" : "mice";
            const std::string type = category == Phase3InputDeviceCategory::Keyboard
                ? "keyboard" : "mouse";
            const auto& values = jsonArray(jsonRequired(*seat.object, property, "current profile seat"),
                                           "current profile Seat input list");
            for (const auto& value : values) {
                const auto stable = jsonString(value, "current profile device identity", 1024u);
                if (shared.contains({type, stable})) continue;
                const auto key = std::make_pair(category, stable);
                const auto [found, inserted] = owners.emplace(key, seat.id);
                if (!inserted && found->second != seat.id) {
                    evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                                 "current profile has conflicting exclusive Seat ownership");
                }
                if (!inserted) continue;
                result.ownership.push_back({stable, category, seat.id});
            }
        }
    }
    std::sort(result.ownership.begin(), result.ownership.end(), ownershipLess);

    const std::set<std::uint32_t> firstTwo{active[0].id, active[1].id};
    for (const auto& row : result.ownership) {
        if (!firstTwo.contains(row.seatId)) continue;
        const auto prefix = categoryPrefix(row.category);
        if (!asciiCasePrefix(row.stableDeviceId, prefix) ||
            row.stableDeviceId.size() == prefix.size()) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "current profile stable input identity cannot derive a HidHide instance ID");
        }
        const auto instance = hardware::canonicalizeInstanceId(
            asciiWide(row.stableDeviceId.substr(prefix.size()), "current profile instance ID"));
        result.nativeScope.push_back({asciiWide(row.stableDeviceId, "current stable device ID"),
                                      instance, row.category, row.seatId});
    }
    std::sort(result.nativeScope.begin(), result.nativeScope.end(), nativeIdentityLess);
    if (result.nativeScope.size() < 4u ||
        result.nativeScope.size() > kHidHideSessionMaxRequestedDevices) {
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     "current profile native HidHide scope is outside four to sixteen devices");
    }
    for (const auto seatId : firstTwo) {
        bool keyboard = false;
        bool mouse = false;
        for (const auto& item : result.nativeScope) {
            if (item.seatId != seatId) continue;
            keyboard = keyboard || item.category == Phase3InputDeviceCategory::Keyboard;
            mouse = mouse || item.category == Phase3InputDeviceCategory::Mouse;
        }
        if (!keyboard || !mouse) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "each of the first two Seats must own an exclusive keyboard and mouse");
        }
    }
    return result;
}

struct TraceEvidenceSummary {
    std::set<std::string> inputDevices;
    std::map<std::string, std::size_t> routedByDevice;
    std::size_t arrivals{0u};
    std::size_t removals{0u};
    std::size_t sharedAmbiguous{0u};
    std::size_t sharedRouted{0u};
};

TraceEvidenceSummary parseTraceEvidence(
    const std::filesystem::path& path,
    const std::map<std::string, std::uint32_t>* expectedSeats,
    std::string_view sharedDevice = {}) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) evidenceFail(Phase3HardwareEvidenceStatus::Tampered,
                         "P3-HW trace file is missing");
    if (size > kPhase3TraceMaxBytes) evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                                                 "P3-HW trace exceeds bounded size");
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) evidenceFail(Phase3HardwareEvidenceStatus::Tampered,
                                      "P3-HW trace could not be opened");
    TraceEvidenceSummary summary;
    std::string line;
    std::size_t lines = 0u;
    while (std::getline(input, line)) {
        if (++lines > kPhase3TraceMaxLines) {
            evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                         "P3-HW trace exceeds bounded line count");
        }
        if (line.size() > kPhase3TraceMaxLineBytes) {
            evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                         "P3-HW trace contains an oversized line");
        }
        if (line.empty()) continue;
        JsonValue record;
        try {
            record = StrictJsonParser(line).parse();
        } catch (const EvidenceError&) {
            throw;
        } catch (const std::exception& exception) {
            evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                         std::string("P3-HW trace JSON is malformed: ") + exception.what());
        }
        const auto& object = jsonObject(record, "P3-HW trace record");
        const auto recordType = jsonString(jsonRequired(object, "record", "trace record"),
                                           "trace record type", 32u);
        const auto device = jsonString(jsonRequired(object, "device_id", "trace record"),
                                       "trace device_id", 1024u);
        if (recordType == "device_change") {
            const auto change = jsonString(jsonRequired(object, "change", "device change"),
                                           "device change kind", 32u);
            if (change == "Arrival") ++summary.arrivals;
            else if (change == "Removal") ++summary.removals;
            continue;
        }
        if (recordType != "input") {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "P3-HW trace contains an unknown record type");
        }
        summary.inputDevices.insert(device);
        if (jsonString(jsonRequired(object, "isolation_guarantee", "trace input"),
                       "trace isolation guarantee", 128u) != kPhase3IsolationGuarantee) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "P3-HW trace isolation guarantee is missing or incorrect");
        }
        if (jsonBool(jsonRequired(object, "physical_suppression_requested", "trace input"),
                     "trace physical_suppression_requested")) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "P3-HW diagnostic trace unexpectedly claims physical suppression");
        }
        const auto route = jsonString(jsonRequired(object, "route", "trace input"),
                                      "trace route", 64u);
        if (route == "Routed") {
            ++summary.routedByDevice[device];
            if (!sharedDevice.empty() && device == sharedDevice) ++summary.sharedRouted;
            if (expectedSeats != nullptr) {
                const auto owner = expectedSeats->find(device);
                if (owner == expectedSeats->end()) {
                    evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                                 "P3-HW trace routed a device absent from expected ownership");
                }
                const auto seat = jsonUint(jsonRequired(object, "seat_id", "routed trace input"),
                                           "routed trace seat_id");
                if (seat != owner->second) {
                    evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                                 "P3-HW trace routed a device to the wrong Seat");
                }
            }
        } else if (route == "AmbiguousSharedDevice" && !sharedDevice.empty() &&
                   device == sharedDevice) {
            ++summary.sharedAmbiguous;
        }
    }
    if (!input.eof()) evidenceFail(Phase3HardwareEvidenceStatus::Tampered,
                                   "P3-HW trace read failed");
    if (lines == 0u) evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                                 "P3-HW trace contains no evidence records");
    return summary;
}

void validateGateCMetrics(const std::filesystem::path& path) {
    const auto bytes = readBoundedFile(path, kPhase3MetricsMaxBytes, "Gate C metrics");
    JsonValue root;
    try {
        root = StrictJsonParser(bytes).parse();
    } catch (const EvidenceError&) {
        throw;
    } catch (const std::exception& exception) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string("Gate C metrics JSON is malformed: ") + exception.what());
    }
    const auto& report = jsonObject(root, "Gate C metrics");
    if (jsonUint(jsonRequired(report, "schema_version", "Gate C metrics"),
                 "Gate C metrics schema_version") != 1u) {
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     "Gate C metrics schema_version is not 1");
    }
    const auto unique = jsonUint(jsonRequired(report, "unique_input_events", "Gate C metrics"),
                                 "Gate C unique_input_events");
    const auto complete = jsonUint(jsonRequired(report, "complete_input_events", "Gate C metrics"),
                                   "Gate C complete_input_events");
    const auto missingStage = jsonUint(jsonRequired(report, "missing_stage_events", "Gate C metrics"),
                                       "Gate C missing_stage_events");
    const auto receiver = jsonUint(jsonRequired(report, "receiver_verified_events", "Gate C metrics"),
                                   "Gate C receiver_verified_events");
    const auto missingReceiver = jsonUint(
        jsonRequired(report, "missing_receiver_evidence_events", "Gate C metrics"),
        "Gate C missing_receiver_evidence_events");
    const auto crossSeat = jsonUint(jsonRequired(report, "cross_seat_events", "Gate C metrics"),
                                    "Gate C cross_seat_events");
    const auto crossProcess = jsonUint(jsonRequired(report, "cross_process_events", "Gate C metrics"),
                                       "Gate C cross_process_events");
    if (unique == 0u || complete != unique || missingStage != 0u || receiver != unique ||
        missingReceiver != 0u || crossSeat != 0u || crossProcess != 0u) {
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     "Gate C receiver/zero-bleed evidence is incomplete or contradictory");
    }
    const auto& queue = jsonObject(jsonRequired(report, "queue", "Gate C metrics"),
                                   "Gate C queue");
    if (jsonUint(jsonRequired(queue, "dropped_frames", "Gate C queue"),
                 "Gate C queue.dropped_frames") != 0u) {
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     "Gate C writer queue dropped frames");
    }
    const auto& recorder = jsonObject(jsonRequired(report, "recorder", "Gate C metrics"),
                                      "Gate C recorder");
    for (const auto field : {"rotation_drops", "contention_drops", "invalid_samples"}) {
        if (jsonUint(jsonRequired(recorder, field, "Gate C recorder"), field) != 0u) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         std::string("Gate C metrics recorder ") + field + " is nonzero");
        }
    }
}

struct EvidenceArtifact {
    std::filesystem::path path;
    std::string sha256;
};

struct ValidatedPhysicalManifest {
    std::string sessionId;
    std::string manifestSha256;
    std::filesystem::path profilePath;
    std::string profileSha256;
    std::vector<EvidenceArtifact> artifacts;
    std::vector<Phase3SeatDeviceIdentity> nativeScope;
    std::uint64_t manualVerdictUnixSeconds{0u};
    std::uint64_t validUntilUnixSeconds{0u};
};

std::set<std::string> requiredManualChecks(std::string_view stage) {
    if (stage == "gate_a") {
        return {"two_keyboards_distinct", "two_pointing_devices_distinct",
                "key_down_up_transitions", "composite_child_removal",
                "unplug_replug_identity", "soak_minimum_duration",
                "drop_counter_reviewed"};
    }
    if (stage == "gate_b") {
        return {"seat1_exclusive_routing", "seat2_exclusive_routing",
                "unassigned_fails_closed", "shared_ambiguous_fails_closed",
                "missing_target_explicit_failure", "trace_seat_target_reviewed"};
    }
    return {"two_controlled_targets_visible", "seat1_changes_only_target1",
            "seat2_changes_only_target2", "unassigned_shared_fail_closed",
            "normal_windows_input_not_claimed_suppressed",
            "cleanup_no_owned_child_left", "metrics_reviewed"};
}

void validateManualChecks(const JsonValue& value, std::string_view stage) {
    const auto& checks = jsonObject(value, "P3-HW manual checks");
    const auto required = requiredManualChecks(stage);
    if (checks.size() != required.size()) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string(stage) + " manual check set is incomplete or contains extras");
    }
    for (const auto& requiredName : required) {
        const auto found = checks.find(requiredName);
        if (found == checks.end()) {
            evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                         std::string(stage) + " is missing manual check " + requiredName);
        }
        const auto verdict = jsonString(found->second, "manual check verdict", 32u);
        if (verdict == "PASS") continue;
        if (stage == "gate_a" && requiredName == "composite_child_removal" &&
            verdict == "NOT_APPLICABLE") {
            continue;
        }
        if (verdict == "PENDING") {
            evidenceFail(Phase3HardwareEvidenceStatus::Pending,
                         std::string(stage) + " manual checks remain PENDING");
        }
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     std::string(stage) + " manual check did not PASS");
    }
}

ValidatedPhysicalManifest validatePhysicalManifest(
    const std::filesystem::path& manifestPath,
    std::string_view manifestBytes,
    std::uint64_t nowUnixSeconds) {
    JsonValue rootValue;
    try {
        rootValue = StrictJsonParser(manifestBytes).parse();
    } catch (const EvidenceError&) {
        throw;
    } catch (const std::exception& exception) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string("P3-HW manifest JSON is malformed: ") + exception.what());
    }
    const auto& root = jsonObject(rootValue, "P3-HW manifest");
    jsonExactKeys(root,
                  {"schema_version", "session_id", "created_utc", "updated_utc", "state",
                   "privacy", "environment", "profile", "stages", "manual_verdict",
                   "manual_verdict_note", "manual_verdict_unix", "evidence_valid_until_unix"},
                  "P3-HW manifest");
    if (jsonUint(jsonRequired(root, "schema_version", "P3-HW manifest"),
                 "P3-HW schema_version") != 1u) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     "P3-HW manifest schema_version must be 1");
    }
    const auto sessionId = jsonString(jsonRequired(root, "session_id", "P3-HW manifest"),
                                      "P3-HW session_id", 128u);
    (void)jsonString(jsonRequired(root, "created_utc", "P3-HW manifest"),
                     "P3-HW created_utc", 64u);
    (void)jsonString(jsonRequired(root, "updated_utc", "P3-HW manifest"),
                     "P3-HW updated_utc", 64u);
    (void)jsonStringAllowEmpty(jsonRequired(root, "manual_verdict_note", "P3-HW manifest"),
                               "P3-HW manual_verdict_note", 8192u);

    const auto state = jsonString(jsonRequired(root, "state", "P3-HW manifest"),
                                  "P3-HW state", 32u);
    const auto manualVerdict = jsonString(
        jsonRequired(root, "manual_verdict", "P3-HW manifest"),
        "P3-HW manual_verdict", 32u);
    if (state != "MANUAL_PASS" || manualVerdict != "PASS") {
        if (state == "IN_PROGRESS" || state == "READY_FOR_REVIEW" ||
            manualVerdict == "PENDING") {
            evidenceFail(Phase3HardwareEvidenceStatus::Pending,
                         "P3-HW manifest does not contain an explicit final manual PASS");
        }
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     "P3-HW manifest final state/verdict is not PASS");
    }
    const auto manualUnix = jsonUint(
        jsonRequired(root, "manual_verdict_unix", "P3-HW manifest"),
        "P3-HW manual_verdict_unix");
    const auto validUntil = jsonUint(
        jsonRequired(root, "evidence_valid_until_unix", "P3-HW manifest"),
        "P3-HW evidence_valid_until_unix");
    if (manualUnix > std::numeric_limits<std::uint64_t>::max() -
                         kPhase3HardwareEvidenceValiditySeconds ||
        validUntil != manualUnix + kPhase3HardwareEvidenceValiditySeconds) {
        evidenceFail(Phase3HardwareEvidenceStatus::Stale,
                     "P3-HW evidence validity window is not the fixed 24-hour interval");
    }
    if (manualUnix > nowUnixSeconds + kPhase3HardwareEvidenceMaxClockSkewSeconds ||
        validUntil <= nowUnixSeconds) {
        evidenceFail(Phase3HardwareEvidenceStatus::Stale,
                     "P3-HW evidence is stale or has an implausible future timestamp");
    }

    const auto& privacy = jsonObject(jsonRequired(root, "privacy", "P3-HW manifest"),
                                     "P3-HW privacy");
    jsonExactKeys(privacy, {"sensitive_key_ids_enabled", "notice_acknowledged"},
                  "P3-HW privacy");
    (void)jsonBool(jsonRequired(privacy, "sensitive_key_ids_enabled", "P3-HW privacy"),
                   "P3-HW sensitive_key_ids_enabled");
    if (!jsonBool(jsonRequired(privacy, "notice_acknowledged", "P3-HW privacy"),
                  "P3-HW notice_acknowledged")) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     "P3-HW privacy notice was not acknowledged");
    }
    const auto& environment = jsonObject(
        jsonRequired(root, "environment", "P3-HW manifest"), "P3-HW environment");
    jsonExactKeys(environment,
                  {"windows_version", "windows_build", "architecture", "hardware_notes"},
                  "P3-HW environment");
    (void)jsonStringAllowEmpty(jsonRequired(environment, "windows_version", "environment"),
                               "environment.windows_version", 128u);
    (void)jsonStringAllowEmpty(jsonRequired(environment, "windows_build", "environment"),
                               "environment.windows_build", 128u);
    (void)jsonStringAllowEmpty(jsonRequired(environment, "architecture", "environment"),
                               "environment.architecture", 64u);
    (void)jsonStringAllowEmpty(jsonRequired(environment, "hardware_notes", "environment"),
                               "environment.hardware_notes", 8192u);

    const auto& profile = jsonObject(jsonRequired(root, "profile", "P3-HW manifest"),
                                     "P3-HW profile block");
    jsonExactKeys(profile,
                  {"source_path", "sha256", "schema_version", "expected_ownership",
                   "native_hidhide_scope", "shareable_resources", "shared_case"},
                  "P3-HW profile block");
    if (jsonUint(jsonRequired(profile, "schema_version", "P3-HW profile block"),
                 "P3-HW profile schema_version") != 2u) {
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     "P3-HW recorded profile schema_version is not 2");
    }
    const auto profilePath = resolveProfilePath(
        manifestPath,
        jsonString(jsonRequired(profile, "source_path", "P3-HW profile block"),
                   "P3-HW profile source_path", 32768u));
    const auto profileSha = normalizedSha256(
        jsonString(jsonRequired(profile, "sha256", "P3-HW profile block"),
                   "P3-HW profile sha256", 64u));
    requireExpectedHash(profilePath, profileSha, kPhase3ProfileMaxBytes, "current profile");
    const auto profileBytes = readBoundedFile(profilePath, kPhase3ProfileMaxBytes,
                                              "current profile");
    JsonValue currentProfile;
    try {
        currentProfile = StrictJsonParser(profileBytes).parse();
    } catch (const EvidenceError&) {
        throw;
    } catch (const std::exception& exception) {
        evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                     std::string("current profile JSON is malformed: ") + exception.what());
    }
    if (sha256Bytes(profileBytes) != profileSha) {
        evidenceFail(Phase3HardwareEvidenceStatus::Tampered,
                     "current profile changed while P3-HW evidence was being verified");
    }
    const auto derivedProfile = deriveCurrentProfileEvidence(currentProfile);
    const auto recordedOwnership = parseManifestOwnership(
        jsonRequired(profile, "expected_ownership", "P3-HW profile block"));
    const auto recordedNative = parseManifestNativeScope(
        jsonRequired(profile, "native_hidhide_scope", "P3-HW profile block"));
    if (recordedOwnership != derivedProfile.ownership) {
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     "P3-HW Seat/device ownership does not match the current hashed profile");
    }
    if (recordedNative != derivedProfile.nativeScope) {
        evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                     "P3-HW native HidHide scope does not match the current hashed profile");
    }
    (void)jsonArray(jsonRequired(profile, "shareable_resources", "P3-HW profile block"),
                    "P3-HW shareable_resources");

    ValidatedPhysicalManifest validated;
    validated.sessionId = sessionId;
    validated.manifestSha256 = sha256Bytes(manifestBytes);
    validated.profilePath = profilePath;
    validated.profileSha256 = profileSha;
    validated.nativeScope = recordedNative;
    validated.manualVerdictUnixSeconds = manualUnix;
    validated.validUntilUnixSeconds = validUntil;

    const auto& sharedCase = jsonObject(
        jsonRequired(profile, "shared_case", "P3-HW profile block"),
        "P3-HW shared_case");
    jsonExactKeys(sharedCase, {"derived_profile", "sha256", "device_id", "category"},
                  "P3-HW shared_case");
    const auto sharedRelative = jsonString(
        jsonRequired(sharedCase, "derived_profile", "P3-HW shared_case"),
        "P3-HW shared_case derived_profile", 260u);
    const auto sharedPath = resolveSessionArtifact(manifestPath, sharedRelative,
                                                   "shared-case profile");
    const auto sharedSha = normalizedSha256(jsonString(
        jsonRequired(sharedCase, "sha256", "P3-HW shared_case"),
        "P3-HW shared_case sha256", 64u));
    requireExpectedHash(sharedPath, sharedSha, kPhase3ProfileMaxBytes, "shared-case profile");
    const auto sharedDevice = jsonString(
        jsonRequired(sharedCase, "device_id", "P3-HW shared_case"),
        "P3-HW shared_case device_id", 1024u);
    (void)parseCategory(jsonString(
        jsonRequired(sharedCase, "category", "P3-HW shared_case"),
        "P3-HW shared_case category", 16u), "P3-HW shared_case category");
    validated.artifacts.push_back({sharedPath, sharedSha});

    std::map<std::string, std::uint32_t> expectedSeats;
    for (const auto& row : recordedOwnership) {
        const auto [found, inserted] = expectedSeats.emplace(row.stableDeviceId, row.seatId);
        if (!inserted && found->second != row.seatId) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "P3-HW expected ownership contains conflicting stable device IDs");
        }
    }
    std::set<std::string> nativeStableIds;
    for (const auto& item : recordedNative) {
        std::string stable;
        stable.reserve(item.stableDeviceId.size());
        for (const auto ch : item.stableDeviceId) {
            if (ch < 0 || static_cast<std::uint32_t>(ch) > 0x7fu) {
                evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                             "P3-HW stable device ID is not ASCII");
            }
            stable.push_back(static_cast<char>(ch));
        }
        nativeStableIds.insert(std::move(stable));
    }

    const auto& stages = jsonObject(jsonRequired(root, "stages", "P3-HW manifest"),
                                    "P3-HW stages");
    jsonExactKeys(stages, {"gate_a", "gate_b", "gate_c"}, "P3-HW stages");
    for (const auto stageName : {std::string_view("gate_a"),
                                 std::string_view("gate_b"),
                                 std::string_view("gate_c")}) {
        const auto& stage = jsonObject(jsonRequired(stages, stageName, "P3-HW stages"),
                                       std::string("P3-HW ") + std::string(stageName));
        jsonExactKeys(stage,
                      {"status", "verdict", "started_utc", "ended_utc",
                       "duration_seconds", "process_exit_code", "trace", "trace_sha256",
                       "metrics_report", "metrics_report_sha256", "auxiliary_traces",
                       "auxiliary_trace_sha256", "manual_checks", "notes"},
                      std::string("P3-HW ") + std::string(stageName));
        const auto status = jsonString(jsonRequired(stage, "status", stageName),
                                       "P3-HW stage status", 32u);
        const auto verdict = jsonString(jsonRequired(stage, "verdict", stageName),
                                        "P3-HW stage verdict", 32u);
        if (status != "RECORDED" || verdict != "PASS") {
            if (status == "PENDING" || status == "RUNNING" || verdict == "PENDING") {
                evidenceFail(Phase3HardwareEvidenceStatus::Pending,
                             std::string(stageName) + " is not a recorded PASS");
            }
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         std::string(stageName) + " did not PASS");
        }
        jsonStringOrNull(jsonRequired(stage, "started_utc", stageName), "stage.started_utc");
        jsonStringOrNull(jsonRequired(stage, "ended_utc", stageName), "stage.ended_utc");
        if (jsonUint(jsonRequired(stage, "process_exit_code", stageName),
                     "stage.process_exit_code") != 0u) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         std::string(stageName) + " process exit code is nonzero");
        }
        if (stageName == "gate_a" &&
            jsonDouble(jsonRequired(stage, "duration_seconds", stageName),
                       "gate_a.duration_seconds") < 600.0) {
            evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                         "Gate A duration is below the required 10-minute soak");
        }
        (void)jsonStringAllowEmpty(jsonRequired(stage, "notes", stageName),
                                   "stage.notes", 8192u);
        validateManualChecks(jsonRequired(stage, "manual_checks", stageName), stageName);

        const auto traceRelative = jsonString(jsonRequired(stage, "trace", stageName),
                                              "stage.trace", 260u);
        const auto tracePath = resolveSessionArtifact(manifestPath, traceRelative,
                                                      std::string(stageName) + " trace");
        const auto traceSha = normalizedSha256(jsonString(
            jsonRequired(stage, "trace_sha256", stageName), "stage.trace_sha256", 64u));
        requireExpectedHash(tracePath, traceSha, kPhase3TraceMaxBytes,
                            std::string(stageName) + " trace");
        validated.artifacts.push_back({tracePath, traceSha});

        if (stageName == "gate_a") {
            if (!jsonNull(jsonRequired(stage, "metrics_report", stageName)) ||
                !jsonNull(jsonRequired(stage, "metrics_report_sha256", stageName)) ||
                !jsonArray(jsonRequired(stage, "auxiliary_traces", stageName),
                           "gate_a auxiliary_traces").empty() ||
                !jsonArray(jsonRequired(stage, "auxiliary_trace_sha256", stageName),
                           "gate_a auxiliary_trace_sha256").empty()) {
                evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                             "Gate A must not contain metrics or auxiliary artifacts");
            }
            const auto trace = parseTraceEvidence(tracePath, nullptr);
            if (trace.arrivals == 0u || trace.removals == 0u) {
                evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                             "Gate A lacks removal/arrival hot-plug evidence");
            }
            for (const auto& stable : nativeStableIds) {
                if (!trace.inputDevices.contains(stable)) {
                    evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                                 "Gate A is missing a native-scope physical identity");
                }
            }
        } else if (stageName == "gate_b") {
            if (!jsonNull(jsonRequired(stage, "metrics_report", stageName)) ||
                !jsonNull(jsonRequired(stage, "metrics_report_sha256", stageName))) {
                evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                             "Gate B must not contain a metrics report");
            }
            const auto mainTrace = parseTraceEvidence(tracePath, &expectedSeats);
            for (const auto& stable : nativeStableIds) {
                const auto found = mainTrace.routedByDevice.find(stable);
                if (found == mainTrace.routedByDevice.end() || found->second == 0u) {
                    evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                                 "Gate B lacks routed evidence for a native-scope device");
                }
            }
            const auto& auxiliary = jsonArray(
                jsonRequired(stage, "auxiliary_traces", stageName),
                "gate_b auxiliary_traces");
            const auto& auxiliaryHashes = jsonArray(
                jsonRequired(stage, "auxiliary_trace_sha256", stageName),
                "gate_b auxiliary_trace_sha256");
            if (auxiliary.size() != 1u || auxiliaryHashes.size() != 1u) {
                evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                             "Gate B requires exactly one shared-case auxiliary trace/hash");
            }
            const auto auxiliaryPath = resolveSessionArtifact(
                manifestPath, jsonString(auxiliary.front(), "Gate B auxiliary trace", 260u),
                "Gate B auxiliary trace");
            const auto auxiliarySha = normalizedSha256(
                jsonString(auxiliaryHashes.front(), "Gate B auxiliary SHA", 64u));
            requireExpectedHash(auxiliaryPath, auxiliarySha, kPhase3TraceMaxBytes,
                                "Gate B auxiliary trace");
            validated.artifacts.push_back({auxiliaryPath, auxiliarySha});
            const auto sharedTrace = parseTraceEvidence(auxiliaryPath, nullptr, sharedDevice);
            if (sharedTrace.sharedAmbiguous == 0u || sharedTrace.sharedRouted != 0u) {
                evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                             "Gate B shared ownership did not fail closed as ambiguous");
            }
        } else {
            if (!jsonArray(jsonRequired(stage, "auxiliary_traces", stageName),
                           "gate_c auxiliary_traces").empty() ||
                !jsonArray(jsonRequired(stage, "auxiliary_trace_sha256", stageName),
                           "gate_c auxiliary_trace_sha256").empty()) {
                evidenceFail(Phase3HardwareEvidenceStatus::Malformed,
                             "Gate C must not contain auxiliary traces");
            }
            const auto mainTrace = parseTraceEvidence(tracePath, &expectedSeats);
            for (const auto& stable : nativeStableIds) {
                const auto found = mainTrace.routedByDevice.find(stable);
                if (found == mainTrace.routedByDevice.end() || found->second == 0u) {
                    evidenceFail(Phase3HardwareEvidenceStatus::Mismatched,
                                 "Gate C lacks routed evidence for a native-scope device");
                }
            }
            const auto metricsRelative = jsonString(
                jsonRequired(stage, "metrics_report", stageName), "gate_c.metrics_report", 260u);
            const auto metricsPath = resolveSessionArtifact(manifestPath, metricsRelative,
                                                            "Gate C metrics report");
            const auto metricsSha = normalizedSha256(jsonString(
                jsonRequired(stage, "metrics_report_sha256", stageName),
                "gate_c.metrics_report_sha256", 64u));
            requireExpectedHash(metricsPath, metricsSha, kPhase3MetricsMaxBytes,
                                "Gate C metrics report");
            validateGateCMetrics(metricsPath);
            if (sha256File(metricsPath, kPhase3MetricsMaxBytes, "Gate C metrics report") !=
                metricsSha) {
                evidenceFail(Phase3HardwareEvidenceStatus::Tampered,
                             "Gate C metrics changed while being verified");
            }
            validated.artifacts.push_back({metricsPath, metricsSha});
        }
        if (sha256File(tracePath, kPhase3TraceMaxBytes,
                       std::string(stageName) + " trace") != traceSha) {
            evidenceFail(Phase3HardwareEvidenceStatus::Tampered,
                         std::string(stageName) + " trace changed while being verified");
        }
    }
    return validated;
}

} // namespace

Phase3HardwareEvidenceLoadResult loadPhase3HardwareAcceptanceEvidence(
    const std::filesystem::path& manifestPath,
    std::uint64_t nowUnixSeconds) {
    Phase3HardwareEvidenceLoadResult result;
    if (manifestPath.empty()) {
        result.status = Phase3HardwareEvidenceStatus::Missing;
        result.diagnostic = "P3-HW manifest path is empty";
        return result;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(manifestPath, ec) || ec) {
        result.status = Phase3HardwareEvidenceStatus::Missing;
        result.diagnostic = "P3-HW manifest file is missing";
        return result;
    }
    const auto resolvedNow = nowUnixSeconds == 0u ? currentUnixSeconds() : nowUnixSeconds;
    if (resolvedNow == 0u) {
        result.status = Phase3HardwareEvidenceStatus::IoFailure;
        result.diagnostic = "system wall-clock time is unavailable for P3-HW freshness verification";
        return result;
    }
    try {
        const auto canonicalManifest = std::filesystem::weakly_canonical(manifestPath, ec);
        if (ec) {
            evidenceFail(Phase3HardwareEvidenceStatus::IoFailure,
                         "P3-HW manifest path could not be canonicalized");
        }
        const auto bytes = readBoundedFile(canonicalManifest, kPhase3ManifestMaxBytes,
                                           "P3-HW manifest");
        auto validated = validatePhysicalManifest(canonicalManifest, bytes, resolvedNow);

        Phase3HardwareAcceptanceEvidence evidence;
        evidence.sessionId_ = std::move(validated.sessionId);
        evidence.manifestPath_ = canonicalManifest;
        evidence.manifestSha256_ = std::move(validated.manifestSha256);
        evidence.profilePath_ = std::move(validated.profilePath);
        evidence.profileSha256_ = std::move(validated.profileSha256);
        evidence.nativeScope_ = std::move(validated.nativeScope);
        evidence.manualVerdictUnixSeconds_ = validated.manualVerdictUnixSeconds;
        evidence.validUntilUnixSeconds_ = validated.validUntilUnixSeconds;
        evidence.artifacts_.reserve(validated.artifacts.size());
        for (auto& artifact : validated.artifacts) {
            evidence.artifacts_.push_back(
                Phase3HardwareAcceptanceEvidence::ArtifactBinding{
                    std::move(artifact.path), std::move(artifact.sha256)});
        }

        result.status = Phase3HardwareEvidenceStatus::Accepted;
        result.evidence = std::move(evidence);
        result.diagnostic = "P3-HW manifest, profile, Gate A/B/C, receiver evidence, and manual PASS verified";
        return result;
    } catch (const EvidenceError& exception) {
        result.status = exception.status;
        result.diagnostic = boundedDiagnostic(exception.what());
        return result;
    } catch (const std::exception& exception) {
        result.status = Phase3HardwareEvidenceStatus::Malformed;
        result.diagnostic = boundedDiagnostic(
            std::string("P3-HW evidence validation failed: ") + exception.what());
        return result;
    }
}

bool validatePhase3HardwareAcceptanceEvidenceForDevices(
    const Phase3HardwareAcceptanceEvidence& evidence,
    std::span<const std::wstring> requestedDeviceInstanceIds,
    std::uint64_t nowUnixSeconds,
    std::string* error) {
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = boundedDiagnostic(std::move(message));
        return false;
    };
    const auto resolvedNow = nowUnixSeconds == 0u ? currentUnixSeconds() : nowUnixSeconds;
    if (resolvedNow == 0u) return fail("system wall-clock time is unavailable");
    if (evidence.manualVerdictUnixSeconds_ == 0u || evidence.validUntilUnixSeconds_ == 0u ||
        evidence.manualVerdictUnixSeconds_ >
            std::numeric_limits<std::uint64_t>::max() -
                kPhase3HardwareEvidenceValiditySeconds ||
        evidence.validUntilUnixSeconds_ !=
            evidence.manualVerdictUnixSeconds_ + kPhase3HardwareEvidenceValiditySeconds) {
        return fail("typed P3-HW evidence contains an invalid validity window");
    }
    if (evidence.manualVerdictUnixSeconds_ >
            resolvedNow + kPhase3HardwareEvidenceMaxClockSkewSeconds ||
        evidence.validUntilUnixSeconds_ <= resolvedNow) {
        return fail("typed P3-HW evidence is stale or future-dated");
    }
    if (evidence.nativeScope_.size() < 4u ||
        evidence.nativeScope_.size() > kHidHideSessionMaxRequestedDevices) {
        return fail("typed P3-HW evidence contains an invalid native device scope");
    }
    try {
        if (sha256File(evidence.manifestPath_, kPhase3ManifestMaxBytes,
                       "P3-HW manifest") != evidence.manifestSha256_) {
            return fail("P3-HW manifest changed after typed evidence was loaded");
        }
        if (sha256File(evidence.profilePath_, kPhase3ProfileMaxBytes,
                       "P3-HW profile") != evidence.profileSha256_) {
            return fail("P3-HW profile changed after typed evidence was loaded");
        }
        for (const auto& artifact : evidence.artifacts_) {
            if (sha256File(artifact.path, kPhase3TraceMaxBytes,
                           "P3-HW bound artifact") != artifact.sha256) {
                return fail("P3-HW bound artifact changed after typed evidence was loaded");
            }
        }
    } catch (const EvidenceError& exception) {
        return fail(exception.what());
    } catch (const std::exception& exception) {
        return fail(std::string("P3-HW bound evidence revalidation failed: ") +
                    exception.what());
    }

    std::vector<std::wstring> expected;
    expected.reserve(evidence.nativeScope_.size());
    for (const auto& item : evidence.nativeScope_) {
        expected.push_back(hardware::canonicalizeInstanceId(item.deviceInstanceId));
    }
    std::sort(expected.begin(), expected.end());
    if (std::adjacent_find(expected.begin(), expected.end()) != expected.end()) {
        return fail("typed P3-HW evidence contains duplicate native instance IDs");
    }

    std::vector<std::wstring> requested;
    requested.reserve(requestedDeviceInstanceIds.size());
    for (const auto& item : requestedDeviceInstanceIds) {
        if (!boundedText(item)) return fail("requested HidHide device identity is invalid");
        requested.push_back(hardware::canonicalizeInstanceId(item));
    }
    std::sort(requested.begin(), requested.end());
    if (std::adjacent_find(requested.begin(), requested.end()) != requested.end()) {
        return fail("requested HidHide device identities are not unique");
    }
    if (requested != expected) {
        return fail("requested HidHide device scope does not exactly match typed P3-HW native scope");
    }
    if (error != nullptr) error->clear();
    return true;
}

bool validatePhase3HardwareAcceptanceEvidenceForSeatDevices(
    const Phase3HardwareAcceptanceEvidence& evidence,
    std::uint32_t seatId,
    std::span<const std::wstring> requestedDeviceInstanceIds,
    std::uint64_t nowUnixSeconds,
    std::string* error) {
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = boundedDiagnostic(std::move(message));
        return false;
    };
    if (seatId == 0) return fail("typed P3-HW Seat-scoped validation requires a nonzero Seat ID");

    // Revalidate the complete signed/hashed acceptance scope first. Seat-scoped
    // authority is a strict reduction of that existing capability, never a way
    // to bypass its freshness/artifact/native-scope checks.
    std::vector<std::wstring> fullScope;
    fullScope.reserve(evidence.nativeScope().size());
    for (const auto& item : evidence.nativeScope()) fullScope.push_back(item.deviceInstanceId);
    std::string fullError;
    if (!validatePhase3HardwareAcceptanceEvidenceForDevices(
            evidence, fullScope, nowUnixSeconds, &fullError)) {
        return fail("typed P3-HW full evidence revalidation failed: " + fullError);
    }

    std::vector<std::wstring> expected;
    for (const auto& item : evidence.nativeScope()) {
        if (item.seatId == seatId) {
            expected.push_back(hardware::canonicalizeInstanceId(item.deviceInstanceId));
        }
    }
    if (expected.empty()) return fail("typed P3-HW evidence contains no native devices for this Seat");
    std::sort(expected.begin(), expected.end());
    if (std::adjacent_find(expected.begin(), expected.end()) != expected.end()) {
        return fail("typed P3-HW Seat scope contains duplicate native instance IDs");
    }

    std::vector<std::wstring> requested;
    requested.reserve(requestedDeviceInstanceIds.size());
    for (const auto& item : requestedDeviceInstanceIds) {
        if (!boundedText(item)) return fail("requested Seat HidHide device identity is invalid");
        requested.push_back(hardware::canonicalizeInstanceId(item));
    }
    std::sort(requested.begin(), requested.end());
    if (std::adjacent_find(requested.begin(), requested.end()) != requested.end()) {
        return fail("requested Seat HidHide device identities are not unique");
    }
    if (requested != expected) {
        return fail("requested HidHide device scope does not exactly match the typed P3-HW native scope for this Seat");
    }
    if (error != nullptr) error->clear();
    return true;
}

std::string_view phase3HardwareEvidenceStatusName(
    Phase3HardwareEvidenceStatus status) noexcept {
    switch (status) {
    case Phase3HardwareEvidenceStatus::Accepted: return "accepted";
    case Phase3HardwareEvidenceStatus::Missing: return "missing";
    case Phase3HardwareEvidenceStatus::Malformed: return "malformed";
    case Phase3HardwareEvidenceStatus::Pending: return "pending";
    case Phase3HardwareEvidenceStatus::Stale: return "stale";
    case Phase3HardwareEvidenceStatus::Tampered: return "tampered";
    case Phase3HardwareEvidenceStatus::Mismatched: return "mismatched";
    case Phase3HardwareEvidenceStatus::IoFailure: return "io-failure";
    }
    return "unknown";
}

HidHideSessionTransaction::HidHideSessionTransaction(
    std::shared_ptr<HidHideSessionPlatform> platform)
    : platform_(std::move(platform)) {}

HidHideSessionTransaction::~HidHideSessionTransaction() {
    if (phase_ == HidHideSessionPhase::Active ||
        phase_ == HidHideSessionPhase::Prepared) {
        (void)rollbackInternal(false);
    }
}

HidHideSessionResult HidHideSessionTransaction::prepare(
    HidHideSessionRequest request,
    std::uint64_t nowMilliseconds) {
    if (phase_ == HidHideSessionPhase::RecoveryRequired) {
        return result(HidHideSessionResultCode::RecoveryRequired,
                      "HidHide session is recovery-required");
    }
    if (phase_ == HidHideSessionPhase::Active) {
        return result(HidHideSessionResultCode::UnsupportedState,
                      "active HidHide session must roll back before replanning");
    }

    std::string error;
    if (!validateHidHideSessionRequest(request, error)) {
        return result(HidHideSessionResultCode::InvalidRequest, std::move(error));
    }
    if (!platform_) {
        return result(HidHideSessionResultCode::BackendFailure,
                      "HidHide session platform is unavailable");
    }

    HidHideSessionSnapshot before;
    if (!platform_->readState(before, error)) {
        return result(HidHideSessionResultCode::BackendFailure,
                      boundedDiagnostic("HidHide state snapshot failed: " + error));
    }
    before = normalizedSnapshot(std::move(before));
    if (before.inverseWhitelist) {
        return result(HidHideSessionResultCode::UnsupportedState,
                      "inverse HidHide application-list mode is not supported by the guarded v1 transaction");
    }

    if (request.expiryMilliseconds >
        std::numeric_limits<std::uint64_t>::max() - nowMilliseconds) {
        return result(HidHideSessionResultCode::InvalidRequest,
                      "HidHide session expiry would overflow the monotonic deadline");
    }

    request.deviceInstanceIds = normalizedUnique(request.deviceInstanceIds, false);
    request.allowedApplications = normalizedUnique(request.allowedApplications, true);

    HidHideSessionPlan plan;
    plan.request = std::move(request);
    plan.before = before;
    plan.applied = makeHidHideSessionAppliedState(plan.before, plan.request);
    plan.preparedAtMilliseconds = nowMilliseconds;
    plan.expiryAtMilliseconds = nowMilliseconds + plan.request.expiryMilliseconds;

    plan_ = std::move(plan);
    phase_ = HidHideSessionPhase::Prepared;
    return result(HidHideSessionResultCode::Ok,
                  "HidHide session plan prepared without mutation");
}

HidHideSessionResult HidHideSessionTransaction::activate(
    std::uint64_t nowMilliseconds) {
    if (phase_ == HidHideSessionPhase::Active) {
        return result(HidHideSessionResultCode::AlreadySatisfied,
                      "HidHide session is already active");
    }
    if (phase_ == HidHideSessionPhase::RecoveryRequired) {
        return result(HidHideSessionResultCode::RecoveryRequired,
                      "HidHide session requires recovery before activation");
    }
    if (phase_ != HidHideSessionPhase::Prepared || !plan_) {
        return result(HidHideSessionResultCode::UnsupportedState,
                      "HidHide activation requires a prepared plan");
    }
    if (nowMilliseconds >= plan_->expiryAtMilliseconds) {
        phase_ = HidHideSessionPhase::Idle;
        plan_.reset();
        return result(HidHideSessionResultCode::Expired,
                      "prepared HidHide session expired before activation");
    }
    if (!plan_->request.physicalAcceptanceEvidence.has_value()) {
        return result(HidHideSessionResultCode::PhysicalGateRequired,
                      "native HidHide mutation requires typed P3-HW Gate A/B/C physical evidence");
    }
    std::string physicalEvidenceError;
    const bool physicalEvidenceValid = plan_->request.physicalEvidenceSeatId == 0u
        ? validatePhase3HardwareAcceptanceEvidenceForDevices(
              *plan_->request.physicalAcceptanceEvidence,
              plan_->request.deviceInstanceIds, 0u, &physicalEvidenceError)
        : validatePhase3HardwareAcceptanceEvidenceForSeatDevices(
              *plan_->request.physicalAcceptanceEvidence,
              plan_->request.physicalEvidenceSeatId,
              plan_->request.deviceInstanceIds, 0u, &physicalEvidenceError);
    if (!physicalEvidenceValid) {
        return result(
            HidHideSessionResultCode::PhysicalGateRequired,
            boundedDiagnostic("typed P3-HW physical evidence is no longer valid: " +
                              physicalEvidenceError));
    }
    if (!plan_->request.nativeMutationApproved) {
        return result(HidHideSessionResultCode::NativeMutationDisabled,
                      "native HidHide mutation requires explicit high-risk approval");
    }
    if (!platform_->mutationSupported()) {
        return result(HidHideSessionResultCode::NativeMutationDisabled,
                      "selected HidHide platform exposes no native mutation capability");
    }
    if (!platform_->sessionBlacklistSupported()) {
        return result(HidHideSessionResultCode::UnsupportedState,
                      "verified HidHide process-lifetime session blacklist support is required");
    }

    std::string error;
    if (!platform_->writeState(plan_->applied, error)) {
        // A backend may have partially mutated before reporting failure. Keep
        // the exact snapshot/plan owned and immediately enter verified cleanup
        // rather than assuming the failed call was side-effect free.
        phase_ = HidHideSessionPhase::RollingBack;
        const auto cleanup = rollbackInternal(false);
        if (!cleanup.succeeded()) return cleanup;
        return result(HidHideSessionResultCode::BackendFailure,
                      boundedDiagnostic("HidHide session apply failed and prior state was restored: " + error));
    }

    HidHideSessionSnapshot observed;
    if (!platform_->readState(observed, error)) {
        phase_ = HidHideSessionPhase::RollingBack;
        const auto cleanup = rollbackInternal(false);
        if (!cleanup.succeeded()) return cleanup;
        return result(HidHideSessionResultCode::VerificationFailed,
                      boundedDiagnostic("HidHide apply verification read failed: " + error));
    }
    if (!sameSnapshot(observed, plan_->applied)) {
        phase_ = HidHideSessionPhase::RollingBack;
        const auto cleanup = rollbackInternal(false);
        if (!cleanup.succeeded()) return cleanup;
        return result(HidHideSessionResultCode::VerificationFailed,
                      "HidHide persistent state did not match the exact prepared session plan");
    }

    // Device IDs are added only through HidHide's process-lifetime session
    // blacklist. No HydraSeat device is appended to the persistent blacklist.
    // Treat the session-list IOCTL as side-effectful before it returns. A
    // backend/driver may partially add entries and still report failure, so a
    // failed call must take the same verified-clear path as a successful one.
    sessionEntriesActive_ = true;
    if (!platform_->addSessionBlacklist(plan_->request.deviceInstanceIds, error)) {
        phase_ = HidHideSessionPhase::RollingBack;
        const auto cleanup = rollbackInternal(false);
        if (!cleanup.succeeded()) return cleanup;
        return result(HidHideSessionResultCode::BackendFailure,
                      boundedDiagnostic("HidHide session blacklist apply failed and prior state was verified clean: " + error));
    }

    phase_ = HidHideSessionPhase::Active;
    return result(HidHideSessionResultCode::Ok,
                  "HidHide persistent state and process-lifetime session blacklist applied");
}

HidHideSessionResult HidHideSessionTransaction::expireIfNeeded(
    std::uint64_t nowMilliseconds) {
    if (phase_ != HidHideSessionPhase::Active || !plan_) {
        return result(HidHideSessionResultCode::AlreadySatisfied,
                      "no active HidHide session requires expiry cleanup");
    }
    if (nowMilliseconds < plan_->expiryAtMilliseconds) {
        return result(HidHideSessionResultCode::AlreadySatisfied,
                      "HidHide session expiry has not elapsed");
    }
    return rollbackInternal(true);
}

HidHideSessionResult HidHideSessionTransaction::rollback() {
    return rollbackInternal(false);
}

HidHideSessionResult HidHideSessionTransaction::rollbackInternal(bool expired) {
    if (!plan_) {
        phase_ = HidHideSessionPhase::Idle;
        return result(expired ? HidHideSessionResultCode::Expired
                              : HidHideSessionResultCode::AlreadySatisfied,
                      expired ? "HidHide session already expired and clean"
                              : "HidHide session is already clean");
    }
    if (phase_ == HidHideSessionPhase::RecoveryRequired) {
        return result(HidHideSessionResultCode::RecoveryRequired,
                      "HidHide cleanup is already recovery-required");
    }

    // Prepared plans are read-only. Cancelling one needs no backend write.
    if (phase_ == HidHideSessionPhase::Prepared) {
        plan_.reset();
        phase_ = HidHideSessionPhase::Idle;
        return result(expired ? HidHideSessionResultCode::Expired
                              : HidHideSessionResultCode::Ok,
                      expired ? "prepared HidHide session expired without mutation"
                              : "prepared HidHide session cancelled without mutation");
    }

    if (!platform_) {
        phase_ = HidHideSessionPhase::RecoveryRequired;
        return result(HidHideSessionResultCode::RecoveryRequired,
                      "HidHide cleanup lost its platform owner");
    }

    phase_ = HidHideSessionPhase::RollingBack;
    std::string error;
    std::string sessionClearError;
    bool sessionCleared = true;
    if (sessionEntriesActive_) {
        sessionCleared = platform_->clearSessionBlacklist(sessionClearError);
        if (sessionCleared) sessionEntriesActive_ = false;
    }

    HidHideSessionSnapshot current;
    if (!platform_->readState(current, error)) {
        phase_ = HidHideSessionPhase::RecoveryRequired;
        std::string diagnostic = "HidHide rollback preflight read failed: " + error;
        if (!sessionCleared) diagnostic += "; session blacklist clear failed: " + sessionClearError;
        return result(HidHideSessionResultCode::RecoveryRequired,
                      boundedDiagnostic(std::move(diagnostic)));
    }
    current = normalizedSnapshot(std::move(current));

    if (sameSnapshot(current, plan_->before)) {
        if (!sessionCleared) {
            phase_ = HidHideSessionPhase::RecoveryRequired;
            return result(HidHideSessionResultCode::RecoveryRequired,
                          boundedDiagnostic("persistent state is restored but session blacklist clear failed: " + sessionClearError));
        }
        plan_.reset();
        phase_ = HidHideSessionPhase::Idle;
        return result(expired ? HidHideSessionResultCode::Expired
                              : HidHideSessionResultCode::AlreadySatisfied,
                      expired ? "HidHide session expired; prior state was already restored"
                              : "HidHide prior state is already restored");
    }

    // Never clobber a third-party/user change that occurred after activation.
    // A mismatch against both the before and exact applied state is surfaced for
    // explicit recovery rather than silently restoring a stale snapshot.
    if (!sameSnapshot(current, plan_->applied)) {
        phase_ = HidHideSessionPhase::RecoveryRequired;
        std::string diagnostic =
            "HidHide persistent state changed outside the owned transaction; stale snapshot restore refused";
        if (!sessionCleared) diagnostic += "; session blacklist clear failed: " + sessionClearError;
        return result(HidHideSessionResultCode::RecoveryRequired,
                      boundedDiagnostic(std::move(diagnostic)));
    }

    if (!platform_->writeState(plan_->before, error)) {
        phase_ = HidHideSessionPhase::RecoveryRequired;
        std::string diagnostic = "HidHide snapshot restore failed: " + error;
        if (!sessionCleared) diagnostic += "; session blacklist clear failed: " + sessionClearError;
        return result(HidHideSessionResultCode::RecoveryRequired,
                      boundedDiagnostic(std::move(diagnostic)));
    }

    HidHideSessionSnapshot restored;
    if (!platform_->readState(restored, error) ||
        !sameSnapshot(restored, plan_->before)) {
        phase_ = HidHideSessionPhase::RecoveryRequired;
        std::string diagnostic = error.empty()
            ? "HidHide snapshot restore verification failed"
            : "HidHide snapshot restore verification failed: " + error;
        if (!sessionCleared) diagnostic += "; session blacklist clear failed: " + sessionClearError;
        return result(HidHideSessionResultCode::RecoveryRequired,
                      boundedDiagnostic(std::move(diagnostic)));
    }
    if (!sessionCleared) {
        phase_ = HidHideSessionPhase::RecoveryRequired;
        return result(HidHideSessionResultCode::RecoveryRequired,
                      boundedDiagnostic("persistent state restored but session blacklist clear failed: " + sessionClearError));
    }

    plan_.reset();
    phase_ = HidHideSessionPhase::Idle;
    return result(expired ? HidHideSessionResultCode::Expired
                          : HidHideSessionResultCode::Ok,
                  expired ? "HidHide session expiry cleared process-lifetime entries and restored prior state"
                          : "HidHide session cleared process-lifetime entries and restored prior state");
}

HidHideSessionResult HidHideSessionTransaction::result(
    HidHideSessionResultCode code,
    std::string diagnostic) const {
    return {code, phase_, plan_, boundedDiagnostic(std::move(diagnostic))};
}

bool validateHidHideSessionRequest(const HidHideSessionRequest& request,
                                   std::string& error) {
    error.clear();
    if (request.generation == 0) {
        error = "HidHide session generation must be nonzero";
        return false;
    }
    if (!request.replacementPathVerified || !request.recoveryReady) {
        error = "replacement input path and independent recovery must be verified before session cloaking";
        return false;
    }
    if (request.deviceInstanceIds.empty() ||
        request.deviceInstanceIds.size() > kHidHideSessionMaxRequestedDevices) {
        error = "HidHide session requires between one and sixteen explicit device instance IDs";
        return false;
    }
    if (request.allowedApplications.empty() ||
        request.allowedApplications.size() > kHidHideSessionMaxRequestedApplications) {
        error = "HidHide session requires between one and eight explicit allowed applications";
        return false;
    }
    for (const auto& value : request.deviceInstanceIds) {
        if (!boundedText(value)) {
            error = "HidHide device instance IDs must be nonempty and bounded";
            return false;
        }
    }
    for (const auto& value : request.allowedApplications) {
        if (!boundedText(value)) {
            error = "HidHide allowed application paths must be nonempty and bounded";
            return false;
        }
    }
    if (normalizedUnique(request.deviceInstanceIds, false).size() !=
        request.deviceInstanceIds.size()) {
        error = "HidHide session device instance IDs must be unique";
        return false;
    }
    if (normalizedUnique(request.allowedApplications, true).size() !=
        request.allowedApplications.size()) {
        error = "HidHide session allowed application paths must be unique";
        return false;
    }
    if (request.physicalAcceptanceEvidence.has_value()) {
        const bool authorized = request.physicalEvidenceSeatId == 0u
            ? validatePhase3HardwareAcceptanceEvidenceForDevices(
                  *request.physicalAcceptanceEvidence,
                  request.deviceInstanceIds, 0u, &error)
            : validatePhase3HardwareAcceptanceEvidenceForSeatDevices(
                  *request.physicalAcceptanceEvidence,
                  request.physicalEvidenceSeatId,
                  request.deviceInstanceIds, 0u, &error);
        if (!authorized) {
            error = boundedDiagnostic(
                "typed P3-HW physical evidence does not authorize this HidHide request: " +
                error);
            return false;
        }
    }
    if (request.expiryMilliseconds < kHidHideSessionMinExpiryMs ||
        request.expiryMilliseconds > kHidHideSessionMaxExpiryMs) {
        error = "HidHide session expiry must be between one and sixty seconds";
        return false;
    }
    if (!request.spareRecoveryInputPresent &&
        request.expiryMilliseconds > kHidHideSessionNoSpareInputMaxExpiryMs) {
        error = "without a spare recovery input, HidHide session expiry must not exceed ten seconds";
        return false;
    }
    return true;
}

bool equivalentHidHideSessionSnapshots(
    const HidHideSessionSnapshot& left,
    const HidHideSessionSnapshot& right) {
    return sameSnapshot(left, right);
}

HidHideSessionSnapshot makeHidHideSessionAppliedState(
    const HidHideSessionSnapshot& before,
    const HidHideSessionRequest& request) {
    auto applied = normalizedSnapshot(before);
    applied.active = true;
    // The persistent blacklist is preserved byte-for-byte at the logical
    // identity level. Requested HydraSeat devices are owned only by HidHide's
    // process-lifetime session blacklist.
    mergeUnique(applied.allowedApplications,
                request.allowedApplications, true);
    return applied;
}

watchdog::RollbackActionDescriptor makeHidHideSessionRollbackAction(
    std::uint32_t actionId,
    std::uint32_t activationOrdinal,
    std::uint32_t timeoutMilliseconds,
    std::uint64_t generation,
    std::uint64_t snapshotResourceId) noexcept {
    watchdog::RollbackActionDescriptor action;
    action.actionId = actionId;
    action.kind = watchdog::RollbackActionKind::RestoreSnapshotState;
    action.activationOrdinal = activationOrdinal;
    action.timeoutMilliseconds = timeoutMilliseconds;
    action.generation = generation;
    action.resourceId = snapshotResourceId;
    return action;
}

std::string_view hidHideSessionPhaseName(HidHideSessionPhase phase) noexcept {
    switch (phase) {
        case HidHideSessionPhase::Idle: return "idle";
        case HidHideSessionPhase::Prepared: return "prepared";
        case HidHideSessionPhase::Active: return "active";
        case HidHideSessionPhase::RollingBack: return "rolling-back";
        case HidHideSessionPhase::RecoveryRequired: return "recovery-required";
    }
    return "unknown";
}

std::string_view hidHideSessionResultCodeName(
    HidHideSessionResultCode code) noexcept {
    switch (code) {
        case HidHideSessionResultCode::Ok: return "ok";
        case HidHideSessionResultCode::AlreadySatisfied: return "already-satisfied";
        case HidHideSessionResultCode::InvalidRequest: return "invalid-request";
        case HidHideSessionResultCode::UnsupportedState: return "unsupported-state";
        case HidHideSessionResultCode::PhysicalGateRequired: return "physical-gate-required";
        case HidHideSessionResultCode::NativeMutationDisabled: return "native-mutation-disabled";
        case HidHideSessionResultCode::BackendFailure: return "backend-failure";
        case HidHideSessionResultCode::VerificationFailed: return "verification-failed";
        case HidHideSessionResultCode::RecoveryRequired: return "recovery-required";
        case HidHideSessionResultCode::Expired: return "expired";
        case HidHideSessionResultCode::RecoveryNotArmed: return "recovery-not-armed";
    }
    return "unknown";
}

} // namespace hydra
