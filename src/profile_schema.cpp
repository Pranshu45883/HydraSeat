#include "hydra/profile_schema.hpp"
#include "hydra/internal/strict_json.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace hydra::profile {
namespace {

struct SchemaException final : std::runtime_error {
    SchemaException(SchemaResult code, std::string message)
        : std::runtime_error(std::move(message)), result(code) {}
    SchemaResult result;
};

[[noreturn]] void fail(SchemaResult result, std::string message) {
    throw SchemaException(result, std::move(message));
}

SchemaDiagnostic ok() { return {}; }
SchemaDiagnostic diagnostic(SchemaResult result, std::string message) {
    return {result, std::move(message)};
}

using JsonNumber = internal::json::Number;
using JsonValue = internal::json::Value;

JsonValue parseJson(std::string_view text) {
    try {
        return internal::json::parse(text, internal::json::ParseOptions{48u, 32768u});
    } catch (const internal::json::ParseError& error) {
        fail(SchemaResult::ParseError, error.what());
    }
}

const JsonValue::Object& asObject(const JsonValue& value, std::string_view field) {
    const auto* object = std::get_if<JsonValue::Object>(&value.value);
    if (object == nullptr) fail(SchemaResult::WrongType, std::string(field) + " must be an object");
    return *object;
}

const JsonValue::Array& asArray(const JsonValue& value, std::string_view field) {
    const auto* array = std::get_if<JsonValue::Array>(&value.value);
    if (array == nullptr) fail(SchemaResult::WrongType, std::string(field) + " must be an array");
    return *array;
}

const std::string& asString(const JsonValue& value, std::string_view field) {
    const auto* text = std::get_if<std::string>(&value.value);
    if (text == nullptr) fail(SchemaResult::WrongType, std::string(field) + " must be a string");
    return *text;
}

bool asBool(const JsonValue& value, std::string_view field) {
    const auto* boolean = std::get_if<bool>(&value.value);
    if (boolean == nullptr) fail(SchemaResult::WrongType, std::string(field) + " must be a boolean");
    return *boolean;
}

std::uint64_t asUnsigned(const JsonValue& value, std::string_view field) {
    const auto* number = std::get_if<JsonNumber>(&value.value);
    if (number == nullptr || number->text.empty() || number->text.front() == '-') {
        fail(SchemaResult::WrongType, std::string(field) + " must be an unsigned integer");
    }
    std::uint64_t result = 0u;
    const auto converted = std::from_chars(number->text.data(),
                                            number->text.data() + number->text.size(),
                                            result);
    if (converted.ec != std::errc{} ||
        converted.ptr != number->text.data() + number->text.size()) {
        fail(SchemaResult::InvalidValue, std::string(field) + " integer is out of range");
    }
    return result;
}

std::uint32_t asU32(const JsonValue& value, std::string_view field) {
    const auto raw = asUnsigned(value, field);
    if (raw > std::numeric_limits<std::uint32_t>::max()) {
        fail(SchemaResult::InvalidValue, std::string(field) + " exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(raw);
}

bool isNull(const JsonValue& value) {
    return std::holds_alternative<std::nullptr_t>(value.value);
}

const JsonValue& required(const JsonValue::Object& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        fail(SchemaResult::MissingField, std::string("missing field: ") + key);
    }
    return found->second;
}

void exactFields(const JsonValue::Object& object,
                 std::initializer_list<const char*> requiredFields,
                 std::initializer_list<const char*> optionalFields = {}) {
    std::set<std::string_view> known;
    for (const auto* key : requiredFields) {
        known.insert(key);
        if (!object.contains(key)) {
            fail(SchemaResult::MissingField, std::string("missing field: ") + key);
        }
    }
    for (const auto* key : optionalFields) known.insert(key);
    for (const auto& [key, ignored] : object) {
        (void)ignored;
        if (!known.contains(key)) {
            fail(SchemaResult::UnknownField, "unknown field: " + key);
        }
    }
}

std::uint32_t schemaVersion(const JsonValue::Object& object) {
    const auto version = asU32(required(object, "schema_version"), "schema_version");
    if (version != kProfileSchemaVersion) {
        fail(SchemaResult::UnsupportedVersion,
             "unsupported schema_version: " + std::to_string(version));
    }
    return version;
}

std::wstring fromUtf8(std::string_view input) {
    std::wstring output;
    output.reserve(input.size());
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
            fail(SchemaResult::InvalidValue, "invalid UTF-8 leading byte");
        }
        if (index + continuation > input.size()) {
            fail(SchemaResult::InvalidValue, "truncated UTF-8 sequence");
        }
        for (unsigned count = 0u; count < continuation; ++count) {
            const auto current = static_cast<unsigned char>(input[index++]);
            if ((current & 0xc0u) != 0x80u) {
                fail(SchemaResult::InvalidValue, "invalid UTF-8 continuation byte");
            }
            codePoint = (codePoint << 6u) | (current & 0x3fu);
        }
        if (codePoint < minimum || codePoint > 0x10ffffu ||
            (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            fail(SchemaResult::InvalidValue, "invalid UTF-8 code point");
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

std::string toUtf8(std::wstring_view input) {
    std::string output;
    auto append = [&](std::uint32_t codePoint) {
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
    };
    for (std::size_t index = 0u; index < input.size(); ++index) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(input[index]);
        if constexpr (sizeof(wchar_t) == 2u) {
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu) {
                if (++index >= input.size()) {
                    fail(SchemaResult::InvalidValue, "unpaired UTF-16 high surrogate");
                }
                const auto low = static_cast<std::uint32_t>(input[index]);
                if (low < 0xdc00u || low > 0xdfffu) {
                    fail(SchemaResult::InvalidValue, "invalid UTF-16 surrogate pair");
                }
                codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) +
                            (low - 0xdc00u);
            } else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
                fail(SchemaResult::InvalidValue, "unpaired UTF-16 low surrogate");
            }
        } else if (codePoint > 0x10ffffu ||
                   (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            fail(SchemaResult::InvalidValue, "invalid wide-character code point");
        }
        append(codePoint);
    }
    return output;
}

std::string quoteUtf8(std::string_view text) {
    std::ostringstream output;
    output << '"';
    static constexpr char hex[] = "0123456789abcdef";
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (ch < 0x20u) {
                output << "\\u00" << hex[ch >> 4u] << hex[ch & 0x0fu];
            } else {
                output << static_cast<char>(ch);
            }
        }
    }
    output << '"';
    return output.str();
}

