#include "hydra/compatibility_result.hpp"
#include "hydra/internal/strict_json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace hydra::compat {
namespace {

struct CompatException final : std::runtime_error {
    CompatException(CompatibilityResultCode value, std::string message)
        : std::runtime_error(std::move(message)), code(value) {}
    CompatibilityResultCode code;
};

[[noreturn]] void fail(CompatibilityResultCode code, std::string message) {
    throw CompatException(code, std::move(message));
}

CompatibilityDiagnostic diagnostic(CompatibilityResultCode code, std::string message) {
    return {code, std::move(message)};
}

using JsonNumber = internal::json::Number;
using JsonValue = internal::json::Value;

JsonValue parseJson(std::string_view text) {
    try {
        return internal::json::parse(text, internal::json::ParseOptions{32u, 4096u});
    } catch (const internal::json::ParseError& error) {
        fail(CompatibilityResultCode::ParseError, error.what());
    }
}

const JsonValue::Object& objectOf(const JsonValue& value, std::string_view field) {
    const auto* object = std::get_if<JsonValue::Object>(&value.value);
    if (object == nullptr) fail(CompatibilityResultCode::WrongType,
                                std::string(field) + " must be an object");
    return *object;
}

const JsonValue::Array& arrayOf(const JsonValue& value, std::string_view field) {
    const auto* array = std::get_if<JsonValue::Array>(&value.value);
    if (array == nullptr) fail(CompatibilityResultCode::WrongType,
                               std::string(field) + " must be an array");
    return *array;
}

const std::string& stringOf(const JsonValue& value, std::string_view field) {
    const auto* text = std::get_if<std::string>(&value.value);
    if (text == nullptr) fail(CompatibilityResultCode::WrongType,
                              std::string(field) + " must be a string");
    return *text;
}

bool boolOf(const JsonValue& value, std::string_view field) {
    const auto* boolean = std::get_if<bool>(&value.value);
    if (boolean == nullptr) fail(CompatibilityResultCode::WrongType,
                                 std::string(field) + " must be boolean");
    return *boolean;
}

std::uint64_t uintOf(const JsonValue& value, std::string_view field) {
    const auto* number = std::get_if<JsonNumber>(&value.value);
    if (number == nullptr || number->text.empty() || number->text.front() == '-') {
        fail(CompatibilityResultCode::WrongType, std::string(field) + " must be uint64");
    }
    std::uint64_t output = 0u;
    const auto parsed = std::from_chars(number->text.data(),
                                        number->text.data() + number->text.size(), output);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size()) {
        fail(CompatibilityResultCode::WrongType, std::string(field) + " must be uint64");
    }
    return output;
}

const JsonValue& required(const JsonValue::Object& object, std::string_view field) {
    const auto found = object.find(std::string(field));
    if (found == object.end()) {
        fail(CompatibilityResultCode::MissingField, std::string("missing field: ") +
             std::string(field));
    }
    return found->second;
}

void exactFields(const JsonValue::Object& object,
                 std::initializer_list<std::string_view> expected) {
    std::set<std::string_view> names(expected.begin(), expected.end());
    for (const auto& [key, ignored] : object) {
        (void)ignored;
        if (!names.contains(key)) {
            fail(CompatibilityResultCode::UnknownField, "unknown field: " + key);
        }
    }
    for (const auto name : names) (void)required(object, name);
}

std::optional<std::string> optionalString(const JsonValue& value, std::string_view field) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) return std::nullopt;
    return stringOf(value, field);
}

std::optional<std::uint64_t> optionalUint(const JsonValue& value, std::string_view field) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) return std::nullopt;
    return uintOf(value, field);
}

bool validUtf8(std::string_view value) noexcept {
    std::size_t position = 0u;
    while (position < value.size()) {
        const auto first = static_cast<unsigned char>(value[position++]);
        if (first <= 0x7fu) continue;
        std::uint32_t codePoint = 0u;
        std::size_t continuation = 0u;
        if ((first & 0xe0u) == 0xc0u) {
            codePoint = first & 0x1fu;
            continuation = 1u;
            if (codePoint == 0u) return false;
        } else if ((first & 0xf0u) == 0xe0u) {
            codePoint = first & 0x0fu;
            continuation = 2u;
        } else if ((first & 0xf8u) == 0xf0u) {
            codePoint = first & 0x07u;
            continuation = 3u;
        } else {
            return false;
        }
        if (position + continuation > value.size()) return false;
        for (std::size_t index = 0u; index < continuation; ++index) {
            const auto next = static_cast<unsigned char>(value[position++]);
            if ((next & 0xc0u) != 0x80u) return false;
            codePoint = (codePoint << 6u) | (next & 0x3fu);
        }
        if ((continuation == 1u && codePoint < 0x80u) ||
            (continuation == 2u && codePoint < 0x800u) ||
            (continuation == 3u && codePoint < 0x10000u) ||
            codePoint > 0x10ffffu || (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

bool validIdentifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumCompatibilityIdentifierBytes ||
        !validUtf8(value)) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
              ch == ':' || ch == '@' || ch == '+')) {
            return false;
        }
    }
    return true;
}

