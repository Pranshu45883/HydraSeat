#include "hydra/game_runtime_requirement_resolver.hpp"

#include "hydra/internal/strict_json.hpp"

#ifdef _WIN32
#include "hydra/compatibility_local_store.hpp"
#include "hydra/custom_executable_provider.hpp"
#include "hydra/steam_provider.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace hydra::requirement {
namespace {

using internal::json::Number;
using internal::json::Value;

class StoreError final : public std::runtime_error {
public:
    StoreError(RequirementStoreCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    RequirementStoreCode code() const noexcept { return code_; }

private:
    RequirementStoreCode code_;
};

RequirementStoreDiagnostic storeFailure(RequirementStoreCode code, std::string message) {
    return {code, std::move(message)};
}

bool validUtf8(std::string_view input) noexcept;

bool validProfileIdentifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > profile::kMaximumIdentifierBytes) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        const bool alphaNumeric = (ch >= 'A' && ch <= 'Z') ||
                                  (ch >= 'a' && ch <= 'z') ||
                                  (ch >= '0' && ch <= '9');
        if (!alphaNumeric && ch != '.' && ch != '_' && ch != '-' && ch != ':') return false;
    }
    return true;
}

bool validEvidenceIdentifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > compat::kMaximumCompatibilityIdentifierBytes ||
        !validUtf8(value)) {
        return false;
    }
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        const bool alphaNumeric = (ch >= 'A' && ch <= 'Z') ||
                                  (ch >= 'a' && ch <= 'z') ||
                                  (ch >= '0' && ch <= '9');
        if (!alphaNumeric && ch != '-' && ch != '_' && ch != '.' && ch != ':' &&
            ch != '@' && ch != '+') {
            return false;
        }
    }
    return true;
}

bool validCompatibilityVersion(std::string_view value) noexcept {
    if (value.empty() || value.size() > compat::kMaximumCompatibilityVersionBytes ||
        !validUtf8(value)) {
        return false;
    }
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        const bool alphaNumeric = (ch >= 'A' && ch <= 'Z') ||
                                  (ch >= 'a' && ch <= 'z') ||
                                  (ch >= '0' && ch <= '9');
        if (!alphaNumeric && ch != '-' && ch != '_' && ch != '.' && ch != ':' &&
            ch != '+' && ch != '@') {
            return false;
        }
    }
    return true;
}

bool validSha256(std::string_view value) noexcept {
    if (value.size() != 64u) return false;
    for (const char ch : value) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'f';
        const bool upper = ch >= 'A' && ch <= 'F';
        if (!digit && !lower && !upper) return false;
    }
    return true;
}

bool validCompatibility(const profile::CompatibilityReference& value) noexcept {
    return validProfileIdentifier(value.recordId) && validProfileIdentifier(value.provenance) &&
           value.evidenceRevision != 0u;
}

void appendUtf8CodePoint(std::string& output, std::uint32_t codePoint) {
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

bool wideToUtf8(std::wstring_view input, std::string& output) {
    std::string converted;
    converted.reserve(input.size());
    for (std::size_t index = 0u; index < input.size(); ++index) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(input[index]);
        if constexpr (sizeof(wchar_t) == 2u) {
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu) {
                if (index + 1u >= input.size()) return false;
                const auto low = static_cast<std::uint32_t>(input[++index]);
                if (low < 0xdc00u || low > 0xdfffu) return false;
                codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) + (low - 0xdc00u);
            } else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
                return false;
            }
        }
        if (codePoint > 0x10ffffu || (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            return false;
        }
        appendUtf8CodePoint(converted, codePoint);
    }
    output = std::move(converted);
    return true;
}