std::string quoteWide(std::wstring_view text) { return quoteUtf8(toUtf8(text)); }

bool validIdentifier(std::string_view value) {
    if (value.empty() || value.size() > kMaximumIdentifierBytes) return false;
    return std::all_of(value.begin(), value.end(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return std::isalnum(ch) != 0 || ch == '.' || ch == '_' || ch == '-' || ch == ':';
    });
}

bool validLocale(std::string_view value) {
    if (value.empty()) return true;
    if (value.size() > kMaximumLocaleBytes) return false;
    return std::all_of(value.begin(), value.end(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
    });
}

bool validSha256(std::string_view value) {
    return value.size() == 64u && std::all_of(value.begin(), value.end(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return std::isxdigit(ch) != 0;
    });
}

bool equivalentDeviceId(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0u; index < left.size(); ++index) {
        auto l = left[index];
        auto r = right[index];
        if (l >= L'A' && l <= L'Z') l = static_cast<wchar_t>(l - L'A' + L'a');
        if (r >= L'A' && r <= L'Z') r = static_cast<wchar_t>(r - L'A' + L'a');
        if (l != r) return false;
    }
    return true;
}

SchemaDiagnostic validateWide(std::wstring_view value,
                              std::size_t maximum,
                              std::string_view field,
                              bool allowEmpty) {
    if (!allowEmpty && value.empty()) {
        return diagnostic(SchemaResult::InvalidValue,
                          std::string(field) + " must not be empty");
    }
    if (value.size() > maximum) {
        return diagnostic(SchemaResult::BoundsExceeded,
                          std::string(field) + " exceeds maximum length");
    }
    try {
        (void)toUtf8(value);
    } catch (const SchemaException& error) {
        return diagnostic(error.result, std::string(field) + ": " + error.what());
    }
    return ok();
}

SchemaDiagnostic validateDeviceList(const std::vector<std::wstring>& values,
                                    std::string_view field) {
    if (values.size() > kMaximumDeviceIdsPerSeat) {
        return diagnostic(SchemaResult::BoundsExceeded,
                          std::string(field) + " contains too many entries");
    }
    for (const auto& value : values) {
        const auto checked = validateWide(value, kMaximumDeviceIdCodeUnits, field, false);
        if (!checked.succeeded()) return checked;
    }
    for (std::size_t left = 0u; left < values.size(); ++left) {
        for (std::size_t right = left + 1u; right < values.size(); ++right) {
            if (equivalentDeviceId(values[left], values[right])) {
                return diagnostic(SchemaResult::DuplicateId,
                                  std::string(field) + " contains duplicate device IDs");
            }
        }
    }
    return ok();
}

template <typename T, typename IdFn>
SchemaDiagnostic uniqueIds(const std::vector<T>& values,
                           IdFn id,
                           std::string_view field) {
    std::set<std::string> seen;
    for (const auto& value : values) {
        const auto identifier = id(value);
        if (!seen.insert(identifier).second) {
            return diagnostic(SchemaResult::DuplicateId,
                              std::string(field) + " contains duplicate ID: " + identifier);
        }
    }
    return ok();
}

std::vector<std::wstring> readWideArray(const JsonValue& value,
                                        std::string_view field) {
    const auto& array = asArray(value, field);
    std::vector<std::wstring> result;
    result.reserve(array.size());
    for (const auto& item : array) result.push_back(fromUtf8(asString(item, field)));
    return result;
}

std::optional<std::wstring> readOptionalWide(const JsonValue& value,
                                             std::string_view field) {
    if (isNull(value)) return std::nullopt;
    return fromUtf8(asString(value, field));
}

std::optional<std::string> readOptionalString(const JsonValue& value,
                                              std::string_view field) {
    if (isNull(value)) return std::nullopt;
    return asString(value, field);
}

void writeWideArray(std::ostringstream& output,
                    const std::vector<std::wstring>& values) {
    output << '[';
    for (std::size_t index = 0u; index < values.size(); ++index) {
        if (index != 0u) output << ',';
        output << quoteWide(values[index]);
    }
    output << ']';
}

void writeOptionalWide(std::ostringstream& output,
                       const std::optional<std::wstring>& value) {
    if (value) output << quoteWide(*value);
    else output << "null";
}

void writeOptionalString(std::ostringstream& output,
                         const std::optional<std::string>& value) {
    if (value) output << quoteUtf8(*value);
    else output << "null";
}

template <typename Document, typename Validator, typename Encoder>
std::string encodeValidated(const Document& document,
                            Validator validate,
                            Encoder encode,
                            SchemaDiagnostic* outputDiagnostic) {
    try {
        const auto checked = validate(document);
        if (!checked.succeeded()) {
            if (outputDiagnostic != nullptr) *outputDiagnostic = checked;
            return {};
        }
        auto result = encode(document);
        if (result.size() > kMaximumSchemaDocumentBytes) {
            const auto oversized = diagnostic(SchemaResult::DocumentTooLarge,
                                              "encoded schema document exceeds maximum size");
            if (outputDiagnostic != nullptr) *outputDiagnostic = oversized;
            return {};
        }
        if (outputDiagnostic != nullptr) *outputDiagnostic = ok();
        return result;
    } catch (const SchemaException& error) {
        const auto failed = diagnostic(error.result, error.what());
        if (outputDiagnostic != nullptr) *outputDiagnostic = failed;
        return {};
    } catch (const std::exception& error) {
        const auto failed = diagnostic(SchemaResult::InvalidValue, error.what());
        if (outputDiagnostic != nullptr) *outputDiagnostic = failed;
        return {};
    }
}

