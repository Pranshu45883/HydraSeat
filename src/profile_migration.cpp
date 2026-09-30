#include "hydra/profile_migration.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace hydra::profile {
namespace {

inline constexpr std::size_t kMaximumLegacyShareableResources = 128u;
inline constexpr std::size_t kMaximumDiagnosticPathBytes = 1024u;
inline constexpr std::size_t kMaximumDiagnosticNoteBytes = 2048u;

struct MigrationException final : std::runtime_error {
    MigrationException(ProfileMigrationResult code, std::string message)
        : std::runtime_error(std::move(message)), result(code) {}

    ProfileMigrationResult result;
};

[[noreturn]] void fail(ProfileMigrationResult result, std::string message) {
    throw MigrationException(result, std::move(message));
}

ProfileMigrationOutcome outcome(ProfileMigrationResult result,
                                std::string message,
                                const ProfileMigrationReport* report = nullptr) {
    ProfileMigrationOutcome value;
    value.result = result;
    value.message = std::move(message);
    if (report != nullptr) value.report = *report;
    return value;
}

struct JsonNumber {
    std::string text;
};

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    std::variant<std::nullptr_t, bool, JsonNumber, std::string, Array, Object> value;
};

bool validUtf8(std::string_view input) {
    for (std::size_t index = 0u; index < input.size();) {
        const auto first = static_cast<unsigned char>(input[index++]);
        std::uint32_t codePoint = 0u;
        unsigned continuation = 0u;
        std::uint32_t minimum = 0u;
        if (first <= 0x7fu) {
            codePoint = first;
        } else if ((first & 0xe0u) == 0xc0u) {
            codePoint = first & 0x1fu;
            continuation = 1u;
            minimum = 0x80u;
        } else if ((first & 0xf0u) == 0xe0u) {
            codePoint = first & 0x0fu;
            continuation = 2u;
            minimum = 0x800u;
        } else if ((first & 0xf8u) == 0xf0u) {
            codePoint = first & 0x07u;
            continuation = 3u;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (index + continuation > input.size()) return false;
        for (unsigned count = 0u; count < continuation; ++count) {
            const auto current = static_cast<unsigned char>(input[index++]);
            if ((current & 0xc0u) != 0x80u) return false;
            codePoint = (codePoint << 6u) | (current & 0x3fu);
        }
        if (codePoint < minimum || codePoint > 0x10ffffu ||
            (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skip();
        auto result = parseValue(0u);
        skip();
        if (position_ != text_.size()) syntax("trailing content");
        return result;
    }

private:
    [[noreturn]] void syntax(std::string message) const {
        fail(ProfileMigrationResult::LegacyParseError,
             std::move(message) + " at byte " + std::to_string(position_));
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
            if (raw == static_cast<unsigned char>('"')) {
                if (!validUtf8(output)) syntax("invalid UTF-8 string");
                return output;
            }
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

    JsonValue parseValue(unsigned depth) {
        if (depth > 48u) syntax("nesting too deep");
        skip();
        if (position_ >= text_.size()) syntax("value expected");
        const char current = text_[position_];
        if (current == '"') return JsonValue{parseString()};
        if (current == '{') {
            ++position_;
            JsonValue::Object object;
            skip();
            if (consume('}')) return JsonValue{std::move(object)};
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
            return JsonValue{std::move(object)};
        }
        if (current == '[') {
            ++position_;
            JsonValue::Array array;
            skip();
            if (consume(']')) return JsonValue{std::move(array)};
            for (;;) {
                array.push_back(parseValue(depth + 1u));
                skip();
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
        if (current == '-' || std::isdigit(static_cast<unsigned char>(current)) != 0) {
            return JsonValue{parseNumber()};
        }
        syntax("invalid value");
    }

    std::string_view text_;
    std::size_t position_{0u};
};

const JsonValue::Object& asObject(const JsonValue& value, std::string_view field) {
    const auto* object = std::get_if<JsonValue::Object>(&value.value);
    if (object == nullptr) {
        fail(ProfileMigrationResult::LegacyValidationError,
             std::string(field) + " must be an object");
    }
    return *object;
}

const JsonValue::Array& asArray(const JsonValue& value, std::string_view field) {
    const auto* array = std::get_if<JsonValue::Array>(&value.value);
    if (array == nullptr) {
        fail(ProfileMigrationResult::LegacyValidationError,
             std::string(field) + " must be an array");
    }
    return *array;
}

const std::string& asString(const JsonValue& value, std::string_view field) {
    const auto* text = std::get_if<std::string>(&value.value);
    if (text == nullptr) {
        fail(ProfileMigrationResult::LegacyValidationError,
             std::string(field) + " must be a string");
    }
    return *text;
}

bool asBool(const JsonValue& value, std::string_view field) {
    const auto* boolean = std::get_if<bool>(&value.value);
    if (boolean == nullptr) {
        fail(ProfileMigrationResult::LegacyValidationError,
             std::string(field) + " must be a boolean");
    }
    return *boolean;
}

std::uint64_t asUnsigned(const JsonValue& value, std::string_view field) {
    const auto* number = std::get_if<JsonNumber>(&value.value);
    if (number == nullptr || number->text.empty() || number->text.front() == '-') {
        fail(ProfileMigrationResult::LegacyValidationError,
             std::string(field) + " must be an unsigned integer");
    }
    std::uint64_t result = 0u;
    const auto converted = std::from_chars(number->text.data(),
                                            number->text.data() + number->text.size(),
                                            result);
    if (converted.ec != std::errc{} ||
        converted.ptr != number->text.data() + number->text.size()) {
        fail(ProfileMigrationResult::LegacyValidationError,
             std::string(field) + " integer is out of range");
    }
    return result;
}

const JsonValue& required(const JsonValue::Object& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        fail(ProfileMigrationResult::LegacyValidationError,
             std::string("missing legacy field: ") + key);
    }
    return found->second;
}

const JsonValue* optional(const JsonValue::Object& object, const char* key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

std::string quoteJson(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 2u);
    output.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (const char raw : input) {
        const auto current = static_cast<unsigned char>(raw);
        switch (current) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (current < 0x20u) {
                output += "\\u00";
                output.push_back(hex[current >> 4u]);
                output.push_back(hex[current & 0x0fu]);
            } else {
                output.push_back(static_cast<char>(current));
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

std::string canonicalJson(const JsonValue& value) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) return "null";
    if (const auto* boolean = std::get_if<bool>(&value.value)) {
        return *boolean ? "true" : "false";
    }
    if (const auto* number = std::get_if<JsonNumber>(&value.value)) return number->text;
    if (const auto* text = std::get_if<std::string>(&value.value)) return quoteJson(*text);
    if (const auto* array = std::get_if<JsonValue::Array>(&value.value)) {
        std::string output = "[";
        for (std::size_t index = 0u; index < array->size(); ++index) {
            if (index != 0u) output.push_back(',');
            output += canonicalJson((*array)[index]);
        }
        output.push_back(']');
        return output;
    }
    const auto& object = std::get<JsonValue::Object>(value.value);
    std::string output = "{";
    bool first = true;
    for (const auto& [key, item] : object) {
        if (!first) output.push_back(',');
        first = false;
        output += quoteJson(key);
        output.push_back(':');
        output += canonicalJson(item);
    }
    output.push_back('}');
    return output;
}

std::wstring fromUtf8(std::string_view input) {
    std::wstring output;
    output.reserve(input.size());
    for (std::size_t index = 0u; index < input.size();) {
        const auto first = static_cast<unsigned char>(input[index++]);
        std::uint32_t codePoint = 0u;
        unsigned continuation = 0u;
        if (first <= 0x7fu) {
            codePoint = first;
        } else if ((first & 0xe0u) == 0xc0u) {
            codePoint = first & 0x1fu;
            continuation = 1u;
        } else if ((first & 0xf0u) == 0xe0u) {
            codePoint = first & 0x0fu;
            continuation = 2u;
        } else {
            codePoint = first & 0x07u;
            continuation = 3u;
        }
        for (unsigned count = 0u; count < continuation; ++count) {
            codePoint = (codePoint << 6u) |
                        (static_cast<unsigned char>(input[index++]) & 0x3fu);
        }
        if constexpr (sizeof(wchar_t) == 2u) {
            if (codePoint <= 0xffffu) {
                output.push_back(static_cast<wchar_t>(codePoint));
            } else {
                codePoint -= 0x10000u;
                output.push_back(static_cast<wchar_t>(0xd800u + (codePoint >> 10u)));
                output.push_back(static_cast<wchar_t>(0xdc00u + (codePoint & 0x3ffu)));
            }
        } else {
            output.push_back(static_cast<wchar_t>(codePoint));
        }
    }
    return output;
}

std::wstring normalizeResourceId(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        if (ch >= L'A' && ch <= L'Z') {
            return static_cast<wchar_t>(ch - L'A' + L'a');
        }
        return ch;
    });
    return value;
}

enum class LegacyResourceType : std::uint8_t {
    Display = 0,
    Keyboard = 1,
    Mouse = 2,
    Controller = 3,
    AudioOutput = 4,
    AudioInput = 5,
};

LegacyResourceType parseResourceType(std::string_view type) {
    if (type == "display") return LegacyResourceType::Display;
    if (type == "keyboard") return LegacyResourceType::Keyboard;
    if (type == "mouse") return LegacyResourceType::Mouse;
    if (type == "controller") return LegacyResourceType::Controller;
    if (type == "audio_output") return LegacyResourceType::AudioOutput;
    if (type == "audio_input") return LegacyResourceType::AudioInput;
    fail(ProfileMigrationResult::LegacyValidationError,
         "unknown legacy shareable resource type");
}

struct ResourceKey {
    LegacyResourceType type{LegacyResourceType::Display};
    std::wstring normalizedId;

    bool operator<(const ResourceKey& other) const noexcept {
        if (type != other.type) {
            return static_cast<std::uint8_t>(type) <
                   static_cast<std::uint8_t>(other.type);
        }
        return normalizedId < other.normalizedId;
    }
};

ResourceKey resourceKey(LegacyResourceType type, const std::wstring& id) {
    return {type, normalizeResourceId(id)};
}

std::vector<std::wstring> readDeviceArray(const JsonValue& value,
                                          std::string_view field) {
    const auto& array = asArray(value, field);
    if (array.size() > kMaximumDeviceIdsPerSeat) {
        fail(ProfileMigrationResult::LegacyValidationError,
             std::string(field) + " exceeds v1 device count bound");
    }
    std::vector<std::wstring> output;
    output.reserve(array.size());
    std::set<std::wstring> normalized;
    for (const auto& item : array) {
        const auto& utf8 = asString(item, field);
        auto wide = fromUtf8(utf8);
        if (wide.empty()) {
            fail(ProfileMigrationResult::LegacyValidationError,
                 std::string(field) + " contains an empty device ID");
        }
        if (!normalized.insert(normalizeResourceId(wide)).second) {
            fail(ProfileMigrationResult::LegacyValidationError,
                 std::string(field) + " contains a duplicate device ID");
        }
        output.push_back(std::move(wide));
    }
    return output;
}

std::optional<std::wstring> readOptionalWide(const JsonValue& value,
                                             std::string_view field) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) return std::nullopt;
    auto result = fromUtf8(asString(value, field));
    if (result.empty()) {
        fail(ProfileMigrationResult::LegacyValidationError,
             std::string(field) + " must not be empty when present");
    }
    return result;
}

std::string jsonPointerToken(std::string_view token) {
    std::string output;
    output.reserve(token.size());
    for (const char ch : token) {
        if (ch == '~') output += "~0";
        else if (ch == '/') output += "~1";
        else output.push_back(ch);
    }
    return output;
}

void addDiagnostic(ProfileMigrationReport& report,
                   ProfileMigrationDiagnosticKind kind,
                   std::string path,
                   std::string valueJson,
                   std::string note) {
    if (report.diagnostics.size() >= kMaximumProfileMigrationDiagnostics) {
        fail(ProfileMigrationResult::ReportBoundsExceeded,
             "too many unmapped legacy values for bounded migration report");
    }
    if (path.empty() || path.size() > kMaximumDiagnosticPathBytes ||
        valueJson.size() > kMaximumProfileMigrationDiagnosticValueBytes ||
        note.size() > kMaximumDiagnosticNoteBytes) {
        fail(ProfileMigrationResult::ReportBoundsExceeded,
             "unmapped legacy value exceeds migration report bounds");
    }
    report.diagnostics.push_back(
        {kind, std::move(path), std::move(valueJson), std::move(note)});
}

template <std::size_t Count>
void reportUnknownFields(const JsonValue::Object& object,
                         const std::array<std::string_view, Count>& known,
                         std::string_view basePointer,
                         ProfileMigrationReport& report) {
    for (const auto& [key, value] : object) {
        if (std::find(known.begin(), known.end(), key) != known.end()) continue;
        addDiagnostic(report,
                      ProfileMigrationDiagnosticKind::UnmappedLegacyField,
                      std::string(basePointer) + "/" + jsonPointerToken(key),
                      canonicalJson(value),
                      "legacy field has no v1 mapping and was not interpreted");
    }
}

std::uint64_t fnv1a64(std::string_view bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char raw : bytes) {
        hash ^= static_cast<unsigned char>(raw);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex64(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(16u, '0');
    for (std::size_t index = 0u; index < output.size(); ++index) {
        const std::size_t shift = (output.size() - index - 1u) * 4u;
        output[index] = digits[(value >> shift) & 0x0fu];
    }
    return output;
}

void validateReport(const ProfileMigrationReport& report) {
    if (report.reportVersion != kProfileMigrationReportVersion ||
        report.legacySchemaVersion != kLegacyWorkspaceSchemaVersion ||
        report.targetSchemaVersion != kProfileSchemaVersion) {
        fail(ProfileMigrationResult::ReportBoundsExceeded,
             "migration report version fields are invalid");
    }
    if (report.sourceByteCount > kMaximumSchemaDocumentBytes ||
        report.migratedSeatCount == 0u ||
        report.migratedSeatCount > kMaximumPersistedSeats ||
        report.diagnostics.size() > kMaximumProfileMigrationDiagnostics) {
        fail(ProfileMigrationResult::ReportBoundsExceeded,
             "migration report count or source bounds are invalid");
    }
    for (const auto& item : report.diagnostics) {
        if (item.jsonPointer.empty() ||
            item.jsonPointer.size() > kMaximumDiagnosticPathBytes ||
            item.valueJson.size() > kMaximumProfileMigrationDiagnosticValueBytes ||
            item.note.size() > kMaximumDiagnosticNoteBytes ||
            !validUtf8(item.jsonPointer) || !validUtf8(item.valueJson) ||
            !validUtf8(item.note)) {
            fail(ProfileMigrationResult::ReportBoundsExceeded,
                 "migration diagnostic is invalid or exceeds bounds");
        }
    }
}

struct EncodedBundle {
    std::string seats;
    std::string players;
    std::string games;
    std::string setups;
    std::string report;
};

bool encodeBundle(const ProfileMigrationBundle& bundle,
                  EncodedBundle& encoded,
                  std::string& error) {
    SchemaDiagnostic diagnostic;
    encoded.seats = encodeSeatConfigDocument(bundle.seats, &diagnostic);
    if (!diagnostic.succeeded()) {
        error = "SeatConfigDocument encode failed: " + diagnostic.message;
        return false;
    }
    encoded.players = encodePlayerProfileDocument(bundle.players, &diagnostic);
    if (!diagnostic.succeeded()) {
        error = "PlayerProfileDocument encode failed: " + diagnostic.message;
        return false;
    }
    encoded.games = encodeGameRecordDocument(bundle.games, &diagnostic);
    if (!diagnostic.succeeded()) {
        error = "GameRecordDocument encode failed: " + diagnostic.message;
        return false;
    }
    encoded.setups = encodeTwoPlayerSetupDocument(bundle.setups, &diagnostic);
    if (!diagnostic.succeeded()) {
        error = "TwoPlayerSetupDocument encode failed: " + diagnostic.message;
        return false;
    }
    encoded.report = encodeProfileMigrationReport(bundle.report, &error);
    return !encoded.report.empty();
}

bool readFileBounded(const std::filesystem::path& path,
                     std::size_t maximumBytes,
                     std::string& bytes,
                     std::string& error,
                     bool& tooLarge) {
    tooLarge = false;
    std::error_code fileError;
    const auto size = std::filesystem::file_size(path, fileError);
    if (fileError) {
        error = "could not determine file size: " + fileError.message();
        return false;
    }
    if (size > maximumBytes) {
        tooLarge = true;
        error = "file exceeds bounded size";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open file for reading";
        return false;
    }
    bytes.assign(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty()) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
            error = "file changed or ended during read";
            return false;
        }
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        error = "file changed during bounded read";
        return false;
    }
    return true;
}

bool durableFlush(const std::filesystem::path& path) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE |
                                          FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const BOOL flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    return flushed != FALSE;
#else
    (void)path;
    return true;
#endif
}