bool validUtf8(std::string_view input) noexcept {
    std::size_t index = 0u;
    while (index < input.size()) {
        const auto lead = static_cast<unsigned char>(input[index++]);
        if (lead <= 0x7fu) {
            if (lead < 0x20u || lead == 0x7fu) return false;
            continue;
        }
        unsigned continuationCount = 0u;
        std::uint32_t codePoint = 0u;
        if ((lead & 0xe0u) == 0xc0u) {
            continuationCount = 1u;
            codePoint = lead & 0x1fu;
            if (codePoint < 2u) return false;
        } else if ((lead & 0xf0u) == 0xe0u) {
            continuationCount = 2u;
            codePoint = lead & 0x0fu;
        } else if ((lead & 0xf8u) == 0xf0u) {
            continuationCount = 3u;
            codePoint = lead & 0x07u;
            if (codePoint > 4u) return false;
        } else {
            return false;
        }
        if (index + continuationCount > input.size()) return false;
        for (unsigned count = 0u; count < continuationCount; ++count) {
            const auto continuation = static_cast<unsigned char>(input[index++]);
            if ((continuation & 0xc0u) != 0x80u) return false;
            codePoint = (codePoint << 6u) | (continuation & 0x3fu);
        }
        if ((continuationCount == 2u && codePoint < 0x800u) ||
            (continuationCount == 3u && codePoint < 0x10000u) ||
            codePoint > 0x10ffffu || (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

std::string quote(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const char raw : value) {
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
                constexpr char digits[] = "0123456789abcdef";
                output << "\\u00" << digits[(ch >> 4u) & 0x0fu] << digits[ch & 0x0fu];
            } else {
                output << static_cast<char>(ch);
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

std::string optionalStringJson(const std::optional<std::string>& value) {
    return value ? quote(*value) : "null";
}

std::string compatibilityJson(const std::optional<profile::CompatibilityReference>& value) {
    if (!value) return "null";
    std::ostringstream output;
    output << '{'
           << "\"record_id\":" << quote(value->recordId) << ','
           << "\"provenance\":" << quote(value->provenance) << ','
           << "\"evidence_revision\":" << value->evidenceRevision
           << '}';
    return output.str();
}

std::string requirementsJson(const launch::Requirements& value) {
    std::ostringstream output;
    output << '{'
           << "\"display\":" << (value.display ? "true" : "false") << ','
           << "\"keyboard\":" << (value.keyboard ? "true" : "false") << ','
           << "\"mouse\":" << (value.mouse ? "true" : "false") << ','
           << "\"controller\":" << (value.controller ? "true" : "false") << ','
           << "\"audio_output\":" << (value.audioOutput ? "true" : "false") << ','
           << "\"window_ownership\":" << (value.windowOwnership ? "true" : "false") << ','
           << "\"recovery\":" << (value.recovery ? "true" : "false") << ','
           << "\"high_risk\":" << (value.highRisk ? "true" : "false")
           << '}';
    return output.str();
}

std::string capabilitiesJson(const launch::Capabilities& value) {
    std::ostringstream output;
    output << '{'
           << "\"process\":" << (value.process ? "true" : "false") << ','
           << "\"window\":" << (value.window ? "true" : "false") << ','
           << "\"display\":" << (value.display ? "true" : "false") << ','
           << "\"input\":" << (value.input ? "true" : "false") << ','
           << "\"controller\":" << (value.controller ? "true" : "false") << ','
           << "\"audio\":" << (value.audio ? "true" : "false") << ','
           << "\"recovery\":" << (value.recovery ? "true" : "false")
           << '}';
    return output.str();
}

const Value::Object& asObject(const Value& value, std::string_view label) {
    const auto* object = std::get_if<Value::Object>(&value.value);
    if (object == nullptr) throw StoreError(RequirementStoreCode::ParseError,
                                           std::string(label) + " must be an object");
    return *object;
}

const Value::Array& asArray(const Value& value, std::string_view label) {
    const auto* array = std::get_if<Value::Array>(&value.value);
    if (array == nullptr) throw StoreError(RequirementStoreCode::ParseError,
                                          std::string(label) + " must be an array");
    return *array;
}

const Value& required(const Value::Object& object, std::string_view key) {
    const auto iterator = object.find(std::string(key));
    if (iterator == object.end()) {
        throw StoreError(RequirementStoreCode::ParseError,
                         "missing required field: " + std::string(key));
    }
    return iterator->second;
}

void requireFields(const Value::Object& object,
                   std::initializer_list<std::string_view> expected) {
    std::set<std::string_view> names(expected.begin(), expected.end());
    if (object.size() != names.size()) {
        for (const auto& [key, value] : object) {
            (void)value;
            if (!names.contains(key)) {
                throw StoreError(RequirementStoreCode::UnknownField,
                                 "unknown field: " + key);
            }
        }
        throw StoreError(RequirementStoreCode::ParseError,
                         "object is missing one or more required fields");
    }
    for (const auto& [key, value] : object) {
        (void)value;
        if (!names.contains(key)) {
            throw StoreError(RequirementStoreCode::UnknownField,
                             "unknown field: " + key);
        }
    }
}

std::string asString(const Value& value, std::string_view label) {
    const auto* text = std::get_if<std::string>(&value.value);
    if (text == nullptr) throw StoreError(RequirementStoreCode::ParseError,
                                         std::string(label) + " must be a string");
    return *text;
}

std::optional<std::string> asOptionalString(const Value& value, std::string_view label) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) return std::nullopt;
    return asString(value, label);
}

bool asBool(const Value& value, std::string_view label) {
    const auto* boolean = std::get_if<bool>(&value.value);
    if (boolean == nullptr) throw StoreError(RequirementStoreCode::ParseError,
                                            std::string(label) + " must be a boolean");
    return *boolean;
}

std::uint64_t asU64(const Value& value, std::string_view label) {
    const auto* number = std::get_if<Number>(&value.value);
    if (number == nullptr || number->text.empty() || number->text.front() == '-') {
        throw StoreError(RequirementStoreCode::ParseError,
                         std::string(label) + " must be an unsigned integer");
    }
    std::uint64_t parsed = 0u;
    const auto* begin = number->text.data();
    const auto* end = begin + number->text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw StoreError(RequirementStoreCode::ParseError,
                         std::string(label) + " is out of range");
    }
    return parsed;
}

std::uint32_t asU32(const Value& value, std::string_view label) {
    const auto parsed = asU64(value, label);
    if (parsed > (std::numeric_limits<std::uint32_t>::max)()) {
        throw StoreError(RequirementStoreCode::ParseError,
                         std::string(label) + " is out of range");
    }
    return static_cast<std::uint32_t>(parsed);
}

std::optional<profile::CompatibilityReference> parseCompatibility(const Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) return std::nullopt;
    const auto& object = asObject(value, "catalog_compatibility");
    requireFields(object, {"record_id", "provenance", "evidence_revision"});
    profile::CompatibilityReference reference;
    reference.recordId = asString(required(object, "record_id"), "record_id");
    reference.provenance = asString(required(object, "provenance"), "provenance");
    reference.evidenceRevision = asU32(required(object, "evidence_revision"),
                                       "evidence_revision");
    return reference;
}

launch::Requirements parseRequirements(const Value& value) {
    const auto& object = asObject(value, "requirements");
    requireFields(object, {"display", "keyboard", "mouse", "controller", "audio_output",
                           "window_ownership", "recovery", "high_risk"});
    launch::Requirements result;
    result.display = asBool(required(object, "display"), "display");
    result.keyboard = asBool(required(object, "keyboard"), "keyboard");
    result.mouse = asBool(required(object, "mouse"), "mouse");
    result.controller = asBool(required(object, "controller"), "controller");
    result.audioOutput = asBool(required(object, "audio_output"), "audio_output");
    result.windowOwnership = asBool(required(object, "window_ownership"), "window_ownership");
    result.recovery = asBool(required(object, "recovery"), "recovery");
    result.highRisk = asBool(required(object, "high_risk"), "high_risk");
    return result;
}

launch::Capabilities parseCapabilities(const Value& value) {
    const auto& object = asObject(value, "capabilities");
    requireFields(object,
                  {"process", "window", "display", "input", "controller", "audio", "recovery"});
    launch::Capabilities result;
    result.process = asBool(required(object, "process"), "process");
    result.window = asBool(required(object, "window"), "window");
    result.display = asBool(required(object, "display"), "display");
    result.input = asBool(required(object, "input"), "input");
    result.controller = asBool(required(object, "controller"), "controller");
    result.audio = asBool(required(object, "audio"), "audio");
    result.recovery = asBool(required(object, "recovery"), "recovery");
    return result;
}

LocalRequirementEvidenceRecord parseRecord(const Value& value, std::uint32_t schemaVersion) {
    const auto& object = asObject(value, "requirement record");
    if (schemaVersion == kLegacyRequirementEvidenceStoreSchemaVersion) {
        requireFields(object,
                      {"record_id", "revision", "game_id", "provider_id", "provider_app_id",
                       "game_version", "executable_sha256", "catalog_compatibility",
                       "provider_metadata_revision", "evidence_result_id", "evidence_provenance_id",
                       "evidence_provenance_revision", "requirements", "capabilities"});
    } else {
        requireFields(object,
                      {"record_id", "revision", "game_id", "provider_id", "provider_app_id",
                       "game_version", "executable_sha256", "catalog_compatibility",
                       "provider_metadata_revision", "evidence_result_id", "evidence_provenance_id",
                       "evidence_provenance_revision", "validated_seat_count", "requirements",
                       "capabilities"});
    }
    LocalRequirementEvidenceRecord record;
    record.recordId = asString(required(object, "record_id"), "record_id");
    record.revision = asU64(required(object, "revision"), "revision");
    record.gameId = asString(required(object, "game_id"), "game_id");
    record.providerId = asString(required(object, "provider_id"), "provider_id");
    record.providerAppId = asOptionalString(required(object, "provider_app_id"), "provider_app_id");
    record.gameVersionUtf8 = asOptionalString(required(object, "game_version"), "game_version");
    record.executableSha256 = asOptionalString(required(object, "executable_sha256"),
                                                "executable_sha256");
    record.catalogCompatibility = parseCompatibility(required(object, "catalog_compatibility"));
    record.providerMetadataRevision = asU64(required(object, "provider_metadata_revision"),
                                            "provider_metadata_revision");
    record.evidenceResultId = asString(required(object, "evidence_result_id"),
                                       "evidence_result_id");
    record.evidenceProvenanceId = asString(required(object, "evidence_provenance_id"),
                                           "evidence_provenance_id");
    record.evidenceProvenanceRevision = asU64(required(object, "evidence_provenance_revision"),
                                              "evidence_provenance_revision");
    if (schemaVersion == kLegacyRequirementEvidenceStoreSchemaVersion) {
        record.validatedSeatCount = 2u;
    } else {
        const auto seatCount = asU32(required(object, "validated_seat_count"),
                                     "validated_seat_count");
        if (seatCount < 1u || seatCount > 2u) {
            throw StoreError(RequirementStoreCode::InvalidRecord,
                             "validated_seat_count must be one or two");
        }
        record.validatedSeatCount = static_cast<std::uint8_t>(seatCount);
    }
    record.requirements = parseRequirements(required(object, "requirements"));
    record.capabilities = parseCapabilities(required(object, "capabilities"));
    return record;
}

bool parseMonth(std::string_view value, std::int64_t& output) noexcept {
    if (value.size() != 7u || value[4] != '-') return false;
    for (std::size_t index = 0u; index < value.size(); ++index) {
        if (index == 4u) continue;
        if (value[index] < '0' || value[index] > '9') return false;
    }
    const auto year = static_cast<std::int64_t>(value[0] - '0') * 1000 +
                      static_cast<std::int64_t>(value[1] - '0') * 100 +
                      static_cast<std::int64_t>(value[2] - '0') * 10 +
                      static_cast<std::int64_t>(value[3] - '0');
    const auto month = static_cast<std::int64_t>(value[5] - '0') * 10 +
                       static_cast<std::int64_t>(value[6] - '0');
    if (year < 2000 || year > 9999 || month < 1 || month > 12) return false;
    output = year * 12 + (month - 1);
    return true;
}

bool resultMonth(const compat::CompatibilityResult& result, std::int64_t& output) noexcept {
    if (result.timestampBucket.size() < 7u) return false;
    return parseMonth(std::string_view(result.timestampBucket).substr(0u, 7u), output);
}

const LocalRequirementEvidenceRecord* findStoredRecord(
    const RequirementEvidenceDocument& document,
    std::string_view gameId) noexcept {
    for (const auto& record : document.records) {
        if (record.gameId == gameId) return &record;
    }
    return nullptr;
}

provider::LauncherProviderAdapter* findProvider(
    std::span<const plan::ProviderAdapterBinding> providers,
    std::string_view providerId,
    const std::optional<std::string>& providerAppId,
    bool& duplicate) noexcept {
    provider::LauncherProviderAdapter* exact = nullptr;
    bool exactMatched = false;
    provider::LauncherProviderAdapter* providerWide = nullptr;
    duplicate = false;
    for (const auto& binding : providers) {
        if (binding.providerId != providerId) continue;
        if (binding.providerAppId) {
            if (!providerAppId || *binding.providerAppId != *providerAppId) continue;
            if (exactMatched) {
                duplicate = true;
                return nullptr;
            }
            exactMatched = true;
            exact = binding.adapter;
            continue;
        }
        if (providerWide != nullptr) {
            duplicate = true;
            return nullptr;
        }
        providerWide = binding.adapter;
    }
    return exactMatched ? exact : providerWide;
}

bool gameIdentityMatches(const LocalRequirementEvidenceRecord& stored,
                         const profile::GameRecord& game) {
    if (stored.gameId != game.gameId || stored.providerId != game.providerId ||
        stored.providerAppId != game.providerAppId ||
        stored.executableSha256 != game.executableSha256 ||
        stored.catalogCompatibility != game.compatibility) {
        return false;
    }
    std::optional<std::string> currentVersion;
    if (game.localVersion) {
        std::string converted;
        if (!wideToUtf8(*game.localVersion, converted)) return false;
        currentVersion = std::move(converted);
    }
    return stored.gameVersionUtf8 == currentVersion;
}

bool capabilitiesCover(const LocalRequirementEvidenceRecord& record) noexcept {
    const auto& needs = record.requirements;
    const auto& caps = record.capabilities;
    return caps.process && (!needs.windowOwnership || caps.window) &&
           (!needs.display || caps.display) &&
           (!(needs.keyboard || needs.mouse) || caps.input) &&
           (!needs.controller || caps.controller) &&
           (!needs.audioOutput || caps.audio) && (!needs.recovery || caps.recovery);
}

bool resultCoversRequirements(const LocalRequirementEvidenceRecord& record,
                              const compat::CompatibilityResult& result) noexcept {
    if (result.launch != compat::EvidenceStatus::Pass) return false;
    if ((record.requirements.keyboard || record.requirements.mouse) &&
        result.inputIsolation != compat::EvidenceStatus::Pass) {
        return false;
    }
    if (record.requirements.controller && result.controller != compat::EvidenceStatus::Pass) {
        return false;
    }
    if (record.requirements.audioOutput && result.audio != compat::EvidenceStatus::Pass) {
        return false;
    }
    if (record.requirements.recovery &&
        (result.cleanExit != compat::EvidenceStatus::Pass ||
         result.rollback != compat::EvidenceStatus::Pass)) {
        return false;
    }
    if (record.requirements.highRisk) {
        if (!result.protectedExperimental || result.scenario != compat::Scenario::ProtectedExperiment) {
            return false;
        }
    } else if (result.protectedExperimental) {
        return false;
    }
    return true;
}

bool resultIdentityMatches(const LocalRequirementEvidenceRecord& record,
                           const RequirementResolveContext& context,
                           const compat::CompatibilityResult& result) noexcept {
    return result.resultId == record.evidenceResultId &&
           result.provenanceId == record.evidenceProvenanceId &&
           result.provenanceRevision == record.evidenceProvenanceRevision &&
           result.gameId == record.gameId && result.providerId == record.providerId &&
           result.providerAppId == record.providerAppId &&
           result.gameVersion == record.gameVersionUtf8 &&
           result.hydraSeatVersion == context.hydraSeatVersion &&
           result.hydraSeatBuild == context.hydraSeatBuild &&
           result.windowsBuildClass == context.windowsBuildClass &&
           result.architecture == context.architecture;
}

bool originAllowed(LocalEvidenceTrust trust, compat::ResultOrigin origin) noexcept {
    if (origin == compat::ResultOrigin::ImportedCommunity ||
        origin == compat::ResultOrigin::Synthetic) {
        return false;
    }
    if (trust == LocalEvidenceTrust::PhysicalOnly) {
        return origin == compat::ResultOrigin::Physical;
    }
    return origin == compat::ResultOrigin::ControlledProcess ||
           origin == compat::ResultOrigin::Physical;
}

bool approvalStructurallyValid(const ProtectedRuntimeApproval& approval) noexcept {
    return validProfileIdentifier(approval.gameId) &&
           validProfileIdentifier(approval.providerId) &&
           (!approval.providerAppId || validProfileIdentifier(*approval.providerAppId)) &&
           approval.providerMetadataRevision != 0u &&
           validProfileIdentifier(approval.requirementRecordId) &&
           approval.requirementRevision != 0u &&
           validEvidenceIdentifier(approval.evidenceResultId) &&
           approval.evidenceProvenanceRevision != 0u;
}

bool approvalMatches(const ProtectedRuntimeApproval& approval,
                     const LocalRequirementEvidenceRecord& record) noexcept {
    return approval.gameId == record.gameId && approval.providerId == record.providerId &&
           approval.providerAppId == record.providerAppId &&
           approval.providerMetadataRevision == record.providerMetadataRevision &&
           approval.requirementRecordId == record.recordId &&
           approval.requirementRevision == record.revision &&
           approval.evidenceResultId == record.evidenceResultId &&
           approval.evidenceProvenanceRevision == record.evidenceProvenanceRevision;
}

void addIssue(TrustedRequirementSnapshot& snapshot,
              RequirementResolveCode code,
              std::string gameId,
              std::string message) {
    if (snapshot.blockedGames.size() >= kMaximumRequirementResolveIssues) return;
    snapshot.blockedGames.push_back({code, std::move(gameId), std::move(message)});
}

RequirementSnapshotDiagnostic snapshotFailure(RequirementSnapshotCode code,
                                              std::string message) {
    return {code, std::move(message)};
}

class UnavailableTrustedRequirementSource final : public ITrustedRequirementSource {
public:
    explicit UnavailableTrustedRequirementSource(std::string message)
        : message_(std::move(message)) {}

    RequirementSnapshotDiagnostic resolveCurrent(
        TrustedRequirementSnapshot& output) override {
        output = {};
        return snapshotFailure(RequirementSnapshotCode::InputUnavailable, message_);
    }

private:
    std::string message_;
};

#ifdef _WIN32

constexpr std::string_view kProductionHydraSeatVersion = "0.1.0";

std::optional<std::filesystem::path> localAppDataFile(
    std::wstring_view filename, std::string& error) {
    wchar_t localAppData[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length == 0u || length >= static_cast<DWORD>(std::size(localAppData))) {
        error = "LOCALAPPDATA is unavailable or exceeds the bounded path buffer";
        return std::nullopt;
    }
    return std::filesystem::path(localAppData) / L"HydraSeat" / filename;
}

bool loadManualGameRecords(profile::GameRecordDocument& output,
                           std::string& error) {
    output = {};
    const auto path = localAppDataFile(L"manual-games.json", error);
    if (!path) return false;

    std::error_code filesystemError;
    const bool exists = std::filesystem::exists(*path, filesystemError);
    if (filesystemError) {
        error = "manual game store existence check failed";
        return false;
    }
    if (!exists) return true;

    const auto byteCount = std::filesystem::file_size(*path, filesystemError);
    if (filesystemError || byteCount == 0u ||
        byteCount > profile::kMaximumSchemaDocumentBytes) {
        error = "manual game store is empty, unreadable, or exceeds its bound";
        return false;
    }
    std::ifstream input(*path, std::ios::binary);
    if (!input) {
        error = "manual game store could not be opened";
        return false;
    }
    std::string json(static_cast<std::size_t>(byteCount), '\0');
    input.read(json.data(), static_cast<std::streamsize>(json.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(json.size())) {
        error = "manual game store could not be read completely";
        return false;
    }
    char trailing = '\0';
    if (input.get(trailing)) {
        error = "manual game store changed while it was being read";
        return false;
    }

    profile::GameRecordDocument decoded;
    const auto diagnostic = profile::decodeGameRecordDocument(json, decoded);
    if (!diagnostic.succeeded()) {
        error = "manual game store failed schema validation: " + diagnostic.message;
        return false;
    }
    if (std::any_of(decoded.games.begin(), decoded.games.end(), [](const auto& game) {
            return game.providerId != "custom" ||
                   game.origin != profile::GameOrigin::Manual ||
                   !game.providerAppId || game.executableCandidates.empty();
        })) {
        error = "manual game store contains a non-custom or incomplete record";
        return false;
    }
    output = std::move(decoded);
    return true;
}

bool currentReferenceMonth(std::string& output, std::string& error) {
    SYSTEMTIME value{};
    GetSystemTime(&value);
    if (value.wYear < 2000u || value.wYear > 9999u ||
        value.wMonth < 1u || value.wMonth > 12u) {
        error = "current system month is outside the supported range";
        return false;
    }
    output = std::to_string(value.wYear) + "-";
    if (value.wMonth < 10u) output.push_back('0');
    output += std::to_string(value.wMonth);
    return true;
}

bool currentWindowsBuildClass(std::string& output, std::string& error) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        error = "ntdll.dll is unavailable for Windows build identification";
        return false;
    }
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtlGetVersion == nullptr) {
        error = "RtlGetVersion is unavailable for Windows build identification";
        return false;
    }
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlGetVersion(&version) != 0 || version.dwMajorVersion != 10u ||
        version.dwBuildNumber == 0u) {
        error = "current Windows version is outside the supported v1 build family";
        return false;
    }
    output = version.dwBuildNumber >= 22000u ? "win11-build-" : "win10-build-";
    output += std::to_string(version.dwBuildNumber);
    return true;
}