template <typename Document, typename Decoder, typename Validator>
SchemaDiagnostic decodeValidated(std::string_view json,
                                 Document& document,
                                 Decoder decode,
                                 Validator validate) {
    if (json.size() > kMaximumSchemaDocumentBytes) {
        return diagnostic(SchemaResult::DocumentTooLarge,
                          "schema document exceeds maximum size");
    }
    try {
        auto root = parseJson(json);
        Document candidate = decode(root);
        const auto checked = validate(candidate);
        if (!checked.succeeded()) return checked;
        document = std::move(candidate);
        return ok();
    } catch (const SchemaException& error) {
        return diagnostic(error.result, error.what());
    } catch (const std::exception& error) {
        return diagnostic(SchemaResult::ParseError, error.what());
    }
}

std::string encodeSeat(const PersistedSeatConfig& seat) {
    std::ostringstream output;
    output << '{'
           << "\"seat_id\":" << seat.seatId << ','
           << "\"name\":" << quoteWide(seat.name) << ','
           << "\"active\":" << (seat.active ? "true" : "false") << ','
           << "\"display_ids\":";
    writeWideArray(output, seat.displayIds);
    output << ",\"primary_display_id\":";
    writeOptionalWide(output, seat.primaryDisplayId);
    output << ",\"keyboard_ids\":";
    writeWideArray(output, seat.keyboardIds);
    output << ",\"mouse_ids\":";
    writeWideArray(output, seat.mouseIds);
    output << ",\"controller_ids\":";
    writeWideArray(output, seat.controllerIds);
    output << ",\"audio_output_endpoint_id\":";
    writeOptionalWide(output, seat.audioOutputEndpointId);
    output << ",\"audio_input_endpoint_id\":";
    writeOptionalWide(output, seat.audioInputEndpointId);
    output << '}';
    return output.str();
}

PersistedSeatConfig decodeSeat(const JsonValue& value) {
    const auto& object = asObject(value, "seat");
    exactFields(object,
                {"seat_id", "name", "active", "display_ids", "primary_display_id",
                 "keyboard_ids", "mouse_ids", "controller_ids",
                 "audio_output_endpoint_id", "audio_input_endpoint_id"});
    PersistedSeatConfig seat;
    seat.seatId = asU32(required(object, "seat_id"), "seat_id");
    seat.name = fromUtf8(asString(required(object, "name"), "name"));
    seat.active = asBool(required(object, "active"), "active");
    seat.displayIds = readWideArray(required(object, "display_ids"), "display_ids");
    seat.primaryDisplayId = readOptionalWide(required(object, "primary_display_id"),
                                             "primary_display_id");
    seat.keyboardIds = readWideArray(required(object, "keyboard_ids"), "keyboard_ids");
    seat.mouseIds = readWideArray(required(object, "mouse_ids"), "mouse_ids");
    seat.controllerIds = readWideArray(required(object, "controller_ids"), "controller_ids");
    seat.audioOutputEndpointId = readOptionalWide(
        required(object, "audio_output_endpoint_id"), "audio_output_endpoint_id");
    seat.audioInputEndpointId = readOptionalWide(
        required(object, "audio_input_endpoint_id"), "audio_input_endpoint_id");
    return seat;
}

std::string encodePlayer(const PlayerProfile& player) {
    std::ostringstream output;
    output << '{'
           << "\"player_id\":" << quoteUtf8(player.playerId) << ','
           << "\"display_name\":" << quoteWide(player.displayName) << ','
           << "\"preferred_locale\":" << quoteUtf8(player.preferredLocale) << ','
           << "\"provider_accounts\":[";
    for (std::size_t index = 0u; index < player.providerAccounts.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& account = player.providerAccounts[index];
        output << '{'
               << "\"provider_id\":" << quoteUtf8(account.providerId) << ','
               << "\"account_ref\":" << quoteUtf8(account.accountRef)
               << '}';
    }
    output << "]}";
    return output.str();
}

PlayerProfile decodePlayer(const JsonValue& value) {
    const auto& object = asObject(value, "player");
    exactFields(object, {"player_id", "display_name", "preferred_locale",
                         "provider_accounts"});
    PlayerProfile player;
    player.playerId = asString(required(object, "player_id"), "player_id");
    player.displayName = fromUtf8(asString(required(object, "display_name"),
                                           "display_name"));
    player.preferredLocale = asString(required(object, "preferred_locale"),
                                      "preferred_locale");
    for (const auto& accountValue : asArray(required(object, "provider_accounts"),
                                            "provider_accounts")) {
        const auto& accountObject = asObject(accountValue, "provider_account");
        exactFields(accountObject, {"provider_id", "account_ref"});
        player.providerAccounts.push_back({
            asString(required(accountObject, "provider_id"), "provider_id"),
            asString(required(accountObject, "account_ref"), "account_ref")});
    }
    return player;
}

GameOrigin parseGameOrigin(std::string_view value) {
    if (value == "discovered") return GameOrigin::Discovered;
    if (value == "manual") return GameOrigin::Manual;
    fail(SchemaResult::InvalidValue, "unknown game origin");
}

SchemaDiagnostic validateCompatibilityReference(const CompatibilityReference& reference) {
    if (!validIdentifier(reference.recordId) || !validIdentifier(reference.provenance)) {
        return diagnostic(SchemaResult::InvalidValue,
                          "invalid compatibility record/provenance reference");
    }
    if (reference.evidenceRevision == 0u) {
        return diagnostic(SchemaResult::InvalidValue,
                          "compatibility evidence_revision must be nonzero");
    }
    return ok();
}

std::string encodeCompatibilityReference(const CompatibilityReference& reference) {
    std::ostringstream output;
    output << '{'
           << "\"record_id\":" << quoteUtf8(reference.recordId) << ','
           << "\"provenance\":" << quoteUtf8(reference.provenance) << ','
           << "\"evidence_revision\":" << reference.evidenceRevision
           << '}';
    return output.str();
}