bool validVersion(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumCompatibilityVersionBytes || !validUtf8(value)) {
        return false;
    }
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
              ch == ':' || ch == '+' || ch == '@')) {
            return false;
        }
    }
    return true;
}

bool digits(std::string_view value, std::size_t begin, std::size_t count) noexcept {
    if (begin + count > value.size()) return false;
    for (std::size_t index = begin; index < begin + count; ++index) {
        if (value[index] < '0' || value[index] > '9') return false;
    }
    return true;
}

unsigned decimal2(std::string_view value, std::size_t offset) noexcept {
    return static_cast<unsigned>((value[offset] - '0') * 10 + (value[offset + 1u] - '0'));
}

bool validTimestamp(TimestampClass cls, std::string_view value) noexcept {
    if (cls == TimestampClass::MonthBucket) {
        return value.size() == 7u && digits(value, 0u, 4u) && value[4] == '-' &&
               digits(value, 5u, 2u) && decimal2(value, 5u) >= 1u && decimal2(value, 5u) <= 12u;
    }
    if (cls == TimestampClass::DayBucket) {
        return value.size() == 10u && digits(value, 0u, 4u) && value[4] == '-' &&
               digits(value, 5u, 2u) && value[7] == '-' && digits(value, 8u, 2u) &&
               decimal2(value, 5u) >= 1u && decimal2(value, 5u) <= 12u &&
               decimal2(value, 8u) >= 1u && decimal2(value, 8u) <= 31u;
    }
    return false;
}

bool validTimestampClass(TimestampClass value) noexcept {
    return value == TimestampClass::DayBucket || value == TimestampClass::MonthBucket;
}

bool validScenario(Scenario value) noexcept {
    return value == Scenario::DifferentGames || value == Scenario::SameGameTwoInstance ||
           value == Scenario::ProtectedExperiment;
}

bool validStatus(EvidenceStatus value) noexcept {
    return value == EvidenceStatus::NotMeasured || value == EvidenceStatus::Pass ||
           value == EvidenceStatus::Fail || value == EvidenceStatus::Unsupported;
}

bool validOrigin(ResultOrigin value) noexcept {
    return value == ResultOrigin::Synthetic || value == ResultOrigin::ControlledProcess ||
           value == ResultOrigin::Physical || value == ResultOrigin::ImportedCommunity;
}

TimestampClass parseTimestampClass(std::string_view value) {
    if (value == "day") return TimestampClass::DayBucket;
    if (value == "month") return TimestampClass::MonthBucket;
    fail(CompatibilityResultCode::InvalidEnum, "unknown timestamp_class");
}

Scenario parseScenario(std::string_view value) {
    if (value == "different-games") return Scenario::DifferentGames;
    if (value == "same-game-two-instance") return Scenario::SameGameTwoInstance;
    if (value == "protected-experiment") return Scenario::ProtectedExperiment;
    fail(CompatibilityResultCode::InvalidEnum, "unknown scenario");
}

EvidenceStatus parseStatus(std::string_view value) {
    if (value == "not-measured") return EvidenceStatus::NotMeasured;
    if (value == "pass") return EvidenceStatus::Pass;
    if (value == "fail") return EvidenceStatus::Fail;
    if (value == "unsupported") return EvidenceStatus::Unsupported;
    fail(CompatibilityResultCode::InvalidEnum, "unknown evidence status");
}

ResultOrigin parseOrigin(std::string_view value) {
    if (value == "synthetic") return ResultOrigin::Synthetic;
    if (value == "controlled-process") return ResultOrigin::ControlledProcess;
    if (value == "physical") return ResultOrigin::Physical;
    if (value == "imported-community") return ResultOrigin::ImportedCommunity;
    fail(CompatibilityResultCode::InvalidEnum, "unknown result origin");
}

void appendEscaped(std::string& output, std::string_view value) {
    output.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch < 0x20u) {
                    output += "\\u00";
                    output.push_back(hex[(ch >> 4u) & 0x0fu]);
                    output.push_back(hex[ch & 0x0fu]);
                } else {
                    output.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    output.push_back('"');
}