std::string currentArchitecture() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    return "unknown";
#endif
}

bool installedHydraSeatBuildIdentity(std::string& output, std::string& error) {
    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (length == 0u || length >= static_cast<DWORD>(std::size(modulePath))) {
        error = "current executable path is unavailable or exceeds the bounded path buffer";
        return false;
    }
    const auto productExecutable =
        std::filesystem::path(modulePath).parent_path() / L"HydraSeat.exe";
    HANDLE file = CreateFileW(productExecutable.c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "HydraSeat.exe is unavailable for exact build hashing";
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    const auto cleanup = [&]() noexcept {
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
    };

    LARGE_INTEGER fileSize{};
    constexpr std::uint64_t kMaximumProductExecutableBytes =
        512ull * 1024ull * 1024ull;
    if (GetFileSizeEx(file, &fileSize) == FALSE || fileSize.QuadPart <= 0 ||
        static_cast<std::uint64_t>(fileSize.QuadPart) >
            kMaximumProductExecutableBytes) {
        cleanup();
        error = "HydraSeat.exe size is invalid or exceeds the bounded hash input";
        return false;
    }
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0) {
        cleanup();
        error = "SHA-256 provider is unavailable for build identity";
        return false;
    }

    DWORD objectLength = 0u;
    DWORD hashLength = 0u;
    ULONG propertyBytes = 0u;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &propertyBytes, 0) < 0 ||
        propertyBytes != sizeof(objectLength) || objectLength == 0u ||
        objectLength > 1024u * 1024u ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLength),
                          sizeof(hashLength), &propertyBytes, 0) < 0 ||
        propertyBytes != sizeof(hashLength) || hashLength != 32u) {
        cleanup();
        error = "SHA-256 provider returned invalid hash metadata";
        return false;
    }

    std::vector<UCHAR> hashObject(objectLength);
    std::vector<UCHAR> digest(hashLength);
    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength,
                         nullptr, 0, 0) < 0) {
        cleanup();
        error = "SHA-256 hash state could not be created";
        return false;
    }

    std::vector<UCHAR> buffer(64u * 1024u);
    std::uint64_t totalRead = 0u;
    for (;;) {
        DWORD bytesRead = 0u;
        if (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                     &bytesRead, nullptr) == FALSE) {
            cleanup();
            error = "HydraSeat.exe could not be read completely for build identity";
            return false;
        }
        if (bytesRead == 0u) break;
        totalRead += bytesRead;
        if (totalRead > static_cast<std::uint64_t>(fileSize.QuadPart) ||
            BCryptHashData(hash, buffer.data(), bytesRead, 0) < 0) {
            cleanup();
            error = "HydraSeat.exe changed or hashing failed during build identity capture";
            return false;
        }
    }
    if (totalRead != static_cast<std::uint64_t>(fileSize.QuadPart) ||
        BCryptFinishHash(hash, digest.data(), hashLength, 0) < 0) {
        cleanup();
        error = "HydraSeat.exe build hash could not be finalized exactly";
        return false;
    }

    cleanup();
    constexpr char hex[] = "0123456789abcdef";
    output = "sha256-";
    output.reserve(7u + digest.size() * 2u);
    for (const auto byte : digest) {
        output.push_back(hex[(byte >> 4u) & 0x0fu]);
        output.push_back(hex[byte & 0x0fu]);
    }
    return true;
}