bool writeFile(const std::filesystem::path& path,
               std::string_view bytes,
               std::string& error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not open staged file for writing";
        return false;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
        error = "failed while writing staged file";
        return false;
    }
    output.close();
    if (!output) {
        error = "failed while closing staged file";
        return false;
    }
    if (!durableFlush(path)) {
        error = "failed to durably flush staged file";
        return false;
    }
    return true;
}

bool movePath(const std::filesystem::path& from,
              const std::filesystem::path& to,
              std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE) {
        return true;
    }
    error = "MoveFileExW failed with error " + std::to_string(GetLastError());
    return false;
#else
    std::error_code moveError;
    std::filesystem::rename(from, to, moveError);
    if (moveError) {
        error = "rename failed: " + moveError.message();
        return false;
    }
    return true;
#endif
}

bool removeTree(const std::filesystem::path& path, std::string& error) {
    std::error_code existsError;
    const bool present = std::filesystem::exists(path, existsError);
    if (existsError) {
        error = "could not inspect path for cleanup: " + existsError.message();
        return false;
    }
    if (!present) return true;
    std::error_code removeError;
    (void)std::filesystem::remove_all(path, removeError);
    if (removeError) {
        error = "could not remove migration-owned path: " + removeError.message();
        return false;
    }
    return true;
}