void appendOptionalString(std::string& output, const std::optional<std::string>& value) {
    if (!value) output += "null";
    else appendEscaped(output, *value);
}

void appendOptionalUint(std::string& output, const std::optional<std::uint64_t>& value) {
    if (!value) output += "null";
    else output += std::to_string(*value);
}

EvidenceStatus fromVerdict(metrics::EvidenceVerdict value) noexcept {
    switch (value) {
        case metrics::EvidenceVerdict::Pass: return EvidenceStatus::Pass;
        case metrics::EvidenceVerdict::Fail: return EvidenceStatus::Fail;
        case metrics::EvidenceVerdict::InsufficientEvidence: return EvidenceStatus::NotMeasured;
    }
    return EvidenceStatus::NotMeasured;
}

ResultOrigin fromOrigin(metrics::EvidenceOrigin value) noexcept {
    switch (value) {
        case metrics::EvidenceOrigin::Synthetic: return ResultOrigin::Synthetic;
        case metrics::EvidenceOrigin::ControlledProcess: return ResultOrigin::ControlledProcess;
        case metrics::EvidenceOrigin::Physical: return ResultOrigin::Physical;
    }
    return ResultOrigin::Synthetic;
}

template <typename Selector>
EvidenceStatus aggregateCapability(const std::vector<metrics::SeatSessionMetrics>& seats,
                                   Selector selector) noexcept {
    bool measured = false;
    bool unsupported = false;
    for (const auto& seat : seats) {
        const auto value = selector(seat);
        switch (value) {
            case metrics::CapabilityOutcome::Failed: return EvidenceStatus::Fail;
            case metrics::CapabilityOutcome::MissingEvidence: return EvidenceStatus::NotMeasured;
            case metrics::CapabilityOutcome::Unsupported: unsupported = true; break;
            case metrics::CapabilityOutcome::Success: measured = true; break;
            case metrics::CapabilityOutcome::NotRequired: break;
        }
    }
    if (unsupported) return EvidenceStatus::Unsupported;
    return measured ? EvidenceStatus::Pass : EvidenceStatus::NotMeasured;
}