bool captureProductionContext(RequirementResolveContext& output,
                              std::string& error) {
    RequirementResolveContext candidate;
    if (!currentReferenceMonth(candidate.referenceMonth, error) ||
        !installedHydraSeatBuildIdentity(candidate.hydraSeatBuild, error) ||
        !currentWindowsBuildClass(candidate.windowsBuildClass, error)) {
        return false;
    }
    candidate.staleAfterMonths = 6u;
    candidate.hydraSeatVersion = std::string(kProductionHydraSeatVersion);
    candidate.architecture = currentArchitecture();
    candidate.trust = LocalEvidenceTrust::PhysicalOnly;
    if (candidate.architecture == "unknown") {
        error = "current process architecture is unsupported for requirement authority";
        return false;
    }
    output = std::move(candidate);
    return true;
}

class ProductionRequirementResolveInputSource final
    : public IRequirementResolveInputSource {
public:
    bool capture(RequirementResolveInputs& output, std::string& error) override {
        RequirementResolveInputs candidate;
        std::vector<catalog::GameCatalogCandidate> candidates;

        auto steam = std::make_shared<provider::steam::SteamProviderAdapter>(
            provider::steam::makeNativeSteamMetadataSource());
        const auto steamRefresh = steam->refresh();
        if (steamRefresh.succeeded()) {
            std::vector<catalog::GameCatalogCandidate> steamCandidates;
            const auto discovered = provider::discoverInstalledGames(
                *steam, steamCandidates);
            if (!discovered.succeeded()) {
                error = "Steam discovery failed after metadata refresh: " +
                        discovered.message;
                return false;
            }
            candidates.insert(candidates.end(), steamCandidates.begin(),
                              steamCandidates.end());
            candidate.providers.emplace_back("steam", steam.get());
            candidate.providerOwners.push_back(std::move(steam));
        }

        profile::GameRecordDocument manualGames;
        if (!loadManualGameRecords(manualGames, error)) return false;
        for (const auto& game : manualGames.games) {
            provider::custom::CustomExecutableDefinition definition;
            definition.title = game.title;
            definition.executablePath = game.executableCandidates.front();
            if (!game.installRoot.empty()) definition.workingDirectory = game.installRoot;
            auto adapter = std::make_shared<
                provider::custom::CustomExecutableProviderAdapter>(
                    provider::custom::makeNativeCustomExecutableSource(),
                    std::move(definition));
            const auto refreshed = adapter->refresh();
            if (!refreshed.succeeded()) {
                continue;
            }
            std::vector<catalog::GameCatalogCandidate> discovered;
            const auto discovery = provider::discoverInstalledGames(*adapter, discovered);
            if (!discovery.succeeded() || discovered.size() != 1u ||
                !discovered.front().providerAppId ||
                discovered.front().providerAppId != game.providerAppId) {
                error = "manual executable provider returned inconsistent current identity";
                return false;
            }
            candidates.push_back(discovered.front());
            candidate.providers.emplace_back(
                "custom", adapter.get(), *discovered.front().providerAppId);
            candidate.providerOwners.push_back(std::move(adapter));
        }

        const auto catalogDiagnostic = catalog::buildLocalGameCatalog(
            candidates, candidate.catalog);
        if (!catalogDiagnostic.succeeded()) {
            error = "current local game catalog failed validation: " +
                    catalogDiagnostic.message;
            return false;
        }

        std::string compatibilityPathError;
        const auto compatibilityPath =
            community::defaultCompatibilityLocalStorePath(&compatibilityPathError);
        if (!compatibilityPath) {
            error = compatibilityPathError.empty()
                ? "fixed local compatibility evidence path is unavailable"
                : std::move(compatibilityPathError);
            return false;
        }
        community::CompatibilityShareModel localHistory;
        const community::CompatibilityLocalStore compatibilityStore(
            *compatibilityPath);
        const auto compatibilityLoad = compatibilityStore.load(localHistory);
        if (!compatibilityLoad.succeeded()) {
            error = "fixed local compatibility evidence store failed validation: " +
                    compatibilityLoad.message;
            return false;
        }
        if (compatibilityLoad.found()) {
            candidate.localEvidence.reserve(localHistory.history().size());
            for (const auto& entry : localHistory.history()) {
                candidate.localEvidence.push_back(entry.result);
            }
        }

        // v1 currently has no persisted protected-runtime approval authority.
        // Empty is therefore the only safe production projection.
        candidate.protectedApprovals.clear();
        if (!captureProductionContext(candidate.context, error)) return false;

        output = std::move(candidate);
        error.clear();
        return true;
    }
};