std::filesystem::path withSuffix(const std::filesystem::path& path,
                                 std::string_view suffix) {
    auto result = path;
    result += std::filesystem::path(std::string(suffix));
    return result;
}

std::filesystem::path bundlePath(const std::filesystem::path& directory,
                                 std::string_view fileName) {
    return directory / std::filesystem::path(std::string(fileName));
}

bool decodeAndCompareSeat(std::string_view bytes,
                          const SeatConfigDocument& expected,
                          std::string& error) {
    SeatConfigDocument decoded;
    const auto diagnostic = decodeSeatConfigDocument(bytes, decoded);
    if (!diagnostic.succeeded() || decoded != expected) {
        error = "staged SeatConfigDocument failed decode/equality validation: " +
                diagnostic.message;
        return false;
    }
    return true;
}

bool decodeAndComparePlayers(std::string_view bytes,
                             const PlayerProfileDocument& expected,
                             std::string& error) {
    PlayerProfileDocument decoded;
    const auto diagnostic = decodePlayerProfileDocument(bytes, decoded);
    if (!diagnostic.succeeded() || decoded != expected) {
        error = "staged PlayerProfileDocument failed decode/equality validation: " +
                diagnostic.message;
        return false;
    }
    return true;
}

bool decodeAndCompareGames(std::string_view bytes,
                           const GameRecordDocument& expected,
                           std::string& error) {
    GameRecordDocument decoded;
    const auto diagnostic = decodeGameRecordDocument(bytes, decoded);
    if (!diagnostic.succeeded() || decoded != expected) {
        error = "staged GameRecordDocument failed decode/equality validation: " +
                diagnostic.message;
        return false;
    }
    return true;
}