CompatibilityResult decodeObject(const JsonValue::Object& root) {
    exactFields(root, {"schema_version", "result_id", "timestamp_class", "timestamp_bucket",
                       "game", "environment", "scenario", "protected_experimental",
                       "setup_revision", "backends", "outcomes", "measurements", "origin",
                       "redaction", "provenance"});

    const auto schema = uintOf(required(root, "schema_version"), "schema_version");
    if (schema != kCompatibilityResultSchemaVersion) {
        fail(CompatibilityResultCode::UnsupportedSchema, "unsupported compatibility result schema");
    }

    CompatibilityResult result;
    result.schemaVersion = static_cast<std::uint32_t>(schema);
    result.resultId = stringOf(required(root, "result_id"), "result_id");
    result.timestampClass = parseTimestampClass(
        stringOf(required(root, "timestamp_class"), "timestamp_class"));
    result.timestampBucket = stringOf(required(root, "timestamp_bucket"), "timestamp_bucket");

    const auto& game = objectOf(required(root, "game"), "game");
    exactFields(game, {"game_id", "provider_id", "provider_app_id", "version"});
    result.gameId = stringOf(required(game, "game_id"), "game.game_id");
    result.providerId = stringOf(required(game, "provider_id"), "game.provider_id");
    result.providerAppId = optionalString(required(game, "provider_app_id"), "game.provider_app_id");
    result.gameVersion = optionalString(required(game, "version"), "game.version");

    const auto& environment = objectOf(required(root, "environment"), "environment");
    exactFields(environment, {"hydraseat_version", "hydraseat_build", "windows_build_class",
                              "architecture"});
    result.hydraSeatVersion = stringOf(required(environment, "hydraseat_version"),
                                       "environment.hydraseat_version");
    result.hydraSeatBuild = stringOf(required(environment, "hydraseat_build"),
                                     "environment.hydraseat_build");
    result.windowsBuildClass = stringOf(required(environment, "windows_build_class"),
                                        "environment.windows_build_class");
    result.architecture = stringOf(required(environment, "architecture"),
                                   "environment.architecture");

    result.scenario = parseScenario(stringOf(required(root, "scenario"), "scenario"));
    result.protectedExperimental = boolOf(required(root, "protected_experimental"),
                                          "protected_experimental");
    result.setupRevision = optionalUint(required(root, "setup_revision"), "setup_revision");

    const auto& backends = arrayOf(required(root, "backends"), "backends");
    if (backends.size() > kMaximumCompatibilityBackends) {
        fail(CompatibilityResultCode::TooLarge, "too many compatibility backend entries");
    }
    for (const auto& item : backends) {
        const auto& object = objectOf(item, "backends[]");
        exactFields(object, {"backend_id", "version", "status"});
        BackendEvidence backend;
        backend.backendId = stringOf(required(object, "backend_id"), "backends[].backend_id");
        backend.version = optionalString(required(object, "version"), "backends[].version");
        backend.status = parseStatus(stringOf(required(object, "status"), "backends[].status"));
        result.backends.push_back(std::move(backend));
    }

    const auto& outcomes = objectOf(required(root, "outcomes"), "outcomes");
    exactFields(outcomes, {"launch", "second_instance", "input_isolation", "controller",
                           "audio", "clean_exit", "rollback"});
    result.launch = parseStatus(stringOf(required(outcomes, "launch"), "outcomes.launch"));
    result.secondInstance = parseStatus(stringOf(required(outcomes, "second_instance"),
                                                 "outcomes.second_instance"));
    result.inputIsolation = parseStatus(stringOf(required(outcomes, "input_isolation"),
                                                  "outcomes.input_isolation"));
    result.controller = parseStatus(stringOf(required(outcomes, "controller"),
                                             "outcomes.controller"));
    result.audio = parseStatus(stringOf(required(outcomes, "audio"), "outcomes.audio"));
    result.cleanExit = parseStatus(stringOf(required(outcomes, "clean_exit"),
                                            "outcomes.clean_exit"));
    result.rollback = parseStatus(stringOf(required(outcomes, "rollback"),
                                           "outcomes.rollback"));

    const auto& measurements = objectOf(required(root, "measurements"), "measurements");
    exactFields(measurements, {"launch_duration_us", "stop_duration_us", "rollback_duration_us",
                               "observed_input_events", "verified_cross_seat_events",
                               "input_latency_p95_us"});
    result.measurements.launchDurationMicros = optionalUint(
        required(measurements, "launch_duration_us"), "measurements.launch_duration_us");
    result.measurements.stopDurationMicros = optionalUint(
        required(measurements, "stop_duration_us"), "measurements.stop_duration_us");
    result.measurements.rollbackDurationMicros = optionalUint(
        required(measurements, "rollback_duration_us"), "measurements.rollback_duration_us");
    result.measurements.observedInputEvents = optionalUint(
        required(measurements, "observed_input_events"), "measurements.observed_input_events");
    result.measurements.verifiedCrossSeatEvents = optionalUint(
        required(measurements, "verified_cross_seat_events"),
        "measurements.verified_cross_seat_events");
    result.measurements.inputLatencyP95Micros = optionalUint(
        required(measurements, "input_latency_p95_us"), "measurements.input_latency_p95_us");

    result.origin = parseOrigin(stringOf(required(root, "origin"), "origin"));

    const auto& redaction = objectOf(required(root, "redaction"), "redaction");
    exactFields(redaction, {"schema_version", "credentials_excluded", "player_names_excluded",
                            "personal_paths_excluded", "raw_typed_text_excluded"});
    const auto redactionSchema = uintOf(required(redaction, "schema_version"),
                                        "redaction.schema_version");
    if (redactionSchema > std::numeric_limits<std::uint32_t>::max()) {
        fail(CompatibilityResultCode::InvalidRedaction, "redaction schema is out of range");
    }
    result.redaction.schemaVersion = static_cast<std::uint32_t>(redactionSchema);
    result.redaction.credentialsExcluded = boolOf(required(redaction, "credentials_excluded"),
                                                   "redaction.credentials_excluded");
    result.redaction.playerNamesExcluded = boolOf(required(redaction, "player_names_excluded"),
                                                   "redaction.player_names_excluded");
    result.redaction.personalPathsExcluded = boolOf(required(redaction, "personal_paths_excluded"),
                                                     "redaction.personal_paths_excluded");
    result.redaction.rawTypedTextExcluded = boolOf(required(redaction, "raw_typed_text_excluded"),
                                                    "redaction.raw_typed_text_excluded");

    const auto& provenance = objectOf(required(root, "provenance"), "provenance");
    exactFields(provenance, {"source_id", "source_revision"});
    result.provenanceId = stringOf(required(provenance, "source_id"), "provenance.source_id");
    result.provenanceRevision = uintOf(required(provenance, "source_revision"),
                                       "provenance.source_revision");
    return result;
}

} // namespace