#endif // _WIN32

} // namespace

RequirementStoreDiagnostic validateRequirementEvidenceDocument(
    const RequirementEvidenceDocument& document) {
    if (document.schemaVersion != kRequirementEvidenceStoreSchemaVersion) {
        return storeFailure(RequirementStoreCode::UnsupportedSchema,
                            "unsupported runtime requirement evidence schema version");
    }
    if (document.records.size() > kMaximumRequirementEvidenceRecords) {
        return storeFailure(RequirementStoreCode::InvalidRecord,
                            "runtime requirement evidence record count exceeds the bounded maximum");
    }

    std::set<std::string> recordIds;
    std::set<std::string> gameIds;
    for (const auto& record : document.records) {
        if (!validProfileIdentifier(record.recordId) || record.revision == 0u ||
            !validProfileIdentifier(record.gameId) ||
            !validProfileIdentifier(record.providerId) ||
            (record.providerAppId && !validProfileIdentifier(*record.providerAppId)) ||
            (record.gameVersionUtf8 && !validCompatibilityVersion(*record.gameVersionUtf8)) ||
            (record.executableSha256 && !validSha256(*record.executableSha256)) ||
            (record.catalogCompatibility && !validCompatibility(*record.catalogCompatibility)) ||
            record.providerMetadataRevision == 0u ||
            !validEvidenceIdentifier(record.evidenceResultId) ||
            !validEvidenceIdentifier(record.evidenceProvenanceId) ||
            record.evidenceProvenanceRevision == 0u ||
            record.validatedSeatCount < 1u || record.validatedSeatCount > 2u) {
            return storeFailure(RequirementStoreCode::InvalidRecord,
                                "runtime requirement evidence contains an invalid or unbounded record");
        }
        if (!recordIds.insert(record.recordId).second) {
            return storeFailure(RequirementStoreCode::DuplicateRecord,
                                "duplicate runtime requirement evidence record_id");
        }
        if (!gameIds.insert(record.gameId).second) {
            return storeFailure(RequirementStoreCode::DuplicateGameAuthority,
                                "multiple stored requirement records claim authority for one Game");
        }
    }
    return {};
}

RequirementStoreDiagnostic encodeRequirementEvidenceDocumentJson(
    const RequirementEvidenceDocument& document,
    std::string& output) {
    const auto validation = validateRequirementEvidenceDocument(document);
    if (!validation.succeeded()) return validation;
    try {
        std::ostringstream encoded;
        encoded << "{\"schema_version\":" << document.schemaVersion << ",\"records\":[";
        for (std::size_t index = 0u; index < document.records.size(); ++index) {
            if (index != 0u) encoded << ',';
            const auto& record = document.records[index];
            encoded << '{'
                    << "\"record_id\":" << quote(record.recordId) << ','
                    << "\"revision\":" << record.revision << ','
                    << "\"game_id\":" << quote(record.gameId) << ','
                    << "\"provider_id\":" << quote(record.providerId) << ','
                    << "\"provider_app_id\":" << optionalStringJson(record.providerAppId) << ','
                    << "\"game_version\":" << optionalStringJson(record.gameVersionUtf8) << ','
                    << "\"executable_sha256\":" << optionalStringJson(record.executableSha256) << ','
                    << "\"catalog_compatibility\":" << compatibilityJson(record.catalogCompatibility) << ','
                    << "\"provider_metadata_revision\":" << record.providerMetadataRevision << ','
                    << "\"evidence_result_id\":" << quote(record.evidenceResultId) << ','
                    << "\"evidence_provenance_id\":" << quote(record.evidenceProvenanceId) << ','
                    << "\"evidence_provenance_revision\":" << record.evidenceProvenanceRevision << ','
                    << "\"validated_seat_count\":" << static_cast<unsigned int>(record.validatedSeatCount) << ','
                    << "\"requirements\":" << requirementsJson(record.requirements) << ','
                    << "\"capabilities\":" << capabilitiesJson(record.capabilities)
                    << '}';
        }
        encoded << "]}";
        auto candidate = encoded.str();
        if (candidate.size() > kMaximumRequirementEvidenceStoreBytes) {
            return storeFailure(RequirementStoreCode::TooLarge,
                                "encoded runtime requirement evidence exceeds the bounded store size");
        }
        output = std::move(candidate);
        return {};
    } catch (...) {
        return storeFailure(RequirementStoreCode::WriteFailed,
                            "failed to encode runtime requirement evidence");
    }
}

RequirementStoreDiagnostic decodeRequirementEvidenceDocumentJson(
    std::string_view json,
    RequirementEvidenceDocument& output) {
    if (json.empty() || json.size() > kMaximumRequirementEvidenceStoreBytes) {
        return storeFailure(json.empty() ? RequirementStoreCode::ParseError
                                         : RequirementStoreCode::TooLarge,
                            json.empty() ? "runtime requirement evidence is empty"
                                         : "runtime requirement evidence exceeds the bounded store size");
    }
    try {
        const auto root = internal::json::parse(json, {32u, 131072u});
        const auto& object = asObject(root, "runtime requirement evidence document");
        requireFields(object, {"schema_version", "records"});
        RequirementEvidenceDocument candidate;
        const auto encodedSchemaVersion =
            asU32(required(object, "schema_version"), "schema_version");
        if (encodedSchemaVersion != kLegacyRequirementEvidenceStoreSchemaVersion &&
            encodedSchemaVersion != kRequirementEvidenceStoreSchemaVersion) {
            return storeFailure(RequirementStoreCode::UnsupportedSchema,
                                "unsupported runtime requirement evidence schema version");
        }
        candidate.schemaVersion = kRequirementEvidenceStoreSchemaVersion;
        const auto& records = asArray(required(object, "records"), "records");
        if (records.size() > kMaximumRequirementEvidenceRecords) {
            return storeFailure(RequirementStoreCode::InvalidRecord,
                                "runtime requirement evidence record count exceeds the bounded maximum");
        }
        candidate.records.reserve(records.size());
        for (const auto& value : records) {
            candidate.records.push_back(parseRecord(value, encodedSchemaVersion));
        }
        const auto validation = validateRequirementEvidenceDocument(candidate);
        if (!validation.succeeded()) return validation;
        output = std::move(candidate);
        return {};
    } catch (const StoreError& error) {
        return storeFailure(error.code(), error.what());
    } catch (const internal::json::ParseError& error) {
        return storeFailure(RequirementStoreCode::ParseError, error.what());
    } catch (...) {
        return storeFailure(RequirementStoreCode::ParseError,
                            "runtime requirement evidence parsing failed unexpectedly");
    }
}

std::filesystem::path GameRuntimeRequirementStore::temporaryPath() const {
    auto temporary = path_;
    temporary += L".tmp";
    return temporary;
}