bool decodeAndCompareSetups(std::string_view bytes,
                            const TwoPlayerSetupDocument& expected,
                            std::string& error) {
    TwoPlayerSetupDocument decoded;
    const auto diagnostic = decodeTwoPlayerSetupDocument(bytes, decoded);
    if (!diagnostic.succeeded() || decoded != expected) {
        error = "staged TwoPlayerSetupDocument failed decode/equality validation: " +
                diagnostic.message;
        return false;
    }
    return true;
}

bool validateBundleDirectory(const std::filesystem::path& directory,
                             std::string_view legacyBytes,
                             const ProfileMigrationBundle& bundle,
                             const EncodedBundle& encoded,
                             std::string& error) {
    struct ExpectedFile {
        std::string_view name;
        const std::string* bytes;
    };
    const std::array<ExpectedFile, 5u> expectedFiles{{
        {kMigratedSeatConfigFileName, &encoded.seats},
        {kMigratedPlayerProfilesFileName, &encoded.players},
        {kMigratedGameRecordsFileName, &encoded.games},
        {kMigratedTwoPlayerSetupsFileName, &encoded.setups},
        {kProfileMigrationReportFileName, &encoded.report},
    }};

    for (const auto& expected : expectedFiles) {
        std::string actual;
        bool tooLarge = false;
        const std::size_t maximum =
            expected.name == kProfileMigrationReportFileName
                ? kMaximumProfileMigrationReportBytes
                : kMaximumSchemaDocumentBytes;
        if (!readFileBounded(bundlePath(directory, expected.name), maximum,
                             actual, error, tooLarge)) {
            return false;
        }
        if (actual != *expected.bytes) {
            error = std::string(expected.name) +
                    " does not match deterministic planned bytes";
            return false;
        }
    }

    std::string backup;
    bool backupTooLarge = false;
    if (!readFileBounded(bundlePath(directory, kLegacyWorkspaceBackupFileName),
                         kMaximumSchemaDocumentBytes, backup, error,
                         backupTooLarge)) {
        return false;
    }
    if (backup != legacyBytes) {
        error = "legacy backup is not byte-for-byte identical to source";
        return false;
    }

    if (!decodeAndCompareSeat(encoded.seats, bundle.seats, error) ||
        !decodeAndComparePlayers(encoded.players, bundle.players, error) ||
        !decodeAndCompareGames(encoded.games, bundle.games, error) ||
        !decodeAndCompareSetups(encoded.setups, bundle.setups, error)) {
        return false;
    }

    std::error_code iteratorError;
    std::size_t fileCount = 0u;
    for (std::filesystem::directory_iterator iterator(directory, iteratorError), end;
         iterator != end && !iteratorError; iterator.increment(iteratorError)) {
        std::error_code typeError;
        if (!iterator->is_regular_file(typeError) || typeError) {
            error = "migration bundle contains a non-regular or unreadable entry";
            return false;
        }
        ++fileCount;
    }
    if (iteratorError) {
        error = "could not enumerate migration bundle: " + iteratorError.message();
        return false;
    }
    if (fileCount != 6u) {
        error = "migration bundle does not contain exactly six declared files";
        return false;
    }
    return true;
}