CompatibilityDiagnostic validateCompatibilityResult(const CompatibilityResult& value) {
    try {
        if (value.schemaVersion != kCompatibilityResultSchemaVersion) {
            fail(CompatibilityResultCode::UnsupportedSchema, "unsupported compatibility result schema");
        }
        if (!validIdentifier(value.resultId) || !validIdentifier(value.gameId) ||
            !validIdentifier(value.providerId) ||
            (value.providerAppId && !validIdentifier(*value.providerAppId))) {
            fail(CompatibilityResultCode::InvalidIdentifier,
                 "result/game/provider identity must be bounded path-free identifiers");
        }
        if (!validTimestampClass(value.timestampClass) ||
            !validTimestamp(value.timestampClass, value.timestampBucket)) {
            fail(CompatibilityResultCode::InvalidTimestamp,
                 "timestamp bucket does not match its declared privacy class");
        }
        if ((value.gameVersion && !validVersion(*value.gameVersion)) ||
            !validVersion(value.hydraSeatVersion) || !validVersion(value.hydraSeatBuild) ||
            !validVersion(value.windowsBuildClass) || !validIdentifier(value.architecture)) {
            fail(CompatibilityResultCode::InvalidVersion,
                 "version/build values must be bounded path-free public tokens");
        }
        if (!validScenario(value.scenario) || !validOrigin(value.origin) ||
            !validStatus(value.launch) || !validStatus(value.secondInstance) ||
            !validStatus(value.inputIsolation) || !validStatus(value.controller) ||
            !validStatus(value.audio) || !validStatus(value.cleanExit) ||
            !validStatus(value.rollback)) {
            fail(CompatibilityResultCode::InvalidEnum, "compatibility result contains unknown enum");
        }
        if ((value.scenario == Scenario::ProtectedExperiment) != value.protectedExperimental) {
            fail(CompatibilityResultCode::InvalidLocalEvidence,
                 "protected/experimental flag must match the protected scenario class");
        }
        if (value.setupRevision && *value.setupRevision == 0u) {
            fail(CompatibilityResultCode::InvalidLocalEvidence,
                 "material setup revision must be nonzero when present");
        }
        if (value.backends.size() > kMaximumCompatibilityBackends) {
            fail(CompatibilityResultCode::TooLarge, "too many backend evidence records");
        }
        std::set<std::string> backendIds;
        for (const auto& backend : value.backends) {
            if (!validIdentifier(backend.backendId) ||
                (backend.version && !validVersion(*backend.version)) ||
                !validStatus(backend.status)) {
                fail(CompatibilityResultCode::InvalidIdentifier,
                     "backend evidence contains invalid public identity/version/status");
            }
            if (!backendIds.insert(backend.backendId).second) {
                fail(CompatibilityResultCode::DuplicateBackend, "duplicate backend evidence identity");
            }
        }
        if (value.redaction.schemaVersion != kCompatibilityRedactionSchemaVersion ||
            !value.redaction.credentialsExcluded || !value.redaction.playerNamesExcluded ||
            !value.redaction.personalPathsExcluded || !value.redaction.rawTypedTextExcluded) {
            fail(CompatibilityResultCode::InvalidRedaction,
                 "public compatibility result requires the complete v1 redaction contract");
        }
        if (!validIdentifier(value.provenanceId) || value.provenanceRevision == 0u) {
            fail(CompatibilityResultCode::InvalidProvenance,
                 "public compatibility result requires bounded provenance identity/revision");
        }
        if (value.launch == EvidenceStatus::NotMeasured &&
            value.measurements.launchDurationMicros.has_value()) {
            fail(CompatibilityResultCode::InvalidMeasurement,
                 "launch duration cannot exist when launch was not measured");
        }
        if (value.cleanExit == EvidenceStatus::NotMeasured &&
            value.measurements.stopDurationMicros.has_value()) {
            fail(CompatibilityResultCode::InvalidMeasurement,
                 "stop duration cannot exist when clean exit was not measured");
        }
        if (value.rollback == EvidenceStatus::NotMeasured &&
            value.measurements.rollbackDurationMicros.has_value()) {
            fail(CompatibilityResultCode::InvalidMeasurement,
                 "rollback duration cannot exist when rollback was not measured");
        }
        if (value.inputIsolation == EvidenceStatus::NotMeasured &&
            (value.measurements.observedInputEvents.has_value() ||
             value.measurements.verifiedCrossSeatEvents.has_value() ||
             value.measurements.inputLatencyP95Micros.has_value())) {
            fail(CompatibilityResultCode::InvalidMeasurement,
                 "input measurements cannot exist when input isolation was not measured");
        }
        return {};
    } catch (const CompatException& exception) {
        return diagnostic(exception.code, exception.what());
    } catch (...) {
        return diagnostic(CompatibilityResultCode::InvalidLocalEvidence,
                          "compatibility result validation failed unexpectedly");
    }
}