void writeOptionalCompatibilityReference(
    std::ostringstream& output,
    const std::optional<CompatibilityReference>& reference) {
    if (reference) output << encodeCompatibilityReference(*reference);
    else output << "null";
}

std::optional<CompatibilityReference> decodeOptionalCompatibilityReference(
    const JsonValue& value) {
    if (isNull(value)) return std::nullopt;
    const auto& object = asObject(value, "compatibility");
    exactFields(object, {"record_id", "provenance", "evidence_revision"});
    CompatibilityReference reference;
    reference.recordId = asString(required(object, "record_id"), "record_id");
    reference.provenance = asString(required(object, "provenance"), "provenance");
    reference.evidenceRevision = asU32(required(object, "evidence_revision"),
                                       "evidence_revision");
    return reference;
}

std::string encodeGame(const GameRecord& game) {
    std::ostringstream output;
    output << '{'
           << "\"game_id\":" << quoteUtf8(game.gameId) << ','
           << "\"provider_id\":" << quoteUtf8(game.providerId) << ','
           << "\"provider_app_id\":";
    writeOptionalString(output, game.providerAppId);
    output << ",\"title\":" << quoteWide(game.title) << ','
           << "\"install_root\":" << quoteWide(game.installRoot) << ','
           << "\"executable_candidates\":";
    writeWideArray(output, game.executableCandidates);
    output << ",\"local_version\":";
    writeOptionalWide(output, game.localVersion);
    output << ",\"executable_sha256\":";
    writeOptionalString(output, game.executableSha256);
    output << ",\"compatibility\":";
    writeOptionalCompatibilityReference(output, game.compatibility);
    output << ",\"origin\":" << quoteUtf8(gameOriginName(game.origin)) << '}';
    return output.str();
}

GameRecord decodeGame(const JsonValue& value) {
    const auto& object = asObject(value, "game");
    exactFields(object, {"game_id", "provider_id", "provider_app_id", "title",
                         "install_root", "executable_candidates", "local_version",
                         "executable_sha256", "compatibility", "origin"});
    GameRecord game;
    game.gameId = asString(required(object, "game_id"), "game_id");
    game.providerId = asString(required(object, "provider_id"), "provider_id");
    game.providerAppId = readOptionalString(required(object, "provider_app_id"),
                                            "provider_app_id");
    game.title = fromUtf8(asString(required(object, "title"), "title"));
    game.installRoot = fromUtf8(asString(required(object, "install_root"),
                                         "install_root"));
    game.executableCandidates = readWideArray(required(object, "executable_candidates"),
                                              "executable_candidates");
    game.localVersion = readOptionalWide(required(object, "local_version"),
                                         "local_version");
    game.executableSha256 = readOptionalString(required(object, "executable_sha256"),
                                               "executable_sha256");
    game.compatibility = decodeOptionalCompatibilityReference(
        required(object, "compatibility"));
    game.origin = parseGameOrigin(asString(required(object, "origin"), "origin"));
    return game;
}

std::string encodeRecipe(const InstanceRecipe& recipe) {
    std::ostringstream output;
    output << "{\"arguments\":";
    writeWideArray(output, recipe.arguments);
    output << ",\"working_directory\":";
    writeOptionalWide(output, recipe.workingDirectory);
    output << ",\"data_root\":";
    writeOptionalWide(output, recipe.dataRoot);
    output << '}';
    return output.str();
}

InstanceRecipe decodeRecipe(const JsonValue& value) {
    const auto& object = asObject(value, "instance_recipe");
    exactFields(object, {"arguments", "working_directory", "data_root"});
    InstanceRecipe recipe;
    recipe.arguments = readWideArray(required(object, "arguments"), "arguments");
    recipe.workingDirectory = readOptionalWide(required(object, "working_directory"),
                                               "working_directory");
    recipe.dataRoot = readOptionalWide(required(object, "data_root"), "data_root");
    return recipe;
}

std::string encodeSetup(const TwoPlayerSetup& setup) {
    std::ostringstream output;
    output << '{'
           << "\"setup_id\":" << quoteUtf8(setup.setupId) << ','
           << "\"game_id\":" << quoteUtf8(setup.gameId) << ','
           << "\"display_name\":" << quoteWide(setup.displayName) << ','
           << "\"compatibility\":";
    writeOptionalCompatibilityReference(output, setup.compatibility);
    output << ",\"instances\":[";
    for (std::size_t index = 0u; index < setup.instances.size(); ++index) {
        if (index != 0u) output << ',';
        output << encodeRecipe(setup.instances[index]);
    }
    output << "]}";
    return output.str();
}

TwoPlayerSetup decodeSetup(const JsonValue& value) {
    const auto& object = asObject(value, "two_player_setup");
    exactFields(object, {"setup_id", "game_id", "display_name", "compatibility",
                         "instances"});
    TwoPlayerSetup setup;
    setup.setupId = asString(required(object, "setup_id"), "setup_id");
    setup.gameId = asString(required(object, "game_id"), "game_id");
    setup.displayName = fromUtf8(asString(required(object, "display_name"),
                                          "display_name"));
    setup.compatibility = decodeOptionalCompatibilityReference(
        required(object, "compatibility"));
    for (const auto& item : asArray(required(object, "instances"), "instances")) {
        setup.instances.push_back(decodeRecipe(item));
    }
    return setup;
}

std::string encodeBinding(const RuntimeBinding& binding) {
    std::ostringstream output;
    output << '{'
           << "\"seat_id\":" << binding.seatId << ','
           << "\"player_id\":" << quoteUtf8(binding.playerId) << ','
           << "\"game_id\":" << quoteUtf8(binding.gameId) << ','
           << "\"setup_id\":";
    writeOptionalString(output, binding.setupId);
    output << ",\"instance_index\":" << binding.instanceIndex << '}';
    return output.str();
}