RequirementStoreDiagnostic GameRuntimeRequirementStore::load(
    RequirementEvidenceDocument& output) const {
    std::error_code error;
    const bool exists = std::filesystem::exists(path_, error);
    if (error) return storeFailure(RequirementStoreCode::ReadFailed,
                                   "failed to inspect runtime requirement evidence store");
    if (!exists) return {RequirementStoreCode::Missing, "runtime requirement evidence store is missing"};
    const auto size = std::filesystem::file_size(path_, error);
    if (error) return storeFailure(RequirementStoreCode::ReadFailed,
                                   "failed to size runtime requirement evidence store");
    if (size == 0u || size > kMaximumRequirementEvidenceStoreBytes) {
        return storeFailure(size == 0u ? RequirementStoreCode::ParseError
                                       : RequirementStoreCode::TooLarge,
                            "runtime requirement evidence store has an invalid size");
    }
    std::ifstream input(path_, std::ios::binary);
    if (!input) return storeFailure(RequirementStoreCode::ReadFailed,
                                    "failed to open runtime requirement evidence store");
    std::string json(static_cast<std::size_t>(size), '\0');
    input.read(json.data(), static_cast<std::streamsize>(json.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(json.size())) {
        return storeFailure(RequirementStoreCode::ReadFailed,
                            "failed to read complete runtime requirement evidence store");
    }
    char trailing = '\0';
    if (input.get(trailing)) {
        return storeFailure(RequirementStoreCode::ReadFailed,
                            "runtime requirement evidence store changed while being read");
    }
    return decodeRequirementEvidenceDocumentJson(json, output);
}

RequirementStoreDiagnostic GameRuntimeRequirementStore::save(
    const RequirementEvidenceDocument& document) const {
    std::string json;
    const auto encoded = encodeRequirementEvidenceDocumentJson(document, json);
    if (!encoded.succeeded()) return encoded;
    if (path_.empty()) return storeFailure(RequirementStoreCode::WriteFailed,
                                           "runtime requirement evidence store path is empty");

    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return storeFailure(RequirementStoreCode::WriteFailed,
                                       "failed to create runtime requirement evidence directory");
    }
    const auto staging = temporaryPath();
    std::filesystem::remove(staging, error);
    error.clear();
    {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        if (!output) return storeFailure(RequirementStoreCode::WriteFailed,
                                         "failed to create runtime requirement evidence staging file");
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(staging, error);
            return storeFailure(RequirementStoreCode::WriteFailed,
                                "failed to flush runtime requirement evidence staging file");
        }
    }
#ifdef _WIN32
    if (MoveFileExW(staging.c_str(), path_.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        std::filesystem::remove(staging, error);
        return storeFailure(RequirementStoreCode::WriteFailed,
                            "failed to atomically publish runtime requirement evidence store");
    }
#else
    std::filesystem::rename(staging, path_, error);
    if (error) {
        std::filesystem::remove(staging, error);
        return storeFailure(RequirementStoreCode::WriteFailed,
                            "failed to atomically publish runtime requirement evidence store");
    }
#endif
    return {};
}

RequirementStoreDiagnostic GameRuntimeRequirementStore::remove() const {
    std::error_code error;
    const bool exists = std::filesystem::exists(path_, error);
    if (error) return storeFailure(RequirementStoreCode::RemoveFailed,
                                   "failed to inspect runtime requirement evidence store");
    if (!exists) return {RequirementStoreCode::Missing, "runtime requirement evidence store is missing"};
    if (!std::filesystem::remove(path_, error) || error) {
        return storeFailure(RequirementStoreCode::RemoveFailed,
                            "failed to remove runtime requirement evidence store");
    }
    return {};
}

std::optional<std::filesystem::path> defaultGameRuntimeRequirementStorePath(
    std::string* error) {
#ifdef _WIN32
    wchar_t localAppData[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length == 0u || length >= static_cast<DWORD>(std::size(localAppData))) {
        if (error != nullptr) *error = "LOCALAPPDATA is unavailable";
        return std::nullopt;
    }
    return std::filesystem::path(localAppData) / L"HydraSeat" /
           L"runtime-requirements.json";
#else
    if (error != nullptr) *error = "default runtime requirement store path is Windows-only";
    return std::nullopt;
#endif
}