CompatibilityDiagnostic canonicalizeCompatibilityResult(CompatibilityResult& value) {
    const auto validation = validateCompatibilityResult(value);
    if (!validation.succeeded()) return validation;
    std::sort(value.backends.begin(), value.backends.end(), [](const auto& left, const auto& right) {
        if (left.backendId != right.backendId) return left.backendId < right.backendId;
        if (left.version != right.version) return left.version < right.version;
        return static_cast<std::uint8_t>(left.status) < static_cast<std::uint8_t>(right.status);
    });
    return {};
}

CompatibilityDiagnostic encodeCompatibilityResultJson(const CompatibilityResult& value,
                                                        std::string& output) {
    try {
        CompatibilityResult canonical = value;
        const auto validation = canonicalizeCompatibilityResult(canonical);
        if (!validation.succeeded()) return validation;

        std::string json;
        json.reserve(2048u);
        json += "{\"schema_version\":" + std::to_string(canonical.schemaVersion) +
                ",\"result_id\":";
        appendEscaped(json, canonical.resultId);
        json += ",\"timestamp_class\":";
        appendEscaped(json, timestampClassName(canonical.timestampClass));
        json += ",\"timestamp_bucket\":";
        appendEscaped(json, canonical.timestampBucket);
        json += ",\"game\":{\"game_id\":";
        appendEscaped(json, canonical.gameId);
        json += ",\"provider_id\":";
        appendEscaped(json, canonical.providerId);
        json += ",\"provider_app_id\":";
        appendOptionalString(json, canonical.providerAppId);
        json += ",\"version\":";
        appendOptionalString(json, canonical.gameVersion);
        json += "},\"environment\":{\"hydraseat_version\":";
        appendEscaped(json, canonical.hydraSeatVersion);
        json += ",\"hydraseat_build\":";
        appendEscaped(json, canonical.hydraSeatBuild);
        json += ",\"windows_build_class\":";
        appendEscaped(json, canonical.windowsBuildClass);
        json += ",\"architecture\":";
        appendEscaped(json, canonical.architecture);
        json += "},\"scenario\":";
        appendEscaped(json, scenarioName(canonical.scenario));
        json += ",\"protected_experimental\":";
        json += canonical.protectedExperimental ? "true" : "false";
        json += ",\"setup_revision\":";
        appendOptionalUint(json, canonical.setupRevision);
        json += ",\"backends\":[";
        for (std::size_t index = 0u; index < canonical.backends.size(); ++index) {
            if (index != 0u) json.push_back(',');
            const auto& backend = canonical.backends[index];
            json += "{\"backend_id\":";
            appendEscaped(json, backend.backendId);
            json += ",\"version\":";
            appendOptionalString(json, backend.version);
            json += ",\"status\":";
            appendEscaped(json, evidenceStatusName(backend.status));
            json.push_back('}');
        }
        json += "],\"outcomes\":{\"launch\":";
        appendEscaped(json, evidenceStatusName(canonical.launch));
        json += ",\"second_instance\":";
        appendEscaped(json, evidenceStatusName(canonical.secondInstance));
        json += ",\"input_isolation\":";
        appendEscaped(json, evidenceStatusName(canonical.inputIsolation));
        json += ",\"controller\":";
        appendEscaped(json, evidenceStatusName(canonical.controller));
        json += ",\"audio\":";
        appendEscaped(json, evidenceStatusName(canonical.audio));
        json += ",\"clean_exit\":";
        appendEscaped(json, evidenceStatusName(canonical.cleanExit));
        json += ",\"rollback\":";
        appendEscaped(json, evidenceStatusName(canonical.rollback));
        json += "},\"measurements\":{\"launch_duration_us\":";
        appendOptionalUint(json, canonical.measurements.launchDurationMicros);
        json += ",\"stop_duration_us\":";
        appendOptionalUint(json, canonical.measurements.stopDurationMicros);
        json += ",\"rollback_duration_us\":";
        appendOptionalUint(json, canonical.measurements.rollbackDurationMicros);
        json += ",\"observed_input_events\":";
        appendOptionalUint(json, canonical.measurements.observedInputEvents);
        json += ",\"verified_cross_seat_events\":";
        appendOptionalUint(json, canonical.measurements.verifiedCrossSeatEvents);
        json += ",\"input_latency_p95_us\":";
        appendOptionalUint(json, canonical.measurements.inputLatencyP95Micros);
        json += "},\"origin\":";
        appendEscaped(json, resultOriginName(canonical.origin));
        json += ",\"redaction\":{\"schema_version\":" +
                std::to_string(canonical.redaction.schemaVersion) +
                ",\"credentials_excluded\":" +
                std::string(canonical.redaction.credentialsExcluded ? "true" : "false") +
                ",\"player_names_excluded\":" +
                std::string(canonical.redaction.playerNamesExcluded ? "true" : "false") +
                ",\"personal_paths_excluded\":" +
                std::string(canonical.redaction.personalPathsExcluded ? "true" : "false") +
                ",\"raw_typed_text_excluded\":" +
                std::string(canonical.redaction.rawTypedTextExcluded ? "true" : "false") +
                "},\"provenance\":{\"source_id\":";
        appendEscaped(json, canonical.provenanceId);
        json += ",\"source_revision\":" + std::to_string(canonical.provenanceRevision) + "}}";

        if (json.size() > kMaximumCompatibilityResultBytes) {
            return diagnostic(CompatibilityResultCode::TooLarge,
                              "encoded compatibility result exceeds the bounded maximum");
        }
        output = std::move(json);
        return {};
    } catch (const CompatException& exception) {
        return diagnostic(exception.code, exception.what());
    } catch (...) {
        return diagnostic(CompatibilityResultCode::InvalidLocalEvidence,
                          "compatibility result encoding failed unexpectedly");
    }
}