RuntimeBinding decodeBinding(const JsonValue& value) {
    const auto& object = asObject(value, "runtime_binding");
    exactFields(object, {"seat_id", "player_id", "game_id", "setup_id",
                         "instance_index"});
    RuntimeBinding binding;
    binding.seatId = asU32(required(object, "seat_id"), "seat_id");
    binding.playerId = asString(required(object, "player_id"), "player_id");
    binding.gameId = asString(required(object, "game_id"), "game_id");
    binding.setupId = readOptionalString(required(object, "setup_id"), "setup_id");
    binding.instanceIndex = asU32(required(object, "instance_index"), "instance_index");
    return binding;
}

} // namespace

SchemaDiagnostic validateSeatConfigDocument(const SeatConfigDocument& document) {
    if (document.schemaVersion != kProfileSchemaVersion) {
        return diagnostic(SchemaResult::UnsupportedVersion, "unsupported Seat schema version");
    }
    if (document.seats.empty() || document.seats.size() > kMaximumPersistedSeats) {
        return diagnostic(SchemaResult::BoundsExceeded,
                          "Seat document must contain one or two Seats");
    }
    std::set<SeatId> seatIds;
    bool managementFound = false;
    std::size_t activeCount = 0u;
    for (const auto& seat : document.seats) {
        if (seat.seatId == 0u) {
            return diagnostic(SchemaResult::InvalidValue, "Seat ID must be nonzero");
        }
        if (!seatIds.insert(seat.seatId).second) {
            return diagnostic(SchemaResult::DuplicateId, "duplicate Seat ID");
        }
        if (seat.seatId == document.managementSeatId) managementFound = true;
        if (seat.active) ++activeCount;
        auto checked = validateWide(seat.name, kMaximumDisplayNameCodeUnits,
                                    "Seat name", false);
        if (!checked.succeeded()) return checked;
        for (const auto& [list, field] : {
                 std::pair<const std::vector<std::wstring>*, const char*>{&seat.displayIds, "display_ids"},
                 {&seat.keyboardIds, "keyboard_ids"},
                 {&seat.mouseIds, "mouse_ids"},
                 {&seat.controllerIds, "controller_ids"}}) {
            checked = validateDeviceList(*list, field);
            if (!checked.succeeded()) return checked;
        }
        if (seat.primaryDisplayId) {
            checked = validateWide(*seat.primaryDisplayId, kMaximumDeviceIdCodeUnits,
                                   "primary_display_id", false);
            if (!checked.succeeded()) return checked;
            if (std::none_of(seat.displayIds.begin(), seat.displayIds.end(),
                             [&](const auto& id) {
                                 return equivalentDeviceId(id, *seat.primaryDisplayId);
                             })) {
                return diagnostic(SchemaResult::InvalidValue,
                                  "primary display must be a member of display_ids");
            }
        }
        for (const auto& endpoint : {seat.audioOutputEndpointId, seat.audioInputEndpointId}) {
            if (endpoint) {
                checked = validateWide(*endpoint, kMaximumDeviceIdCodeUnits,
                                       "audio endpoint ID", false);
                if (!checked.succeeded()) return checked;
            }
        }
    }
    if (activeCount > 2u) {
        return diagnostic(SchemaResult::BoundsExceeded, "v1 permits at most two active Seats");
    }
    if (document.managementSeatId == 0u || !managementFound) {
        return diagnostic(SchemaResult::CrossReferenceError,
                          "management Seat must reference a persisted Seat");
    }
    return ok();
}

SchemaDiagnostic validatePlayerProfileDocument(const PlayerProfileDocument& document) {
    if (document.schemaVersion != kProfileSchemaVersion) {
        return diagnostic(SchemaResult::UnsupportedVersion, "unsupported Player schema version");
    }
    if (document.players.size() > kMaximumPlayers) {
        return diagnostic(SchemaResult::BoundsExceeded, "too many Player profiles");
    }
    for (const auto& player : document.players) {
        if (!validIdentifier(player.playerId)) {
            return diagnostic(SchemaResult::InvalidValue, "invalid player_id");
        }
        auto checked = validateWide(player.displayName, kMaximumDisplayNameCodeUnits,
                                    "Player display_name", false);
        if (!checked.succeeded()) return checked;
        if (!validLocale(player.preferredLocale)) {
            return diagnostic(SchemaResult::InvalidValue, "invalid preferred_locale");
        }
        if (player.providerAccounts.size() > kMaximumProviderAccountsPerPlayer) {
            return diagnostic(SchemaResult::BoundsExceeded,
                              "too many provider account references");
        }
        std::set<std::string> providers;
        for (const auto& account : player.providerAccounts) {
            if (!validIdentifier(account.providerId) || !validIdentifier(account.accountRef)) {
                return diagnostic(SchemaResult::InvalidValue,
                                  "invalid provider account reference");
            }
            if (!providers.insert(account.providerId).second) {
                return diagnostic(SchemaResult::DuplicateId,
                                  "Player has duplicate provider account reference");
            }
        }
    }
    return uniqueIds(document.players, [](const auto& value) { return value.playerId; },
                     "players");
}