RequirementSnapshotDiagnostic resolveTrustedGameRuntimeRequirements(
    const RequirementEvidenceDocument& stored,
    const catalog::LocalGameCatalog& catalog,
    std::span<const plan::ProviderAdapterBinding> providers,
    std::span<const compat::CompatibilityResult> localEvidence,
    std::span<const ProtectedRuntimeApproval> protectedApprovals,
    const RequirementResolveContext& context,
    TrustedRequirementSnapshot& output) {
    const auto storeValidation = validateRequirementEvidenceDocument(stored);
    if (!storeValidation.succeeded()) {
        return snapshotFailure(RequirementSnapshotCode::InvalidStore, storeValidation.message);
    }
    std::int64_t referenceMonth = 0;
    if (!parseMonth(context.referenceMonth, referenceMonth) || context.staleAfterMonths > 120u ||
        !validCompatibilityVersion(context.hydraSeatVersion) ||
        !validCompatibilityVersion(context.hydraSeatBuild) ||
        !validCompatibilityVersion(context.windowsBuildClass) ||
        !validEvidenceIdentifier(context.architecture)) {
        return snapshotFailure(RequirementSnapshotCode::InvalidContext,
                               "runtime requirement resolve context is invalid or unbounded");
    }
    if (catalog.entries.size() > profile::kMaximumGames ||
        providers.size() > profile::kMaximumGames ||
        localEvidence.size() > 65536u || protectedApprovals.size() > profile::kMaximumGames) {
        return snapshotFailure(RequirementSnapshotCode::TooManyInputs,
                               "runtime requirement resolver input exceeds a bounded maximum");
    }

    profile::GameRecordDocument gameDocument;
    gameDocument.games.reserve(catalog.entries.size());
    for (const auto& entry : catalog.entries) gameDocument.games.push_back(entry.game);
    const auto gameValidation = profile::validateGameRecordDocument(gameDocument);
    if (!gameValidation.succeeded()) {
        return snapshotFailure(RequirementSnapshotCode::InvalidCatalog,
                               "local Game catalog is invalid: " + gameValidation.message);
    }

    std::map<std::string, const compat::CompatibilityResult*> evidenceById;
    std::set<std::string> duplicateEvidenceIds;
    for (const auto& result : localEvidence) {
        if (result.resultId.empty()) continue;
        const auto [iterator, inserted] = evidenceById.emplace(result.resultId, &result);
        if (!inserted) duplicateEvidenceIds.insert(iterator->first);
    }
    if (!duplicateEvidenceIds.empty()) {
        return snapshotFailure(RequirementSnapshotCode::DuplicateLocalEvidenceId,
                               "local compatibility evidence contains duplicate result_id authority");
    }

    std::set<std::string> approvalKeys;
    for (const auto& approval : protectedApprovals) {
        if (!approvalStructurallyValid(approval)) {
            return snapshotFailure(RequirementSnapshotCode::InvalidApproval,
                                   "protected runtime approval is invalid or unbounded");
        }
        std::ostringstream key;
        key << approval.gameId << '\n' << approval.providerId << '\n'
            << (approval.providerAppId ? *approval.providerAppId : std::string{}) << '\n'
            << approval.providerMetadataRevision << '\n' << approval.requirementRecordId << '\n'
            << approval.requirementRevision << '\n' << approval.evidenceResultId << '\n'
            << approval.evidenceProvenanceRevision;
        if (!approvalKeys.insert(key.str()).second) {
            return snapshotFailure(RequirementSnapshotCode::DuplicateApproval,
                                   "duplicate protected runtime approval is ambiguous");
        }
    }

    try {
        TrustedRequirementSnapshot candidate;
        candidate.referenceMonth = context.referenceMonth;
        candidate.staleAfterMonths = context.staleAfterMonths;
        candidate.trust = context.trust;
        candidate.authorities.reserve(catalog.entries.size());
        candidate.requirements.reserve(catalog.entries.size());
        candidate.blockedGames.reserve(catalog.entries.size());

        for (const auto& entry : catalog.entries) {
            const auto& game = entry.game;
            const auto* record = findStoredRecord(stored, game.gameId);
            if (record == nullptr) {
                addIssue(candidate, RequirementResolveCode::MissingStoredEvidence, game.gameId,
                         "no trusted local runtime requirement evidence is stored for this Game");
                continue;
            }
            if (entry.staleness != catalog::CatalogStaleness::Current ||
                entry.mergedCandidateCount == 0u) {
                addIssue(candidate, RequirementResolveCode::StaleCatalogGame, game.gameId,
                         "local Game catalog evidence is stale or has no reconciled candidate");
                continue;
            }
            if (!gameIdentityMatches(*record, game)) {
                addIssue(candidate, RequirementResolveCode::GameIdentityMismatch, game.gameId,
                         "stored requirement evidence does not match the exact local Game identity");
                continue;
            }

            bool duplicateProvider = false;
            auto* adapter = findProvider(providers, game.providerId, game.providerAppId,
                                         duplicateProvider);
            if (duplicateProvider) {
                addIssue(candidate, RequirementResolveCode::DuplicateProvider, game.gameId,
                         "multiple current provider bindings match the Game");
                continue;
            }
            if (adapter == nullptr) {
                addIssue(candidate, RequirementResolveCode::MissingProvider, game.gameId,
                         "the exact current provider binding is missing");
                continue;
            }
            const auto descriptor = adapter->descriptor();
            if (descriptor.providerId != game.providerId ||
                descriptor.availability != provider::ProviderAvailability::Available ||
                descriptor.metadataRevision == 0u) {
                addIssue(candidate, RequirementResolveCode::ProviderUnavailable, game.gameId,
                         "provider snapshot is unavailable, mismatched, or has no revision");
                continue;
            }
            if (descriptor.metadataRevision != record->providerMetadataRevision) {
                addIssue(candidate, RequirementResolveCode::StaleProviderRevision, game.gameId,
                         "stored requirement evidence targets a different provider metadata revision");
                continue;
            }

            const auto evidenceIterator = evidenceById.find(record->evidenceResultId);
            if (evidenceIterator == evidenceById.end()) {
                addIssue(candidate, RequirementResolveCode::MissingLocalEvidence, game.gameId,
                         "the exact local compatibility evidence result is missing");
                continue;
            }
            compat::CompatibilityResult evidence = *evidenceIterator->second;
            const auto evidenceValidation = compat::canonicalizeCompatibilityResult(evidence);
            if (!evidenceValidation.succeeded()) {
                addIssue(candidate, RequirementResolveCode::InvalidLocalEvidence, game.gameId,
                         "local compatibility evidence is invalid: " + evidenceValidation.message);
                continue;
            }
            if (evidence.origin == compat::ResultOrigin::ImportedCommunity) {
                addIssue(candidate, RequirementResolveCode::CommunityEvidenceRejected, game.gameId,
                         "imported community results cannot authorize runtime requirements");
                continue;
            }
            if (!originAllowed(context.trust, evidence.origin)) {
                addIssue(candidate, RequirementResolveCode::UntrustedLocalEvidenceOrigin, game.gameId,
                         "local evidence origin is below the configured runtime trust level");
                continue;
            }
            if (!resultIdentityMatches(*record, context, evidence)) {
                addIssue(candidate, RequirementResolveCode::EvidenceEnvironmentMismatch, game.gameId,
                         "local evidence identity/environment does not exactly match the runtime context");
                continue;
            }
            if (record->catalogCompatibility) {
                if (record->evidenceProvenanceRevision >
                        (std::numeric_limits<std::uint32_t>::max)() ||
                    record->catalogCompatibility->recordId != evidence.resultId ||
                    record->catalogCompatibility->provenance != evidence.provenanceId ||
                    record->catalogCompatibility->evidenceRevision !=
                        static_cast<std::uint32_t>(record->evidenceProvenanceRevision)) {
                    addIssue(candidate, RequirementResolveCode::GameIdentityMismatch, game.gameId,
                             "catalog compatibility reference does not identify the exact local evidence");
                    continue;
                }
            }

            std::int64_t observedMonth = 0;
            if (!resultMonth(evidence, observedMonth)) {
                addIssue(candidate, RequirementResolveCode::InvalidLocalEvidence, game.gameId,
                         "local evidence timestamp cannot be classified by month");
                continue;
            }
            if (observedMonth > referenceMonth) {
                addIssue(candidate, RequirementResolveCode::FutureLocalEvidence, game.gameId,
                         "future-dated local evidence cannot authorize runtime requirements");
                continue;
            }
            const auto age = static_cast<std::uint64_t>(referenceMonth - observedMonth);
            if (age > context.staleAfterMonths) {
                addIssue(candidate, RequirementResolveCode::StaleLocalEvidence, game.gameId,
                         "local evidence is older than the configured freshness horizon");
                continue;
            }
            if (!capabilitiesCover(*record) || !resultCoversRequirements(*record, evidence)) {
                addIssue(candidate, RequirementResolveCode::InsufficientCapabilityEvidence, game.gameId,
                         "local capability/result evidence does not prove every required runtime path");
                continue;
            }

            bool protectedApproved = false;
            if (record->requirements.highRisk) {
                std::size_t matches = 0u;
                for (const auto& approval : protectedApprovals) {
                    if (approvalMatches(approval, *record)) ++matches;
                }
                if (matches > 1u) {
                    addIssue(candidate, RequirementResolveCode::DuplicateProtectedApproval, game.gameId,
                             "multiple approvals match the exact protected runtime authority");
                    continue;
                }
                if (matches == 0u) {
                    addIssue(candidate, RequirementResolveCode::ProtectedApprovalRequired, game.gameId,
                             "Protected / Experimental runtime requires exact current user approval");
                    continue;
                }
                protectedApproved = true;
            }

            plan::GameRuntimeRequirement requirement;
            requirement.gameId = game.gameId;
            requirement.revision = record->revision;
            requirement.validatedSeatCount = record->validatedSeatCount;
            requirement.requirements = record->requirements;
            requirement.capabilities = record->capabilities;
            requirement.highRiskApproved = protectedApproved;
            requirement.compatibility = game.compatibility;

            TrustedGameRuntimeAuthority authority;
            authority.requirement = requirement;
            authority.providerId = record->providerId;
            authority.providerAppId = record->providerAppId;
            authority.providerMetadataRevision = record->providerMetadataRevision;
            authority.gameVersionUtf8 = record->gameVersionUtf8;
            authority.executableSha256 = record->executableSha256;
            authority.executableCandidates = game.executableCandidates;
            authority.evidenceResultId = evidence.resultId;
            authority.evidenceProvenanceId = evidence.provenanceId;
            authority.evidenceProvenanceRevision = evidence.provenanceRevision;
            authority.evidenceTimestampBucket = evidence.timestampBucket;
            authority.evidenceOrigin = evidence.origin;
            candidate.authorities.push_back(std::move(authority));
            candidate.requirements.push_back(std::move(requirement));
        }

        std::sort(candidate.authorities.begin(), candidate.authorities.end(),
                  [](const auto& left, const auto& right) {
                      return left.requirement.gameId < right.requirement.gameId;
                  });
        std::sort(candidate.requirements.begin(), candidate.requirements.end(),
                  [](const auto& left, const auto& right) { return left.gameId < right.gameId; });
        std::sort(candidate.blockedGames.begin(), candidate.blockedGames.end(),
                  [](const auto& left, const auto& right) {
                      if (left.gameId != right.gameId) return left.gameId < right.gameId;
                      return static_cast<std::uint8_t>(left.code) <
                             static_cast<std::uint8_t>(right.code);
                  });
        output = std::move(candidate);
        return {};
    } catch (...) {
        return snapshotFailure(RequirementSnapshotCode::InternalFailure,
                               "runtime requirement resolution failed unexpectedly");
    }
}