bool normalizedAbsolute(const std::filesystem::path& input,
                        std::filesystem::path& output,
                        std::string& error) {
    std::error_code absoluteError;
    output = std::filesystem::absolute(input, absoluteError).lexically_normal();
    if (absoluteError) {
        error = "could not normalize path: " + absoluteError.message();
        return false;
    }
    return true;
}

bool pathWithin(const std::filesystem::path& child,
                const std::filesystem::path& parent) {
    auto childIt = child.begin();
    auto parentIt = parent.begin();
    for (; parentIt != parent.end(); ++parentIt, ++childIt) {
        if (childIt == child.end() || *childIt != *parentIt) return false;
    }
    return true;
}

bool restorePreviousBundle(const std::filesystem::path& destination,
                           const std::filesystem::path& rollback,
                           bool hadPrevious,
                           std::string& error) {
    if (!removeTree(destination, error)) return false;
    if (hadPrevious && !movePath(rollback, destination, error)) return false;
    return true;
}

} // namespace

ProfileMigrationOutcome planLegacyWorkspaceMigration(
    std::string_view legacyWorkspaceJson,
    ProfileMigrationBundle& bundle) {
    if (legacyWorkspaceJson.size() > kMaximumSchemaDocumentBytes) {
        return outcome(ProfileMigrationResult::SourceTooLarge,
                       "legacy workspace document exceeds bounded size");
    }

    try {
        const JsonValue rootValue = JsonParser(legacyWorkspaceJson).parse();
        const auto& root = asObject(rootValue, "legacy workspace root");
        const auto schemaVersion = asUnsigned(required(root, "schema_version"),
                                              "schema_version");
        if (schemaVersion != kLegacyWorkspaceSchemaVersion) {
            fail(ProfileMigrationResult::LegacyValidationError,
                 "unsupported legacy workspace schema_version");
        }

        ProfileMigrationBundle candidate;
        candidate.report.sourceByteCount =
            static_cast<std::uint64_t>(legacyWorkspaceJson.size());
        candidate.report.sourceFnv1a64 = fnv1a64(legacyWorkspaceJson);

        static constexpr std::array<std::string_view, 4u> rootFields{
            "schema_version", "management_seat_id", "shareable_resources", "seats"};
        reportUnknownFields(root, rootFields, "", candidate.report);

        std::set<ResourceKey> shareableResources;
        if (const auto* shareableValue = optional(root, "shareable_resources")) {
            const auto& shareable = asArray(*shareableValue, "shareable_resources");
            if (shareable.size() > kMaximumLegacyShareableResources) {
                fail(ProfileMigrationResult::ReportBoundsExceeded,
                     "legacy shareable resource list exceeds migration report bound");
            }
            for (std::size_t index = 0u; index < shareable.size(); ++index) {
                const auto& item = asObject(shareable[index], "shareable resource");
                const auto type = parseResourceType(
                    asString(required(item, "type"), "shareable resource type"));
                const auto id = fromUtf8(
                    asString(required(item, "id"), "shareable resource id"));
                if (id.empty()) {
                    fail(ProfileMigrationResult::LegacyValidationError,
                         "shareable resource id must not be empty");
                }
                shareableResources.insert(resourceKey(type, id));
                addDiagnostic(
                    candidate.report,
                    ProfileMigrationDiagnosticKind::LegacyShareableResource,
                    "/shareable_resources/" + std::to_string(index),
                    canonicalJson(shareable[index]),
                    "legacy shareability has no v1 schema field; assignments were preserved and the declaration is retained here");
            }
        }

        const auto& seatValues = asArray(required(root, "seats"), "seats");
        if (seatValues.empty() || seatValues.size() > kMaximumPersistedSeats) {
            fail(ProfileMigrationResult::LegacyValidationError,
                 "legacy profile must contain one or two Seats for v1 migration");
        }

        static constexpr std::array<std::string_view, 11u> seatFields{
            "id", "name", "active", "target_hwnd", "displays", "primary_display",
            "keyboards", "mice", "controllers", "audio_output", "audio_input"};
        std::set<SeatId> seatIds;
        std::map<ResourceKey, std::set<SeatId>> owners;

        auto registerList = [&](LegacyResourceType type,
                                SeatId seatId,
                                const std::vector<std::wstring>& values) {
            for (const auto& value : values) owners[resourceKey(type, value)].insert(seatId);
        };
        auto registerOptional = [&](LegacyResourceType type,
                                    SeatId seatId,
                                    const std::optional<std::wstring>& value) {
            if (value) owners[resourceKey(type, *value)].insert(seatId);
        };

        for (std::size_t index = 0u; index < seatValues.size(); ++index) {
            const auto& object = asObject(seatValues[index], "seat");
            const auto rawId = asUnsigned(required(object, "id"), "seat id");
            if (rawId == 0u || rawId > (std::numeric_limits<SeatId>::max)()) {
                fail(ProfileMigrationResult::LegacyValidationError,
                     "legacy Seat ID is out of range");
            }
            const SeatId seatId = static_cast<SeatId>(rawId);
            if (!seatIds.insert(seatId).second) {
                fail(ProfileMigrationResult::LegacyValidationError,
                     "legacy profile contains duplicate Seat ID");
            }

            PersistedSeatConfig seat;
            seat.seatId = seatId;
            seat.name = fromUtf8(asString(required(object, "name"), "seat name"));
            if (seat.name.empty()) {
                fail(ProfileMigrationResult::LegacyValidationError,
                     "legacy Seat name must not be empty");
            }
            seat.active = asBool(required(object, "active"), "seat active");
            const auto& targetHwnd = required(object, "target_hwnd");
            (void)asUnsigned(targetHwnd, "target_hwnd");
            addDiagnostic(candidate.report,
                          ProfileMigrationDiagnosticKind::DroppedRuntimeField,
                          "/seats/" + std::to_string(index) + "/target_hwnd",
                          canonicalJson(targetHwnd),
                          "transient legacy window identity was not migrated into stable Seat persistence");

            seat.displayIds = readDeviceArray(required(object, "displays"), "displays");
            seat.primaryDisplayId = readOptionalWide(
                required(object, "primary_display"), "primary_display");
            seat.keyboardIds = readDeviceArray(required(object, "keyboards"), "keyboards");
            seat.mouseIds = readDeviceArray(required(object, "mice"), "mice");
            seat.controllerIds = readDeviceArray(required(object, "controllers"),
                                                 "controllers");
            seat.audioOutputEndpointId = readOptionalWide(
                required(object, "audio_output"), "audio_output");
            seat.audioInputEndpointId = readOptionalWide(
                required(object, "audio_input"), "audio_input");

            if (seat.primaryDisplayId) {
                const auto wanted = normalizeResourceId(*seat.primaryDisplayId);
                if (std::none_of(seat.displayIds.begin(), seat.displayIds.end(),
                                 [&](const auto& display) {
                                     return normalizeResourceId(display) == wanted;
                                 })) {
                    fail(ProfileMigrationResult::LegacyValidationError,
                         "legacy primary_display is not assigned to its Seat");
                }
            } else if (!seat.displayIds.empty()) {
                fail(ProfileMigrationResult::LegacyValidationError,
                     "legacy Seat with displays must declare primary_display");
            }

            reportUnknownFields(object, seatFields,
                                "/seats/" + std::to_string(index),
                                candidate.report);

            registerList(LegacyResourceType::Display, seatId, seat.displayIds);
            registerList(LegacyResourceType::Keyboard, seatId, seat.keyboardIds);
            registerList(LegacyResourceType::Mouse, seatId, seat.mouseIds);
            registerList(LegacyResourceType::Controller, seatId, seat.controllerIds);
            registerOptional(LegacyResourceType::AudioOutput, seatId,
                             seat.audioOutputEndpointId);
            registerOptional(LegacyResourceType::AudioInput, seatId,
                             seat.audioInputEndpointId);
            candidate.seats.seats.push_back(std::move(seat));
        }

        for (const auto& [resource, resourceOwners] : owners) {
            if (resourceOwners.size() > 1u && !shareableResources.contains(resource)) {
                fail(ProfileMigrationResult::LegacyValidationError,
                     "legacy resource is assigned to multiple Seats without a shareable declaration");
            }
        }

        std::sort(candidate.seats.seats.begin(), candidate.seats.seats.end(),
                  [](const auto& left, const auto& right) {
                      return left.seatId < right.seatId;
                  });

        if (const auto* management = optional(root, "management_seat_id")) {
            const auto rawManagement = asUnsigned(*management, "management_seat_id");
            if (rawManagement == 0u ||
                rawManagement > (std::numeric_limits<SeatId>::max)()) {
                fail(ProfileMigrationResult::LegacyValidationError,
                     "legacy management_seat_id is out of range");
            }
            candidate.seats.managementSeatId = static_cast<SeatId>(rawManagement);
        } else {
            candidate.seats.managementSeatId = seatIds.contains(1u)
                                                   ? 1u
                                                   : *seatIds.begin();
            addDiagnostic(candidate.report,
                          ProfileMigrationDiagnosticKind::AppliedLegacyDefault,
                          "/management_seat_id",
                          std::to_string(candidate.seats.managementSeatId),
                          "legacy management Seat default was applied deterministically");
        }
        if (!seatIds.contains(candidate.seats.managementSeatId)) {
            fail(ProfileMigrationResult::LegacyValidationError,
                 "legacy management_seat_id does not reference a configured Seat");
        }

        const auto seatValidation = validateSeatConfigDocument(candidate.seats);
        if (!seatValidation.succeeded()) {
            fail(ProfileMigrationResult::LegacyValidationError,
                 "migrated Seat document is invalid: " + seatValidation.message);
        }
        for (const auto& validation : {
                 validatePlayerProfileDocument(candidate.players),
                 validateGameRecordDocument(candidate.games),
                 validateTwoPlayerSetupDocument(candidate.setups)}) {
            if (!validation.succeeded()) {
                fail(ProfileMigrationResult::LegacyValidationError,
                     "empty separated v1 document is invalid: " + validation.message);
            }
        }

        candidate.report.migratedSeatCount =
            static_cast<std::uint32_t>(candidate.seats.seats.size());
        std::sort(candidate.report.diagnostics.begin(),
                  candidate.report.diagnostics.end(),
                  [](const auto& left, const auto& right) {
                      if (left.jsonPointer != right.jsonPointer) {
                          return left.jsonPointer < right.jsonPointer;
                      }
                      if (left.kind != right.kind) {
                          return static_cast<std::uint8_t>(left.kind) <
                                 static_cast<std::uint8_t>(right.kind);
                      }
                      if (left.valueJson != right.valueJson) {
                          return left.valueJson < right.valueJson;
                      }
                      return left.note < right.note;
                  });
        validateReport(candidate.report);
        std::string reportError;
        if (encodeProfileMigrationReport(candidate.report, &reportError).empty()) {
            fail(ProfileMigrationResult::ReportBoundsExceeded,
                 reportError.empty() ? "could not encode bounded migration report"
                                     : reportError);
        }

        bundle = std::move(candidate);
        return outcome(ProfileMigrationResult::Success,
                       "legacy workspace migration plan is valid", &bundle.report);
    } catch (const MigrationException& error) {
        return outcome(error.result, error.what());
    } catch (const std::exception& error) {
        return outcome(ProfileMigrationResult::LegacyValidationError, error.what());
    }
}