SchemaDiagnostic validateGameRecordDocument(const GameRecordDocument& document) {
    if (document.schemaVersion != kProfileSchemaVersion) {
        return diagnostic(SchemaResult::UnsupportedVersion, "unsupported Game schema version");
    }
    if (document.games.size() > kMaximumGames) {
        return diagnostic(SchemaResult::BoundsExceeded, "too many Game records");
    }
    for (const auto& game : document.games) {
        if (!validIdentifier(game.gameId) || !validIdentifier(game.providerId)) {
            return diagnostic(SchemaResult::InvalidValue, "invalid Game/provider ID");
        }
        if (game.providerAppId && !validIdentifier(*game.providerAppId)) {
            return diagnostic(SchemaResult::InvalidValue, "invalid provider_app_id");
        }
        auto checked = validateWide(game.title, kMaximumTitleCodeUnits, "Game title", false);
        if (!checked.succeeded()) return checked;
        checked = validateWide(game.installRoot, kMaximumPathCodeUnits, "install_root", false);
        if (!checked.succeeded()) return checked;
        if (game.executableCandidates.empty() ||
            game.executableCandidates.size() > kMaximumExecutableCandidates) {
            return diagnostic(SchemaResult::BoundsExceeded,
                              "executable_candidates must contain 1..32 entries");
        }
        std::set<std::wstring> executableSet;
        for (const auto& executable : game.executableCandidates) {
            checked = validateWide(executable, kMaximumPathCodeUnits,
                                   "executable candidate", false);
            if (!checked.succeeded()) return checked;
            if (!executableSet.insert(executable).second) {
                return diagnostic(SchemaResult::DuplicateId,
                                  "duplicate executable candidate");
            }
        }
        if (game.localVersion) {
            checked = validateWide(*game.localVersion, kMaximumDisplayNameCodeUnits,
                                   "local_version", false);
            if (!checked.succeeded()) return checked;
        }
        if (game.executableSha256 && !validSha256(*game.executableSha256)) {
            return diagnostic(SchemaResult::InvalidValue,
                              "executable_sha256 must contain 64 hexadecimal characters");
        }
        if (game.compatibility) {
            checked = validateCompatibilityReference(*game.compatibility);
            if (!checked.succeeded()) return checked;
        }
    }
    return uniqueIds(document.games, [](const auto& value) { return value.gameId; },
                     "games");
}

SchemaDiagnostic validateTwoPlayerSetupDocument(const TwoPlayerSetupDocument& document) {
    if (document.schemaVersion != kProfileSchemaVersion) {
        return diagnostic(SchemaResult::UnsupportedVersion, "unsupported setup schema version");
    }
    if (document.setups.size() > kMaximumSetups) {
        return diagnostic(SchemaResult::BoundsExceeded, "too many TwoPlayerSetup records");
    }
    for (const auto& setup : document.setups) {
        if (!validIdentifier(setup.setupId) || !validIdentifier(setup.gameId)) {
            return diagnostic(SchemaResult::InvalidValue, "invalid setup_id/game_id");
        }
        auto checked = validateWide(setup.displayName, kMaximumDisplayNameCodeUnits,
                                    "setup display_name", false);
        if (!checked.succeeded()) return checked;
        if (setup.compatibility) {
            checked = validateCompatibilityReference(*setup.compatibility);
            if (!checked.succeeded()) return checked;
        }
        if (setup.instances.size() != 2u) {
            return diagnostic(SchemaResult::InvalidValue,
                              "TwoPlayerSetup must contain exactly two instance recipes");
        }
        for (const auto& instance : setup.instances) {
            if (instance.arguments.size() > kMaximumArgumentsPerInstance) {
                return diagnostic(SchemaResult::BoundsExceeded,
                                  "instance recipe has too many arguments");
            }
            for (const auto& argument : instance.arguments) {
                checked = validateWide(argument, kMaximumArgumentCodeUnits,
                                       "instance argument", true);
                if (!checked.succeeded()) return checked;
            }
            for (const auto& path : {instance.workingDirectory, instance.dataRoot}) {
                if (path) {
                    checked = validateWide(*path, kMaximumPathCodeUnits,
                                           "instance path", false);
                    if (!checked.succeeded()) return checked;
                }
            }
        }
    }
    return uniqueIds(document.setups, [](const auto& value) { return value.setupId; },
                     "setups");
}

SchemaDiagnostic validateRuntimeSessionSelection(
    const RuntimeSessionSelection& selection,
    const SeatConfigDocument& seats,
    const PlayerProfileDocument& players,
    const GameRecordDocument& games,
    const TwoPlayerSetupDocument& setups) {
    if (selection.schemaVersion != kProfileSchemaVersion) {
        return diagnostic(SchemaResult::UnsupportedVersion, "unsupported runtime schema version");
    }
    for (const auto& checked : {validateSeatConfigDocument(seats),
                                validatePlayerProfileDocument(players),
                                validateGameRecordDocument(games),
                                validateTwoPlayerSetupDocument(setups)}) {
        if (!checked.succeeded()) return checked;
    }
    if (selection.bindings.empty() || selection.bindings.size() > kMaximumRuntimeBindings) {
        return diagnostic(SchemaResult::BoundsExceeded,
                          "runtime selection must contain one or two bindings");
    }
    std::set<SeatId> seatIds;
    std::set<std::string> playerIds;
    std::map<std::string, std::set<std::uint32_t>> setupInstances;
    for (const auto& binding : selection.bindings) {
        if (!validIdentifier(binding.playerId) || !validIdentifier(binding.gameId)) {
            return diagnostic(SchemaResult::InvalidValue, "invalid runtime binding ID");
        }
        if (!seatIds.insert(binding.seatId).second) {
            return diagnostic(SchemaResult::DuplicateId,
                              "runtime selection binds one Seat more than once");
        }
        if (!playerIds.insert(binding.playerId).second) {
            return diagnostic(SchemaResult::DuplicateId,
                              "runtime selection binds one Player more than once");
        }
        const auto seat = std::find_if(seats.seats.begin(), seats.seats.end(),
                                       [&](const auto& value) {
                                           return value.seatId == binding.seatId;
                                       });
        if (seat == seats.seats.end() || !seat->active) {
            return diagnostic(SchemaResult::CrossReferenceError,
                              "runtime binding references missing/inactive Seat");
        }
        if (std::none_of(players.players.begin(), players.players.end(),
                         [&](const auto& value) { return value.playerId == binding.playerId; })) {
            return diagnostic(SchemaResult::CrossReferenceError,
                              "runtime binding references missing Player");
        }
        if (std::none_of(games.games.begin(), games.games.end(),
                         [&](const auto& value) { return value.gameId == binding.gameId; })) {
            return diagnostic(SchemaResult::CrossReferenceError,
                              "runtime binding references missing Game");
        }
        if (binding.setupId) {
            if (!validIdentifier(*binding.setupId)) {
                return diagnostic(SchemaResult::InvalidValue, "invalid setup_id");
            }
            const auto setup = std::find_if(setups.setups.begin(), setups.setups.end(),
                                            [&](const auto& value) {
                                                return value.setupId == *binding.setupId;
                                            });
            if (setup == setups.setups.end() || setup->gameId != binding.gameId) {
                return diagnostic(SchemaResult::CrossReferenceError,
                                  "runtime setup reference is missing or belongs to another Game");
            }
            if (binding.instanceIndex >= setup->instances.size()) {
                return diagnostic(SchemaResult::CrossReferenceError,
                                  "runtime instance_index exceeds setup recipe count");
            }
            if (!setupInstances[*binding.setupId].insert(binding.instanceIndex).second) {
                return diagnostic(SchemaResult::DuplicateId,
                                  "same setup instance is assigned more than once");
            }
        } else if (binding.instanceIndex != 0u) {
            return diagnostic(SchemaResult::InvalidValue,
                              "instance_index must be zero without a TwoPlayerSetup");
        }
    }
    return ok();
}