RequirementSnapshotDiagnostic StoreBackedTrustedRequirementSource::resolveCurrent(
    TrustedRequirementSnapshot& output) {
    output = {};
    if (!inputSource_) {
        return snapshotFailure(RequirementSnapshotCode::InputUnavailable,
                               "runtime requirement input source is unavailable");
    }

    RequirementEvidenceDocument stored;
    const auto loaded = store_.load(stored);
    if (!loaded.succeeded() || !loaded.found()) {
        return snapshotFailure(
            RequirementSnapshotCode::InvalidStore,
            loaded.message.empty()
                ? "trusted runtime requirement evidence store is unavailable"
                : loaded.message);
    }

    RequirementResolveInputs inputs;
    std::string error;
    if (!inputSource_->capture(inputs, error)) {
        return snapshotFailure(
            RequirementSnapshotCode::InputUnavailable,
            error.empty() ? "current provider/catalog/evidence snapshot is unavailable"
                          : std::move(error));
    }
    return resolveTrustedGameRuntimeRequirements(
        stored, inputs.catalog, inputs.providers, inputs.localEvidence,
        inputs.protectedApprovals, inputs.context, output);
}

std::shared_ptr<IRequirementResolveInputSource>
makeProductionRequirementResolveInputSource() {
#ifdef _WIN32
    return std::make_shared<ProductionRequirementResolveInputSource>();
#else
    return {};
#endif
}

std::shared_ptr<ITrustedRequirementSource>
makeDefaultProductionTrustedRequirementSource() {
    std::string pathError;
    const auto path = defaultGameRuntimeRequirementStorePath(&pathError);
    if (!path) {
        return std::make_shared<UnavailableTrustedRequirementSource>(
            pathError.empty()
                ? "fixed production runtime requirement store path is unavailable"
                : std::move(pathError));
    }
    auto inputSource = makeProductionRequirementResolveInputSource();
    if (!inputSource) {
        return std::make_shared<UnavailableTrustedRequirementSource>(
            "native production requirement input source is unavailable on this platform");
    }

    std::string materializationPathError;
    const auto materializationPath =
        materialization::defaultLocalMaterializationDecisionStorePath(
            &materializationPathError);
    std::string instancesRootError;
    const auto instancesRoot =
        materialization::defaultInstanceMaterializationRoot(&instancesRootError);
    if (!materializationPath || !instancesRoot) {
        std::string diagnostic =
            "fixed production materialization authority path is unavailable";
        if (!materializationPathError.empty()) {
            diagnostic += ": " + materializationPathError;
        } else if (!instancesRootError.empty()) {
            diagnostic += ": " + instancesRootError;
        }
        return std::make_shared<UnavailableTrustedRequirementSource>(
            std::move(diagnostic));
    }
    auto materializationSource = std::make_shared<
        materialization::StoreBackedTrustedMaterializationDecisionSource>(
        *materializationPath);
    return std::make_shared<StoreBackedTrustedRequirementSource>(
        *path, std::move(inputSource), std::move(materializationSource),
        *instancesRoot);
}

RequirementSnapshotDiagnostic resolveCurrentRequirementProjection(
    ITrustedRequirementSource& source,
    std::vector<plan::GameRuntimeRequirement>& output) {
    output.clear();
    TrustedRequirementSnapshot snapshot;
    const auto diagnostic = source.resolveCurrent(snapshot);
    if (!diagnostic.succeeded()) return diagnostic;
    output = std::move(snapshot.requirements);
    return diagnostic;
}

std::string_view requirementStoreCodeName(RequirementStoreCode code) noexcept {
    switch (code) {
    case RequirementStoreCode::Success: return "Success";
    case RequirementStoreCode::Missing: return "Missing";
    case RequirementStoreCode::TooLarge: return "TooLarge";
    case RequirementStoreCode::ParseError: return "ParseError";
    case RequirementStoreCode::UnsupportedSchema: return "UnsupportedSchema";
    case RequirementStoreCode::UnknownField: return "UnknownField";
    case RequirementStoreCode::InvalidRecord: return "InvalidRecord";
    case RequirementStoreCode::DuplicateRecord: return "DuplicateRecord";
    case RequirementStoreCode::DuplicateGameAuthority: return "DuplicateGameAuthority";
    case RequirementStoreCode::ReadFailed: return "ReadFailed";
    case RequirementStoreCode::WriteFailed: return "WriteFailed";
    case RequirementStoreCode::RemoveFailed: return "RemoveFailed";
    }
    return "Unknown";
}

std::string_view requirementResolveCodeName(RequirementResolveCode code) noexcept {
    switch (code) {
    case RequirementResolveCode::MissingStoredEvidence: return "MissingStoredEvidence";
    case RequirementResolveCode::StaleCatalogGame: return "StaleCatalogGame";
    case RequirementResolveCode::GameIdentityMismatch: return "GameIdentityMismatch";
    case RequirementResolveCode::MissingProvider: return "MissingProvider";
    case RequirementResolveCode::DuplicateProvider: return "DuplicateProvider";
    case RequirementResolveCode::ProviderUnavailable: return "ProviderUnavailable";
    case RequirementResolveCode::StaleProviderRevision: return "StaleProviderRevision";
    case RequirementResolveCode::MissingLocalEvidence: return "MissingLocalEvidence";
    case RequirementResolveCode::InvalidLocalEvidence: return "InvalidLocalEvidence";
    case RequirementResolveCode::CommunityEvidenceRejected: return "CommunityEvidenceRejected";
    case RequirementResolveCode::UntrustedLocalEvidenceOrigin: return "UntrustedLocalEvidenceOrigin";
    case RequirementResolveCode::EvidenceEnvironmentMismatch: return "EvidenceEnvironmentMismatch";
    case RequirementResolveCode::StaleLocalEvidence: return "StaleLocalEvidence";
    case RequirementResolveCode::FutureLocalEvidence: return "FutureLocalEvidence";
    case RequirementResolveCode::InsufficientCapabilityEvidence: return "InsufficientCapabilityEvidence";
    case RequirementResolveCode::ProtectedApprovalRequired: return "ProtectedApprovalRequired";
    case RequirementResolveCode::DuplicateProtectedApproval: return "DuplicateProtectedApproval";
    }
    return "Unknown";
}

std::string_view requirementSnapshotCodeName(RequirementSnapshotCode code) noexcept {
    switch (code) {
    case RequirementSnapshotCode::Success: return "Success";
    case RequirementSnapshotCode::InvalidContext: return "InvalidContext";
    case RequirementSnapshotCode::InvalidStore: return "InvalidStore";
    case RequirementSnapshotCode::InvalidCatalog: return "InvalidCatalog";
    case RequirementSnapshotCode::TooManyInputs: return "TooManyInputs";
    case RequirementSnapshotCode::DuplicateLocalEvidenceId: return "DuplicateLocalEvidenceId";
    case RequirementSnapshotCode::InvalidApproval: return "InvalidApproval";
    case RequirementSnapshotCode::DuplicateApproval: return "DuplicateApproval";
    case RequirementSnapshotCode::InputUnavailable: return "InputUnavailable";
    case RequirementSnapshotCode::InternalFailure: return "InternalFailure";
    }
    return "Unknown";
}

std::string_view trustedPlanRequirementCodeName(TrustedPlanRequirementCode code) noexcept {
    switch (code) {
    case TrustedPlanRequirementCode::Success: return "Success";
    case TrustedPlanRequirementCode::InvalidSnapshot: return "InvalidSnapshot";
    case TrustedPlanRequirementCode::MissingAuthority: return "MissingAuthority";
    case TrustedPlanRequirementCode::DuplicateAuthority: return "DuplicateAuthority";
    case TrustedPlanRequirementCode::GameIdentityMismatch: return "GameIdentityMismatch";
    case TrustedPlanRequirementCode::ProviderIdentityMismatch: return "ProviderIdentityMismatch";
    case TrustedPlanRequirementCode::ProviderRevisionMismatch: return "ProviderRevisionMismatch";
    case TrustedPlanRequirementCode::RequirementMismatch: return "RequirementMismatch";
    case TrustedPlanRequirementCode::UntrustedEvidenceOrigin: return "UntrustedEvidenceOrigin";
    case TrustedPlanRequirementCode::StaleEvidence: return "StaleEvidence";
    case TrustedPlanRequirementCode::ProtectedApprovalRequired: return "ProtectedApprovalRequired";
    }
    return "Unknown";
}

} // namespace hydra::requirement