ProfileMigrationOutcome migrateLegacyWorkspaceProfile(
    const std::filesystem::path& legacyWorkspaceFile,
    const std::filesystem::path& destinationDirectory,
    const ProfileMigrationOptions& options) {
    if (legacyWorkspaceFile.empty() || destinationDirectory.empty() ||
        destinationDirectory.filename().empty() ||
        destinationDirectory.filename() == "." ||
        destinationDirectory.filename() == "..") {
        return outcome(ProfileMigrationResult::InvalidArgument,
                       "source and destination paths must name a file and bundle directory");
    }

    std::string sourceBytes;
    std::string ioError;
    bool sourceTooLarge = false;
    if (!readFileBounded(legacyWorkspaceFile, kMaximumSchemaDocumentBytes,
                         sourceBytes, ioError, sourceTooLarge)) {
        return outcome(sourceTooLarge ? ProfileMigrationResult::SourceTooLarge
                                      : ProfileMigrationResult::SourceReadError,
                       ioError);
    }

    ProfileMigrationBundle bundle;
    auto planned = planLegacyWorkspaceMigration(sourceBytes, bundle);
    if (!planned.succeeded()) return planned;

    EncodedBundle encoded;
    if (!encodeBundle(bundle, encoded, ioError)) {
        return outcome(ProfileMigrationResult::StagedValidationError,
                       ioError, &bundle.report);
    }

    std::filesystem::path sourceAbsolute;
    std::filesystem::path destinationAbsolute;
    if (!normalizedAbsolute(legacyWorkspaceFile, sourceAbsolute, ioError) ||
        !normalizedAbsolute(destinationDirectory, destinationAbsolute, ioError)) {
        return outcome(ProfileMigrationResult::InvalidArgument,
                       ioError, &bundle.report);
    }
    if (sourceAbsolute == destinationAbsolute ||
        pathWithin(sourceAbsolute, destinationAbsolute)) {
        return outcome(ProfileMigrationResult::InvalidArgument,
                       "legacy source must not be inside the destination bundle",
                       &bundle.report);
    }

    const auto staging = withSuffix(destinationDirectory, ".staging");
    const auto rollback = withSuffix(destinationDirectory, ".rollback");
    std::error_code inspectError;
    const bool stagingExists = std::filesystem::exists(staging, inspectError);
    if (inspectError) {
        return outcome(ProfileMigrationResult::DestinationConflict,
                       "could not inspect staging path: " + inspectError.message(),
                       &bundle.report);
    }
    const bool rollbackExists = std::filesystem::exists(rollback, inspectError);
    if (inspectError) {
        return outcome(ProfileMigrationResult::DestinationConflict,
                       "could not inspect rollback path: " + inspectError.message(),
                       &bundle.report);
    }
    if (stagingExists || rollbackExists) {
        return outcome(ProfileMigrationResult::DestinationConflict,
                       "stale staging or rollback path requires recovery before migration",
                       &bundle.report);
    }

    const bool destinationExists =
        std::filesystem::exists(destinationDirectory, inspectError);
    if (inspectError) {
        return outcome(ProfileMigrationResult::DestinationConflict,
                       "could not inspect destination: " + inspectError.message(),
                       &bundle.report);
    }
    if (destinationExists) {
        std::error_code typeError;
        if (!std::filesystem::is_directory(destinationDirectory, typeError) || typeError) {
            return outcome(ProfileMigrationResult::DestinationConflict,
                           "destination exists but is not a readable directory",
                           &bundle.report);
        }
        if (!options.replaceExisting) {
            return outcome(ProfileMigrationResult::DestinationConflict,
                           "destination already exists and replacement was not authorized",
                           &bundle.report);
        }
    }

    std::error_code createError;
    if (!std::filesystem::create_directories(staging, createError) || createError) {
        return outcome(ProfileMigrationResult::StagingError,
                       "could not create staging directory: " + createError.message(),
                       &bundle.report);
    }

    auto failBeforeCommit = [&](ProfileMigrationResult result,
                                std::string message) {
        std::string cleanupError;
        if (!removeTree(staging, cleanupError)) {
            message += "; staging cleanup failed: " + cleanupError;
        }
        return outcome(result, std::move(message), &bundle.report);
    };

    if (options.testingFault == ProfileMigrationTestFault::BeforeBackupWrite) {
        return failBeforeCommit(ProfileMigrationResult::WriteError,
                                "injected backup write failure");
    }
    if (!writeFile(bundlePath(staging, kLegacyWorkspaceBackupFileName),
                   sourceBytes, ioError)) {
        return failBeforeCommit(ProfileMigrationResult::WriteError, ioError);
    }
    if (options.testingFault == ProfileMigrationTestFault::BeforeSeatDocumentWrite) {
        return failBeforeCommit(ProfileMigrationResult::WriteError,
                                "injected Seat document write failure");
    }

    const std::array<std::pair<std::string_view, const std::string*>, 5u> files{{
        {kMigratedSeatConfigFileName, &encoded.seats},
        {kMigratedPlayerProfilesFileName, &encoded.players},
        {kMigratedGameRecordsFileName, &encoded.games},
        {kMigratedTwoPlayerSetupsFileName, &encoded.setups},
        {kProfileMigrationReportFileName, &encoded.report},
    }};
    for (const auto& [name, bytes] : files) {
        if (!writeFile(bundlePath(staging, name), *bytes, ioError)) {
            return failBeforeCommit(ProfileMigrationResult::WriteError, ioError);
        }
    }

    if (options.testingFault ==
        ProfileMigrationTestFault::CorruptSeatDocumentBeforeStagedValidation) {
        if (!writeFile(bundlePath(staging, kMigratedSeatConfigFileName), "{}", ioError)) {
            return failBeforeCommit(ProfileMigrationResult::WriteError, ioError);
        }
    }

    if (!validateBundleDirectory(staging, sourceBytes, bundle, encoded, ioError)) {
        return failBeforeCommit(ProfileMigrationResult::StagedValidationError, ioError);
    }

    bool previousMoved = false;
    if (destinationExists) {
        if (!movePath(destinationDirectory, rollback, ioError)) {
            return failBeforeCommit(ProfileMigrationResult::CommitError, ioError);
        }
        previousMoved = true;
    }

    if (options.testingFault ==
        ProfileMigrationTestFault::AfterPreviousDestinationMoved) {
        std::string rollbackError;
        if (previousMoved && !movePath(rollback, destinationDirectory, rollbackError)) {
            (void)removeTree(staging, ioError);
            return outcome(ProfileMigrationResult::RollbackError,
                           "injected commit failure and rollback failed: " + rollbackError,
                           &bundle.report);
        }
        return failBeforeCommit(ProfileMigrationResult::CommitError,
                                "injected commit failure after previous destination move");
    }

    if (!movePath(staging, destinationDirectory, ioError)) {
        std::string rollbackError;
        if (previousMoved && !movePath(rollback, destinationDirectory, rollbackError)) {
            return outcome(ProfileMigrationResult::RollbackError,
                           ioError + "; previous bundle restore failed: " + rollbackError,
                           &bundle.report);
        }
        std::string cleanupError;
        (void)removeTree(staging, cleanupError);
        return outcome(ProfileMigrationResult::CommitError, ioError, &bundle.report);
    }

    const bool injectedCommittedValidationFailure =
        options.testingFault == ProfileMigrationTestFault::BeforeCommittedValidation;
    if (injectedCommittedValidationFailure ||
        !validateBundleDirectory(destinationDirectory, sourceBytes, bundle,
                                 encoded, ioError)) {
        if (injectedCommittedValidationFailure) {
            ioError = "injected committed bundle validation failure";
        }
        std::string rollbackError;
        if (!restorePreviousBundle(destinationDirectory, rollback,
                                   previousMoved, rollbackError)) {
            return outcome(ProfileMigrationResult::RollbackError,
                           ioError + "; committed bundle rollback failed: " + rollbackError,
                           &bundle.report);
        }
        return outcome(ProfileMigrationResult::CommittedValidationError,
                       ioError, &bundle.report);
    }

    std::string cleanupError;
    if (previousMoved && !removeTree(rollback, cleanupError)) {
        return outcome(ProfileMigrationResult::Success,
                       "migration committed; previous rollback directory retained: " +
                           cleanupError,
                       &bundle.report);
    }
    return outcome(ProfileMigrationResult::Success,
                   "legacy workspace migrated and committed transactionally",
                   &bundle.report);
}