SchemaDiagnostic makePersistedSeatConfig(const SeatConfig& runtime,
                                         PersistedSeatConfig& persisted) {
    PersistedSeatConfig candidate;
    candidate.seatId = runtime.seatId;
    candidate.name = runtime.name;
    candidate.displayIds = runtime.displayIds;
    candidate.primaryDisplayId = runtime.primaryDisplayId;
    candidate.keyboardIds = runtime.keyboardIds;
    candidate.mouseIds = runtime.mouseIds;
    candidate.controllerIds = runtime.controllerIds;
    candidate.audioOutputEndpointId = runtime.audioOutputEndpointId;
    candidate.audioInputEndpointId = runtime.audioInputEndpointId;
    candidate.active = runtime.active;
    SeatConfigDocument temporary;
    temporary.managementSeatId = candidate.seatId;
    temporary.seats.push_back(candidate);
    const auto checked = validateSeatConfigDocument(temporary);
    if (!checked.succeeded()) return checked;
    persisted = std::move(candidate);
    return ok();
}

SeatConfig makeRuntimeSeatConfig(const PersistedSeatConfig& persisted) {
    SeatConfig runtime;
    runtime.seatId = persisted.seatId;
    runtime.name = persisted.name;
    runtime.displayIds = persisted.displayIds;
    runtime.primaryDisplayId = persisted.primaryDisplayId;
    runtime.keyboardIds = persisted.keyboardIds;
    runtime.mouseIds = persisted.mouseIds;
    runtime.controllerIds = persisted.controllerIds;
    runtime.audioOutputEndpointId = persisted.audioOutputEndpointId;
    runtime.audioInputEndpointId = persisted.audioInputEndpointId;
    runtime.active = persisted.active;
    return runtime;
}

std::string encodeSeatConfigDocument(const SeatConfigDocument& document,
                                     SchemaDiagnostic* outputDiagnostic) {
    return encodeValidated(document, validateSeatConfigDocument,
                           [](const SeatConfigDocument& value) {
        std::ostringstream output;
        output << "{\"schema_version\":" << value.schemaVersion
               << ",\"management_seat_id\":" << value.managementSeatId
               << ",\"seats\":[";
        for (std::size_t index = 0u; index < value.seats.size(); ++index) {
            if (index != 0u) output << ',';
            output << encodeSeat(value.seats[index]);
        }
        output << "]}";
        return output.str();
    }, outputDiagnostic);
}

std::string encodePlayerProfileDocument(const PlayerProfileDocument& document,
                                        SchemaDiagnostic* outputDiagnostic) {
    return encodeValidated(document, validatePlayerProfileDocument,
                           [](const PlayerProfileDocument& value) {
        std::ostringstream output;
        output << "{\"schema_version\":" << value.schemaVersion << ",\"players\":[";
        for (std::size_t index = 0u; index < value.players.size(); ++index) {
            if (index != 0u) output << ',';
            output << encodePlayer(value.players[index]);
        }
        output << "]}";
        return output.str();
    }, outputDiagnostic);
}

std::string encodeGameRecordDocument(const GameRecordDocument& document,
                                     SchemaDiagnostic* outputDiagnostic) {
    return encodeValidated(document, validateGameRecordDocument,
                           [](const GameRecordDocument& value) {
        std::ostringstream output;
        output << "{\"schema_version\":" << value.schemaVersion << ",\"games\":[";
        for (std::size_t index = 0u; index < value.games.size(); ++index) {
            if (index != 0u) output << ',';
            output << encodeGame(value.games[index]);
        }
        output << "]}";
        return output.str();
    }, outputDiagnostic);
}

std::string encodeTwoPlayerSetupDocument(const TwoPlayerSetupDocument& document,
                                         SchemaDiagnostic* outputDiagnostic) {
    return encodeValidated(document, validateTwoPlayerSetupDocument,
                           [](const TwoPlayerSetupDocument& value) {
        std::ostringstream output;
        output << "{\"schema_version\":" << value.schemaVersion << ",\"setups\":[";
        for (std::size_t index = 0u; index < value.setups.size(); ++index) {
            if (index != 0u) output << ',';
            output << encodeSetup(value.setups[index]);
        }
        output << "]}";
        return output.str();
    }, outputDiagnostic);
}

std::string encodeRuntimeSessionSelection(const RuntimeSessionSelection& selection,
                                          SchemaDiagnostic* outputDiagnostic) {
    // Cross-reference validation requires the store family and is intentionally
    // performed by validateRuntimeSessionSelection(). Encoding still enforces
    // local bounds/identifier shape so the temporary payload is never unbounded.
    auto localValidate = [](const RuntimeSessionSelection& value) {
        if (value.schemaVersion != kProfileSchemaVersion) {
            return diagnostic(SchemaResult::UnsupportedVersion, "unsupported runtime schema version");
        }
        if (value.bindings.empty() || value.bindings.size() > kMaximumRuntimeBindings) {
            return diagnostic(SchemaResult::BoundsExceeded,
                              "runtime selection must contain one or two bindings");
        }
        std::set<SeatId> seats;
        for (const auto& binding : value.bindings) {
            if (binding.seatId == 0u || !validIdentifier(binding.playerId) ||
                !validIdentifier(binding.gameId) ||
                (binding.setupId && !validIdentifier(*binding.setupId))) {
                return diagnostic(SchemaResult::InvalidValue,
                                  "invalid runtime binding field");
            }
            if (!seats.insert(binding.seatId).second) {
                return diagnostic(SchemaResult::DuplicateId, "duplicate runtime Seat binding");
            }
        }
        return ok();
    };
    return encodeValidated(selection, localValidate,
                           [](const RuntimeSessionSelection& value) {
        std::ostringstream output;
        output << "{\"schema_version\":" << value.schemaVersion << ",\"bindings\":[";
        for (std::size_t index = 0u; index < value.bindings.size(); ++index) {
            if (index != 0u) output << ',';
            output << encodeBinding(value.bindings[index]);
        }
        output << "]}";
        return output.str();
    }, outputDiagnostic);
}