CompatibilityDiagnostic decodeCompatibilityResultJson(std::string_view json,
                                                        CompatibilityResult& output) {
    if (json.size() > kMaximumCompatibilityResultBytes) {
        return diagnostic(CompatibilityResultCode::TooLarge,
                          "compatibility result JSON exceeds the bounded maximum");
    }
    try {
        const auto root = parseJson(json);
        CompatibilityResult decoded = decodeObject(objectOf(root, "root"));
        const auto canonical = canonicalizeCompatibilityResult(decoded);
        if (!canonical.succeeded()) return canonical;
        output = std::move(decoded);
        return {};
    } catch (const CompatException& exception) {
        return diagnostic(exception.code, exception.what());
    } catch (...) {
        return diagnostic(CompatibilityResultCode::ParseError,
                          "compatibility result JSON parsing failed unexpectedly");
    }
}

CompatibilityDiagnostic buildCompatibilityResultFromSessionMetrics(
    const LocalEvidenceContext& context,
    const metrics::SessionMetricsReport& report,
    CompatibilityResult& output) {
    if (report.schemaVersion != metrics::kSessionMetricsSchemaVersion ||
        report.planFingerprint == 0u || report.seats.empty() ||
        report.seats.size() > metrics::kMaximumSessionMetricSeats) {
        return diagnostic(CompatibilityResultCode::InvalidLocalEvidence,
                          "session metrics report is not a valid bounded local evidence source");
    }

    CompatibilityResult candidate;
    candidate.resultId = context.resultId;
    candidate.timestampClass = context.timestampClass;
    candidate.timestampBucket = context.timestampBucket;
    candidate.gameId = context.gameId;
    candidate.providerId = context.providerId;
    candidate.providerAppId = context.providerAppId;
    candidate.gameVersion = context.gameVersion;
    candidate.hydraSeatVersion = context.hydraSeatVersion;
    candidate.hydraSeatBuild = context.hydraSeatBuild;
    candidate.windowsBuildClass = context.windowsBuildClass;
    candidate.architecture = context.architecture;
    candidate.scenario = context.scenario;
    candidate.protectedExperimental = context.protectedExperimental;
    candidate.setupRevision = context.setupRevision;
    candidate.backends = context.backends;
    candidate.origin = fromOrigin(report.origin);
    candidate.provenanceId = context.provenanceId;
    candidate.provenanceRevision = context.provenanceRevision;

    const bool allStarted = std::all_of(report.seats.begin(), report.seats.end(),
                                        [](const auto& seat) { return seat.processStarted; });
    candidate.launch = allStarted ? EvidenceStatus::Pass : EvidenceStatus::Fail;
    if (context.scenario == Scenario::SameGameTwoInstance ||
        context.scenario == Scenario::ProtectedExperiment) {
        candidate.secondInstance = report.seats.size() == 2u && allStarted
                                       ? EvidenceStatus::Pass
                                       : EvidenceStatus::Fail;
    }
    candidate.inputIsolation = fromVerdict(report.isolationVerdict);
    candidate.controller = aggregateCapability(report.seats,
        [](const auto& seat) { return seat.controller; });
    candidate.audio = aggregateCapability(report.seats,
        [](const auto& seat) { return seat.audio; });
    switch (report.finalState) {
        case metrics::SessionFinalState::ReturnedToWindows:
            candidate.cleanExit = EvidenceStatus::Pass;
            break;
        case metrics::SessionFinalState::RecoveryRequired:
            candidate.cleanExit = EvidenceStatus::Fail;
            break;
        case metrics::SessionFinalState::Running:
            candidate.cleanExit = EvidenceStatus::NotMeasured;
            break;
    }
    if (report.rollbackAttempted) {
        candidate.rollback = report.rollbackVerified ? EvidenceStatus::Pass : EvidenceStatus::Fail;
    }

    if (report.maximumLaunchDurationMicros != 0u) {
        candidate.measurements.launchDurationMicros = report.maximumLaunchDurationMicros;
    }
    if (candidate.cleanExit != EvidenceStatus::NotMeasured && report.maximumStopDurationMicros != 0u) {
        candidate.measurements.stopDurationMicros = report.maximumStopDurationMicros;
    }
    if (candidate.rollback != EvidenceStatus::NotMeasured &&
        report.maximumRollbackDurationMicros != 0u) {
        candidate.measurements.rollbackDurationMicros = report.maximumRollbackDurationMicros;
    }
    if (candidate.inputIsolation != EvidenceStatus::NotMeasured &&
        report.input.uniqueInputEvents != 0u) {
        candidate.measurements.observedInputEvents = report.input.uniqueInputEvents;
        candidate.measurements.verifiedCrossSeatEvents = report.input.crossSeatEvents;
        if (report.input.endToEnd.count != 0u) {
            candidate.measurements.inputLatencyP95Micros = report.input.endToEnd.p95Micros;
        }
    }

    const auto canonical = canonicalizeCompatibilityResult(candidate);
    if (!canonical.succeeded()) return canonical;
    output = std::move(candidate);
    return {};
}