std::string encodeProfileMigrationReport(const ProfileMigrationReport& report,
                                         std::string* error) {
    if (error != nullptr) error->clear();
    try {
        validateReport(report);
        std::ostringstream output;
        output << "{\"report_version\":" << report.reportVersion
               << ",\"legacy_schema_version\":" << report.legacySchemaVersion
               << ",\"target_schema_version\":" << report.targetSchemaVersion
               << ",\"source_byte_count\":" << report.sourceByteCount
               << ",\"source_fnv1a64\":" << quoteJson(hex64(report.sourceFnv1a64))
               << ",\"migrated_seat_count\":" << report.migratedSeatCount
               << ",\"diagnostics\":[";
        for (std::size_t index = 0u; index < report.diagnostics.size(); ++index) {
            if (index != 0u) output << ',';
            const auto& item = report.diagnostics[index];
            output << "{\"kind\":"
                   << quoteJson(profileMigrationDiagnosticKindName(item.kind))
                   << ",\"path\":" << quoteJson(item.jsonPointer)
                   << ",\"value_json\":" << quoteJson(item.valueJson)
                   << ",\"note\":" << quoteJson(item.note) << '}';
        }
        output << "]}";
        auto bytes = output.str();
        if (bytes.size() > kMaximumProfileMigrationReportBytes) {
            fail(ProfileMigrationResult::ReportBoundsExceeded,
                 "encoded migration report exceeds bounded size");
        }
        return bytes;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return {};
    }
}