SchemaDiagnostic decodeSeatConfigDocument(std::string_view json,
                                          SeatConfigDocument& document) {
    return decodeValidated(json, document, [](const JsonValue& root) {
        const auto& object = asObject(root, "SeatConfigDocument");
        exactFields(object, {"schema_version", "management_seat_id", "seats"});
        SeatConfigDocument result;
        result.schemaVersion = schemaVersion(object);
        result.managementSeatId = asU32(required(object, "management_seat_id"),
                                        "management_seat_id");
        for (const auto& seat : asArray(required(object, "seats"), "seats")) {
            result.seats.push_back(decodeSeat(seat));
        }
        return result;
    }, validateSeatConfigDocument);
}

SchemaDiagnostic decodePlayerProfileDocument(std::string_view json,
                                             PlayerProfileDocument& document) {
    return decodeValidated(json, document, [](const JsonValue& root) {
        const auto& object = asObject(root, "PlayerProfileDocument");
        exactFields(object, {"schema_version", "players"});
        PlayerProfileDocument result;
        result.schemaVersion = schemaVersion(object);
        for (const auto& player : asArray(required(object, "players"), "players")) {
            result.players.push_back(decodePlayer(player));
        }
        return result;
    }, validatePlayerProfileDocument);
}

SchemaDiagnostic decodeGameRecordDocument(std::string_view json,
                                          GameRecordDocument& document) {
    return decodeValidated(json, document, [](const JsonValue& root) {
        const auto& object = asObject(root, "GameRecordDocument");
        exactFields(object, {"schema_version", "games"});
        GameRecordDocument result;
        result.schemaVersion = schemaVersion(object);
        for (const auto& game : asArray(required(object, "games"), "games")) {
            result.games.push_back(decodeGame(game));
        }
        return result;
    }, validateGameRecordDocument);
}

SchemaDiagnostic decodeTwoPlayerSetupDocument(std::string_view json,
                                              TwoPlayerSetupDocument& document) {
    return decodeValidated(json, document, [](const JsonValue& root) {
        const auto& object = asObject(root, "TwoPlayerSetupDocument");
        exactFields(object, {"schema_version", "setups"});
        TwoPlayerSetupDocument result;
        result.schemaVersion = schemaVersion(object);
        for (const auto& setup : asArray(required(object, "setups"), "setups")) {
            result.setups.push_back(decodeSetup(setup));
        }
        return result;
    }, validateTwoPlayerSetupDocument);
}

SchemaDiagnostic decodeRuntimeSessionSelection(std::string_view json,
                                               RuntimeSessionSelection& selection) {
    auto localValidate = [](const RuntimeSessionSelection& value) {
        if (value.schemaVersion != kProfileSchemaVersion) {
            return diagnostic(SchemaResult::UnsupportedVersion, "unsupported runtime schema version");
        }
        if (value.bindings.empty() || value.bindings.size() > kMaximumRuntimeBindings) {
            return diagnostic(SchemaResult::BoundsExceeded,
                              "runtime selection must contain one or two bindings");
        }
        std::set<SeatId> seats;
        for (const auto& binding : value.bindings) {
            if (binding.seatId == 0u || !validIdentifier(binding.playerId) ||
                !validIdentifier(binding.gameId) ||
                (binding.setupId && !validIdentifier(*binding.setupId))) {
                return diagnostic(SchemaResult::InvalidValue,
                                  "invalid runtime binding field");
            }
            if (!seats.insert(binding.seatId).second) {
                return diagnostic(SchemaResult::DuplicateId, "duplicate runtime Seat binding");
            }
        }
        return ok();
    };
    return decodeValidated(json, selection, [](const JsonValue& root) {
        const auto& object = asObject(root, "RuntimeSessionSelection");
        exactFields(object, {"schema_version", "bindings"});
        RuntimeSessionSelection result;
        result.schemaVersion = schemaVersion(object);
        for (const auto& binding : asArray(required(object, "bindings"), "bindings")) {
            result.bindings.push_back(decodeBinding(binding));
        }
        return result;
    }, localValidate);
}

std::string_view schemaResultName(SchemaResult result) noexcept {
    switch (result) {
    case SchemaResult::Success: return "success";
    case SchemaResult::ParseError: return "parse-error";
    case SchemaResult::DocumentTooLarge: return "document-too-large";
    case SchemaResult::UnsupportedVersion: return "unsupported-version";
    case SchemaResult::UnknownField: return "unknown-field";
    case SchemaResult::MissingField: return "missing-field";
    case SchemaResult::WrongType: return "wrong-type";
    case SchemaResult::InvalidValue: return "invalid-value";
    case SchemaResult::BoundsExceeded: return "bounds-exceeded";
    case SchemaResult::DuplicateId: return "duplicate-id";
    case SchemaResult::RuntimeOnlyStatePresent: return "runtime-only-state-present";
    case SchemaResult::CrossReferenceError: return "cross-reference-error";
    }
    return "unknown";
}

std::string_view gameOriginName(GameOrigin origin) noexcept {
    switch (origin) {
    case GameOrigin::Discovered: return "discovered";
    case GameOrigin::Manual: return "manual";
    }
    return "unknown";
}

} // namespace hydra::profile