std::string_view timestampClassName(TimestampClass value) noexcept {
    switch (value) {
        case TimestampClass::DayBucket: return "day";
        case TimestampClass::MonthBucket: return "month";
    }
    return "unknown";
}

std::string_view scenarioName(Scenario value) noexcept {
    switch (value) {
        case Scenario::DifferentGames: return "different-games";
        case Scenario::SameGameTwoInstance: return "same-game-two-instance";
        case Scenario::ProtectedExperiment: return "protected-experiment";
    }
    return "unknown";
}

std::string_view evidenceStatusName(EvidenceStatus value) noexcept {
    switch (value) {
        case EvidenceStatus::NotMeasured: return "not-measured";
        case EvidenceStatus::Pass: return "pass";
        case EvidenceStatus::Fail: return "fail";
        case EvidenceStatus::Unsupported: return "unsupported";
    }
    return "unknown";
}

std::string_view resultOriginName(ResultOrigin value) noexcept {
    switch (value) {
        case ResultOrigin::Synthetic: return "synthetic";
        case ResultOrigin::ControlledProcess: return "controlled-process";
        case ResultOrigin::Physical: return "physical";
        case ResultOrigin::ImportedCommunity: return "imported-community";
    }
    return "unknown";
}

std::string_view compatibilityResultCodeName(CompatibilityResultCode value) noexcept {
    switch (value) {
        case CompatibilityResultCode::Success: return "Success";
        case CompatibilityResultCode::TooLarge: return "TooLarge";
        case CompatibilityResultCode::ParseError: return "ParseError";
        case CompatibilityResultCode::UnsupportedSchema: return "UnsupportedSchema";
        case CompatibilityResultCode::UnknownField: return "UnknownField";
        case CompatibilityResultCode::MissingField: return "MissingField";
        case CompatibilityResultCode::WrongType: return "WrongType";
        case CompatibilityResultCode::InvalidIdentifier: return "InvalidIdentifier";
        case CompatibilityResultCode::InvalidVersion: return "InvalidVersion";
        case CompatibilityResultCode::InvalidTimestamp: return "InvalidTimestamp";
        case CompatibilityResultCode::InvalidEnum: return "InvalidEnum";
        case CompatibilityResultCode::DuplicateBackend: return "DuplicateBackend";
        case CompatibilityResultCode::InvalidMeasurement: return "InvalidMeasurement";
        case CompatibilityResultCode::InvalidRedaction: return "InvalidRedaction";
        case CompatibilityResultCode::InvalidProvenance: return "InvalidProvenance";
        case CompatibilityResultCode::InvalidLocalEvidence: return "InvalidLocalEvidence";
    }
    return "Unknown";
}

} // namespace hydra::compat