std::string_view profileMigrationResultName(ProfileMigrationResult result) noexcept {
    switch (result) {
    case ProfileMigrationResult::Success: return "success";
    case ProfileMigrationResult::InvalidArgument: return "invalid-argument";
    case ProfileMigrationResult::SourceReadError: return "source-read-error";
    case ProfileMigrationResult::SourceTooLarge: return "source-too-large";
    case ProfileMigrationResult::LegacyParseError: return "legacy-parse-error";
    case ProfileMigrationResult::LegacyValidationError: return "legacy-validation-error";
    case ProfileMigrationResult::ReportBoundsExceeded: return "report-bounds-exceeded";
    case ProfileMigrationResult::DestinationConflict: return "destination-conflict";
    case ProfileMigrationResult::StagingError: return "staging-error";
    case ProfileMigrationResult::WriteError: return "write-error";
    case ProfileMigrationResult::StagedValidationError: return "staged-validation-error";
    case ProfileMigrationResult::CommitError: return "commit-error";
    case ProfileMigrationResult::RollbackError: return "rollback-error";
    case ProfileMigrationResult::CommittedValidationError:
        return "committed-validation-error";
    }
    return "unknown";
}

std::string_view profileMigrationDiagnosticKindName(
    ProfileMigrationDiagnosticKind kind) noexcept {
    switch (kind) {
    case ProfileMigrationDiagnosticKind::DroppedRuntimeField:
        return "dropped-runtime-field";
    case ProfileMigrationDiagnosticKind::UnmappedLegacyField:
        return "unmapped-legacy-field";
    case ProfileMigrationDiagnosticKind::LegacyShareableResource:
        return "legacy-shareable-resource";
    case ProfileMigrationDiagnosticKind::AppliedLegacyDefault:
        return "applied-legacy-default";
    }
    return "unknown";
}

} // namespace hydra::profile
