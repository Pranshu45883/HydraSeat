#include "hydra/compatibility_local_store.hpp"

#include "hydra/internal/strict_json.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace hydra::community {
namespace {

CompatibilityLocalStoreDiagnostic fail(CompatibilityLocalStoreCode code,
                                       std::string message) {
    return {code, std::move(message)};
}

bool removeIfPresentVerified(const std::filesystem::path& path,
                             bool* existed = nullptr) noexcept {
    std::error_code error;
    const bool present = std::filesystem::exists(path, error);
    if (error) return false;
    if (existed != nullptr) *existed = present;
    if (!present) return true;
    if (!std::filesystem::remove(path, error) || error) return false;
    error.clear();
    const bool remains = std::filesystem::exists(path, error);
    return !error && !remains;
}

CompatibilityLocalStoreDiagnostic failAfterStaging(
    CompatibilityLocalStoreCode primaryCode, std::string message,
    const std::filesystem::path& temporary) {
    if (!removeIfPresentVerified(temporary)) {
        return fail(CompatibilityLocalStoreCode::CleanupFailed,
                    std::move(message) + "; staging cleanup could not be verified");
    }
    return fail(primaryCode, std::move(message));
}

bool flushStagedFile(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    return flushed;
#else
    (void)path;
    return true;
#endif
}

bool publishStagedFile(const std::filesystem::path& temporary,
                       const std::filesystem::path& current) noexcept {
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), current.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    std::error_code error;
    std::filesystem::rename(temporary, current, error);
    return !error;
#endif
}

} // namespace

std::filesystem::path CompatibilityLocalStore::temporaryPath() const {
    auto value = path_;
    value += L".tmp";
    return value;
}

CompatibilityLocalStoreDiagnostic CompatibilityLocalStore::load(
    CompatibilityShareModel& model) const {
    if (path_.empty()) {
        return fail(CompatibilityLocalStoreCode::ReadFailed,
                    "local compatibility store path is empty");
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(path_, error);
    if (error) {
        return fail(CompatibilityLocalStoreCode::ReadFailed,
                    "local compatibility store existence check failed");
    }
    if (!exists) {
        return {CompatibilityLocalStoreCode::Missing,
                "local compatibility store does not exist"};
    }

    const auto byteCount = std::filesystem::file_size(path_, error);
    if (error) {
        return fail(CompatibilityLocalStoreCode::ReadFailed,
                    "local compatibility store size could not be read");
    }
    if (byteCount == 0u) {
        return fail(CompatibilityLocalStoreCode::InvalidHistory,
                    "local compatibility store is empty");
    }
    if (byteCount > kMaximumCompatibilityLocalHistoryBytes) {
        return fail(CompatibilityLocalStoreCode::TooLarge,
                    "local compatibility store exceeds its bounded size");
    }

    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        return fail(CompatibilityLocalStoreCode::ReadFailed,
                    "local compatibility store could not be opened");
    }
    std::string bytes(static_cast<std::size_t>(byteCount), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return fail(CompatibilityLocalStoreCode::ReadFailed,
                    "local compatibility store could not be read completely");
    }
    char unexpected = '\0';
    if (input.get(unexpected)) {
        return fail(CompatibilityLocalStoreCode::ReadFailed,
                    "local compatibility store changed while it was being read");
    }

    const auto loaded = model.loadLocalHistoryJsonl(bytes);
    if (!loaded.succeeded()) {
        const auto code = loaded.code == ShareModelCode::LocalHistoryTooLarge
                              ? CompatibilityLocalStoreCode::TooLarge
                              : CompatibilityLocalStoreCode::InvalidHistory;
        return fail(code, "local compatibility store failed validation: " + loaded.message);
    }
    return {};
}

CompatibilityLocalStoreDiagnostic CompatibilityLocalStore::save(
    const CompatibilityShareModel& model) const {
    if (path_.empty()) {
        return fail(CompatibilityLocalStoreCode::WriteFailed,
                    "local compatibility store path is empty");
    }

    std::string bytes;
    const auto encoded = model.exportLocalHistoryJsonl(bytes);
    if (!encoded.succeeded()) {
        const auto code = encoded.code == ShareModelCode::LocalHistoryTooLarge
                              ? CompatibilityLocalStoreCode::TooLarge
                              : CompatibilityLocalStoreCode::InvalidHistory;
        return fail(code, "local compatibility history could not be encoded: " + encoded.message);
    }
    if (bytes.empty() || bytes.size() > kMaximumCompatibilityLocalHistoryBytes) {
        return fail(CompatibilityLocalStoreCode::TooLarge,
                    "encoded local compatibility store is outside its bounded size");
    }

    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return fail(CompatibilityLocalStoreCode::WriteFailed,
                        "local compatibility store directory could not be created");
        }
    }

    const auto temporary = temporaryPath();
    if (!removeIfPresentVerified(temporary)) {
        return fail(CompatibilityLocalStoreCode::CleanupFailed,
                    "stale local compatibility staging file could not be removed safely");
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return failAfterStaging(CompatibilityLocalStoreCode::WriteFailed,
                                    "local compatibility store staging file could not be opened",
                                    temporary);
        }
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            output.close();
            return failAfterStaging(CompatibilityLocalStoreCode::WriteFailed,
                                    "local compatibility store staging write failed", temporary);
        }
    }

    if (!flushStagedFile(temporary)) {
        return failAfterStaging(CompatibilityLocalStoreCode::WriteFailed,
                                "local compatibility store staging flush failed", temporary);
    }
    if (!publishStagedFile(temporary, path_)) {
        return failAfterStaging(CompatibilityLocalStoreCode::WriteFailed,
                                "local compatibility store atomic publication failed", temporary);
    }
    if (!removeIfPresentVerified(temporary)) {
        return fail(CompatibilityLocalStoreCode::CleanupFailed,
                    "published local compatibility staging file cleanup could not be verified");
    }
    return {};
}

CompatibilityLocalStoreDiagnostic CompatibilityLocalStore::remove() const {
    if (path_.empty()) {
        return fail(CompatibilityLocalStoreCode::RemoveFailed,
                    "local compatibility store path is empty");
    }

    bool dataExisted = false;
    if (!removeIfPresentVerified(path_, &dataExisted)) {
        return fail(CompatibilityLocalStoreCode::RemoveFailed,
                    "local compatibility store could not be removed and verified");
    }

    bool stagingExisted = false;
    const auto temporary = temporaryPath();
    if (!removeIfPresentVerified(temporary, &stagingExisted)) {
        return fail(CompatibilityLocalStoreCode::CleanupFailed,
                    "local compatibility staging file could not be removed and verified");
    }
    if (!dataExisted && !stagingExisted) {
        return {CompatibilityLocalStoreCode::Missing,
                "local compatibility store does not exist"};
    }
    return {};
}

std::optional<std::filesystem::path> defaultCompatibilityLocalStorePath(
    std::string* error) {
#ifdef _WIN32
    wchar_t localAppData[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length == 0u || length >= static_cast<DWORD>(std::size(localAppData))) {
        if (error != nullptr) *error = "LOCALAPPDATA is unavailable or exceeds the bounded path buffer";
        return std::nullopt;
    }
    return std::filesystem::path(localAppData) / L"HydraSeat" /
           L"compatibility-results.jsonl";
#else
    if (error != nullptr) *error = "default compatibility store path is Windows-only";
    return std::nullopt;
#endif
}

std::string_view compatibilityLocalStoreCodeName(CompatibilityLocalStoreCode code) noexcept {
    switch (code) {
        case CompatibilityLocalStoreCode::Success: return "Success";
        case CompatibilityLocalStoreCode::Missing: return "Missing";
        case CompatibilityLocalStoreCode::TooLarge: return "TooLarge";
        case CompatibilityLocalStoreCode::InvalidHistory: return "InvalidHistory";
        case CompatibilityLocalStoreCode::ReadFailed: return "ReadFailed";
        case CompatibilityLocalStoreCode::WriteFailed: return "WriteFailed";
        case CompatibilityLocalStoreCode::RemoveFailed: return "RemoveFailed";
        case CompatibilityLocalStoreCode::CleanupFailed: return "CleanupFailed";
    }
    return "Unknown";
}

} // namespace hydra::community

namespace hydra::materialization {
namespace {

using internal::json::Number;
using internal::json::Value;

class DecisionStoreError final : public std::runtime_error {
public:
    DecisionStoreError(MaterializationDecisionStoreCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    MaterializationDecisionStoreCode code() const noexcept { return code_; }

private:
    MaterializationDecisionStoreCode code_;
};

MaterializationDecisionStoreDiagnostic decisionFailure(
    MaterializationDecisionStoreCode code, std::string message) {
    return {code, std::move(message)};
}

bool boundedIdentifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > profile::kMaximumIdentifierBytes) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        const bool alphaNumeric = (ch >= 'A' && ch <= 'Z') ||
                                  (ch >= 'a' && ch <= 'z') ||
                                  (ch >= '0' && ch <= '9');
        if (!alphaNumeric && ch != '.' && ch != '_' && ch != '-' && ch != ':' &&
            ch != '@' && ch != '+') {
            return false;
        }
    }
    return true;
}

bool boundedStepId(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128u && boundedIdentifier(value);
}

bool validCompatibilityReference(
    const std::optional<profile::CompatibilityReference>& value) noexcept {
    return !value || (boundedIdentifier(value->recordId) &&
                      boundedIdentifier(value->provenance) &&
                      value->evidenceRevision != 0u);
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
                codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) +
                            (low - 0xdc00u);
            } else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
                return false;
            }
        }
        if (codePoint > 0x10ffffu || (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            return false;
        }
        if (codePoint <= 0x7fu) {
            converted.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ffu) {
            converted.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
            converted.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else if (codePoint <= 0xffffu) {
            converted.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
            converted.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            converted.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else {
            converted.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
            converted.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
            converted.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            converted.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        }
    }
    output = std::move(converted);
    return true;
}

bool utf8ToWide(std::string_view input, std::wstring& output) {
    std::wstring converted;
    converted.reserve(input.size());
    std::size_t index = 0u;
    while (index < input.size()) {
        const auto lead = static_cast<unsigned char>(input[index++]);
        std::uint32_t codePoint = 0u;
        unsigned continuationCount = 0u;
        if (lead <= 0x7fu) {
            if (lead < 0x20u || lead == 0x7fu) return false;
            codePoint = lead;
        } else if ((lead & 0xe0u) == 0xc0u) {
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
        if constexpr (sizeof(wchar_t) == 2u) {
            if (codePoint <= 0xffffu) {
                converted.push_back(static_cast<wchar_t>(codePoint));
            } else {
                codePoint -= 0x10000u;
                converted.push_back(static_cast<wchar_t>(0xd800u + (codePoint >> 10u)));
                converted.push_back(static_cast<wchar_t>(0xdc00u + (codePoint & 0x3ffu)));
            }
        } else {
            converted.push_back(static_cast<wchar_t>(codePoint));
        }
    }
    output = std::move(converted);
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
                    output << "\\u00" << digits[(ch >> 4u) & 0x0fu]
                           << digits[ch & 0x0fu];
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

std::string compatibilityJson(
    const std::optional<profile::CompatibilityReference>& value) {
    if (!value) return "null";
    std::ostringstream output;
    output << '{'
           << "\"record_id\":" << quote(value->recordId) << ','
           << "\"provenance\":" << quote(value->provenance) << ','
           << "\"evidence_revision\":" << value->evidenceRevision
           << '}';
    return output.str();
}

std::string_view phaseName(setup::RecipeExecutionPhase phase) noexcept {
    switch (phase) {
        case setup::RecipeExecutionPhase::PreSpawn: return "pre-spawn";
        case setup::RecipeExecutionPhase::Startup: return "startup";
        case setup::RecipeExecutionPhase::PostWindow: return "post-window";
        case setup::RecipeExecutionPhase::Runtime: return "runtime";
    }
    return "unknown";
}

std::string_view scopeName(setup::MutationScope scope) noexcept {
    switch (scope) {
        case setup::MutationScope::SeatWritableInstance: return "seat-writable-instance";
        case setup::MutationScope::SharedInstallation: return "shared-installation";
    }
    return "unknown";
}

const Value::Object& asObject(const Value& value, std::string_view label) {
    const auto* object = std::get_if<Value::Object>(&value.value);
    if (object == nullptr) {
        throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                                 std::string(label) + " must be an object");
    }
    return *object;
}

const Value::Array& asArray(const Value& value, std::string_view label) {
    const auto* array = std::get_if<Value::Array>(&value.value);
    if (array == nullptr) {
        throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                                 std::string(label) + " must be an array");
    }
    return *array;
}

const Value& required(const Value::Object& object, std::string_view key) {
    const auto iterator = object.find(std::string(key));
    if (iterator == object.end()) {
        throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
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
                throw DecisionStoreError(MaterializationDecisionStoreCode::UnknownField,
                                         "unknown field: " + key);
            }
        }
        throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                                 "object is missing one or more required fields");
    }
    for (const auto& [key, value] : object) {
        (void)value;
        if (!names.contains(key)) {
            throw DecisionStoreError(MaterializationDecisionStoreCode::UnknownField,
                                     "unknown field: " + key);
        }
    }
}

std::string asString(const Value& value, std::string_view label) {
    const auto* text = std::get_if<std::string>(&value.value);
    if (text == nullptr) {
        throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                                 std::string(label) + " must be a string");
    }
    return *text;
}

std::optional<std::string> asOptionalString(const Value& value,
                                            std::string_view label) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) return std::nullopt;
    return asString(value, label);
}

std::uint64_t asU64(const Value& value, std::string_view label) {
    const auto* number = std::get_if<Number>(&value.value);
    if (number == nullptr || number->text.empty() || number->text.front() == '-') {
        throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                                 std::string(label) + " must be an unsigned integer");
    }
    std::uint64_t parsed = 0u;
    const auto* begin = number->text.data();
    const auto* end = begin + number->text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                                 std::string(label) + " is outside the unsigned integer range");
    }
    return parsed;
}

std::uint32_t asU32(const Value& value, std::string_view label) {
    const auto parsed = asU64(value, label);
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                                 std::string(label) + " exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(parsed);
}

std::optional<profile::CompatibilityReference> parseCompatibility(
    const Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) return std::nullopt;
    const auto& object = asObject(value, "compatibility");
    requireFields(object, {"record_id", "provenance", "evidence_revision"});
    profile::CompatibilityReference compatibility;
    compatibility.recordId = asString(required(object, "record_id"), "record_id");
    compatibility.provenance = asString(required(object, "provenance"), "provenance");
    compatibility.evidenceRevision =
        asU32(required(object, "evidence_revision"), "evidence_revision");
    return compatibility;
}

LocalMaterializationDecisionOrigin parseOrigin(std::string_view value) {
    if (value == "local-approved") return LocalMaterializationDecisionOrigin::LocalApproved;
    if (value == "imported-community") {
        return LocalMaterializationDecisionOrigin::ImportedCommunity;
    }
    throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                             "unknown materialization decision origin");
}

setup::RecipeExecutionPhase parsePhase(std::string_view value) {
    if (value == "pre-spawn") return setup::RecipeExecutionPhase::PreSpawn;
    if (value == "startup") return setup::RecipeExecutionPhase::Startup;
    if (value == "post-window") return setup::RecipeExecutionPhase::PostWindow;
    if (value == "runtime") return setup::RecipeExecutionPhase::Runtime;
    throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                             "unknown compatibility recipe phase");
}

setup::MutationScope parseScope(std::string_view value) {
    if (value == "seat-writable-instance") return setup::MutationScope::SeatWritableInstance;
    if (value == "shared-installation") return setup::MutationScope::SharedInstallation;
    throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                             "unknown compatibility mutation scope");
}

LocalMaterializationDecision parseDecision(const Value& value) {
    const auto& object = asObject(value, "materialization decision");
    requireFields(object, {"schema_version", "decision_id", "revision", "origin",
                           "setup_id", "instance_index", "game_id", "provider_id",
                           "provider_app_id", "provider_metadata_revision",
                           "requirement_revision", "compatibility", "steps"});
    LocalMaterializationDecision decision;
    decision.schemaVersion = asU32(required(object, "schema_version"), "schema_version");
    decision.decisionId = asString(required(object, "decision_id"), "decision_id");
    decision.revision = asU64(required(object, "revision"), "revision");
    decision.origin = parseOrigin(asString(required(object, "origin"), "origin"));
    decision.setupId = asString(required(object, "setup_id"), "setup_id");
    decision.instanceIndex = asU32(required(object, "instance_index"), "instance_index");
    decision.gameId = asString(required(object, "game_id"), "game_id");
    decision.providerId = asString(required(object, "provider_id"), "provider_id");
    decision.providerAppId =
        asOptionalString(required(object, "provider_app_id"), "provider_app_id");
    decision.providerMetadataRevision =
        asU64(required(object, "provider_metadata_revision"), "provider_metadata_revision");
    decision.requirementRevision =
        asU64(required(object, "requirement_revision"), "requirement_revision");
    decision.compatibility = parseCompatibility(required(object, "compatibility"));

    const auto& steps = asArray(required(object, "steps"), "steps");
    decision.steps.reserve(steps.size());
    for (const auto& stepValue : steps) {
        const auto& stepObject = asObject(stepValue, "materialization step");
        requireFields(stepObject, {"step_id", "phase", "scope", "files"});
        CompatibilityRecipeStep step;
        step.stepId = asString(required(stepObject, "step_id"), "step_id");
        step.phase = parsePhase(asString(required(stepObject, "phase"), "phase"));
        step.scope = parseScope(asString(required(stepObject, "scope"), "scope"));
        const auto& files = asArray(required(stepObject, "files"), "files");
        step.files.reserve(files.size());
        for (const auto& fileValue : files) {
            const auto& fileObject = asObject(fileValue, "mutable file");
            requireFields(fileObject,
                          {"source_relative_path", "destination_relative_path",
                           "maximum_bytes"});
            MutableFileSpec file;
            const auto source = asString(required(fileObject, "source_relative_path"),
                                         "source_relative_path");
            const auto destination = asString(
                required(fileObject, "destination_relative_path"),
                "destination_relative_path");
            if (!utf8ToWide(source, file.sourceRelativePath) ||
                !utf8ToWide(destination, file.destinationRelativePath)) {
                throw DecisionStoreError(MaterializationDecisionStoreCode::ParseError,
                                         "mutable file path is not valid bounded UTF-8");
            }
            file.maximumBytes = asU64(required(fileObject, "maximum_bytes"),
                                      "maximum_bytes");
            step.files.push_back(std::move(file));
        }
        decision.steps.push_back(std::move(step));
    }
    return decision;
}

bool decisionRemoveIfPresentVerified(const std::filesystem::path& path,
                                     bool* existed = nullptr) noexcept {
    std::error_code error;
    const bool present = std::filesystem::exists(path, error);
    if (error) return false;
    if (existed != nullptr) *existed = present;
    if (!present) return true;
    if (!std::filesystem::remove(path, error) || error) return false;
    error.clear();
    const bool remains = std::filesystem::exists(path, error);
    return !error && !remains;
}

bool decisionFlushStagedFile(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    return flushed;
#else
    (void)path;
    return true;
#endif
}

bool decisionPublishStagedFile(const std::filesystem::path& temporary,
                               const std::filesystem::path& current) noexcept {
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), current.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    std::error_code error;
    std::filesystem::rename(temporary, current, error);
    return !error;
#endif
}

MaterializationDecisionStoreDiagnostic decisionFailAfterStaging(
    MaterializationDecisionStoreCode primaryCode,
    std::string message,
    const std::filesystem::path& temporary) {
    if (!decisionRemoveIfPresentVerified(temporary)) {
        return decisionFailure(MaterializationDecisionStoreCode::CleanupFailed,
                               std::move(message) +
                                   "; staging cleanup could not be verified");
    }
    return decisionFailure(primaryCode, std::move(message));
}

std::optional<std::filesystem::path> localAppDataHydraSeatRoot(std::string* error) {
#ifdef _WIN32
    wchar_t localAppData[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length == 0u || length >= static_cast<DWORD>(std::size(localAppData))) {
        if (error != nullptr) {
            *error = "LOCALAPPDATA is unavailable or exceeds the bounded path buffer";
        }
        return std::nullopt;
    }
    return std::filesystem::path(localAppData) / L"HydraSeat";
#else
    if (error != nullptr) *error = "default materialization paths are Windows-only";
    return std::nullopt;
#endif
}

} // namespace

MaterializationDecisionStoreDiagnostic validateLocalMaterializationDecisionDocument(
    const LocalMaterializationDecisionDocument& document) {
    if (document.schemaVersion != kLocalMaterializationDecisionSchemaVersion) {
        return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                               "unsupported local materialization decision document version");
    }
    if (document.decisions.size() > kMaximumLocalMaterializationDecisions) {
        return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                               "too many local materialization decisions");
    }

    std::set<std::string> decisionIds;
    std::set<std::pair<std::string, std::uint32_t>> setupInstances;
    for (const auto& decision : document.decisions) {
        if (decision.schemaVersion != kLocalMaterializationDecisionSchemaVersion ||
            !boundedIdentifier(decision.decisionId) || decision.revision == 0u ||
            !boundedIdentifier(decision.setupId) || decision.instanceIndex >= 2u ||
            !boundedIdentifier(decision.gameId) || !boundedIdentifier(decision.providerId) ||
            (decision.providerAppId && !boundedIdentifier(*decision.providerAppId)) ||
            decision.providerMetadataRevision == 0u || decision.requirementRevision == 0u ||
            !validCompatibilityReference(decision.compatibility)) {
            return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                                   "local materialization decision has invalid identity/revision fields");
        }
        if (decision.origin != LocalMaterializationDecisionOrigin::LocalApproved &&
            decision.origin != LocalMaterializationDecisionOrigin::ImportedCommunity) {
            return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                                   "local materialization decision has an unknown origin");
        }
        if (!decisionIds.insert(decision.decisionId).second ||
            !setupInstances.emplace(decision.setupId, decision.instanceIndex).second) {
            return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                                   "local materialization decision identity is duplicated");
        }
        if (decision.steps.empty() ||
            decision.steps.size() > kMaximumCompatibilityRecipeSteps) {
            return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                                   "local materialization decision has invalid step count");
        }

        std::set<std::string> stepIds;
        std::size_t fileCount = 0u;
        std::uint64_t totalMaximumBytes = 0u;
        for (const auto& step : decision.steps) {
            if (!boundedStepId(step.stepId) || !stepIds.insert(step.stepId).second ||
                step.scope != setup::MutationScope::SeatWritableInstance ||
                step.files.empty()) {
                return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                                       "local materialization step is invalid or requests shared mutation");
            }
            fileCount += step.files.size();
            if (fileCount > kMaximumMutableFilesPerRecipe) {
                return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                                       "local materialization decision exceeds mutable-file count");
            }
            for (const auto& file : step.files) {
                if (file.sourceRelativePath.empty() || file.destinationRelativePath.empty() ||
                    file.sourceRelativePath.size() > 2048u ||
                    file.destinationRelativePath.size() > 2048u ||
                    file.sourceRelativePath.find(L'\0') != std::wstring::npos ||
                    file.destinationRelativePath.find(L'\0') != std::wstring::npos ||
                    file.maximumBytes == 0u ||
                    file.maximumBytes > kMaximumSingleMutableFileBytes ||
                    totalMaximumBytes > kMaximumMutableBytesPerInstance - file.maximumBytes) {
                    return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                                           "local materialization mutable-file bounds are invalid");
                }
                totalMaximumBytes += file.maximumBytes;
                std::string sourceUtf8;
                std::string destinationUtf8;
                if (!wideToUtf8(file.sourceRelativePath, sourceUtf8) ||
                    !wideToUtf8(file.destinationRelativePath, destinationUtf8)) {
                    return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                                           "local materialization path is not valid Unicode");
                }
            }
        }
    }
    return {};
}

MaterializationDecisionStoreDiagnostic encodeLocalMaterializationDecisionDocumentJson(
    const LocalMaterializationDecisionDocument& document,
    std::string& output) {
    const auto valid = validateLocalMaterializationDecisionDocument(document);
    if (!valid.succeeded()) return valid;

    std::ostringstream encoded;
    encoded << "{\"schema_version\":" << document.schemaVersion << ",\"decisions\":[";
    for (std::size_t decisionIndex = 0u; decisionIndex < document.decisions.size();
         ++decisionIndex) {
        if (decisionIndex != 0u) encoded << ',';
        const auto& decision = document.decisions[decisionIndex];
        encoded << '{'
                << "\"schema_version\":" << decision.schemaVersion << ','
                << "\"decision_id\":" << quote(decision.decisionId) << ','
                << "\"revision\":" << decision.revision << ','
                << "\"origin\":" << quote(localMaterializationDecisionOriginName(decision.origin))
                << ','
                << "\"setup_id\":" << quote(decision.setupId) << ','
                << "\"instance_index\":" << decision.instanceIndex << ','
                << "\"game_id\":" << quote(decision.gameId) << ','
                << "\"provider_id\":" << quote(decision.providerId) << ','
                << "\"provider_app_id\":" << optionalStringJson(decision.providerAppId) << ','
                << "\"provider_metadata_revision\":" << decision.providerMetadataRevision << ','
                << "\"requirement_revision\":" << decision.requirementRevision << ','
                << "\"compatibility\":" << compatibilityJson(decision.compatibility) << ','
                << "\"steps\":[";
        for (std::size_t stepIndex = 0u; stepIndex < decision.steps.size(); ++stepIndex) {
            if (stepIndex != 0u) encoded << ',';
            const auto& step = decision.steps[stepIndex];
            encoded << '{'
                    << "\"step_id\":" << quote(step.stepId) << ','
                    << "\"phase\":" << quote(phaseName(step.phase)) << ','
                    << "\"scope\":" << quote(scopeName(step.scope)) << ','
                    << "\"files\":[";
            for (std::size_t fileIndex = 0u; fileIndex < step.files.size(); ++fileIndex) {
                if (fileIndex != 0u) encoded << ',';
                const auto& file = step.files[fileIndex];
                std::string sourceUtf8;
                std::string destinationUtf8;
                if (!wideToUtf8(file.sourceRelativePath, sourceUtf8) ||
                    !wideToUtf8(file.destinationRelativePath, destinationUtf8)) {
                    return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                                           "local materialization path could not be encoded as UTF-8");
                }
                encoded << '{'
                        << "\"source_relative_path\":" << quote(sourceUtf8) << ','
                        << "\"destination_relative_path\":" << quote(destinationUtf8) << ','
                        << "\"maximum_bytes\":" << file.maximumBytes
                        << '}';
            }
            encoded << "]}";
        }
        encoded << "]}";
    }
    encoded << "]}";
    auto bytes = encoded.str();
    if (bytes.empty() || bytes.size() > kMaximumLocalMaterializationDecisionBytes) {
        return decisionFailure(MaterializationDecisionStoreCode::TooLarge,
                               "encoded local materialization decision store exceeds its bound");
    }
    output = std::move(bytes);
    return {};
}

MaterializationDecisionStoreDiagnostic decodeLocalMaterializationDecisionDocumentJson(
    std::string_view bytes,
    LocalMaterializationDecisionDocument& output) {
    if (bytes.empty()) {
        return decisionFailure(MaterializationDecisionStoreCode::ParseError,
                               "local materialization decision store is empty");
    }
    if (bytes.size() > kMaximumLocalMaterializationDecisionBytes) {
        return decisionFailure(MaterializationDecisionStoreCode::TooLarge,
                               "local materialization decision store exceeds its bound");
    }
    try {
        const auto parsed = internal::json::parse(bytes, {48u, 32768u});
        const auto& object = asObject(parsed, "materialization decision document");
        requireFields(object, {"schema_version", "decisions"});
        LocalMaterializationDecisionDocument decoded;
        decoded.schemaVersion = asU32(required(object, "schema_version"), "schema_version");
        const auto& decisions = asArray(required(object, "decisions"), "decisions");
        if (decisions.size() > kMaximumLocalMaterializationDecisions) {
            return decisionFailure(MaterializationDecisionStoreCode::InvalidDocument,
                                   "too many local materialization decisions");
        }
        decoded.decisions.reserve(decisions.size());
        for (const auto& decision : decisions) {
            decoded.decisions.push_back(parseDecision(decision));
        }
        const auto valid = validateLocalMaterializationDecisionDocument(decoded);
        if (!valid.succeeded()) return valid;
        output = std::move(decoded);
        return {};
    } catch (const DecisionStoreError& error) {
        return decisionFailure(error.code(), error.what());
    } catch (const internal::json::ParseError& error) {
        return decisionFailure(MaterializationDecisionStoreCode::ParseError, error.what());
    } catch (const std::exception& error) {
        return decisionFailure(MaterializationDecisionStoreCode::ParseError, error.what());
    }
}

std::filesystem::path LocalMaterializationDecisionStore::temporaryPath() const {
    auto value = path_;
    value += L".tmp";
    return value;
}

MaterializationDecisionStoreDiagnostic LocalMaterializationDecisionStore::load(
    LocalMaterializationDecisionDocument& document) const {
    if (path_.empty()) {
        return decisionFailure(MaterializationDecisionStoreCode::ReadFailed,
                               "local materialization decision store path is empty");
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(path_, error);
    if (error) {
        return decisionFailure(MaterializationDecisionStoreCode::ReadFailed,
                               "local materialization decision store existence check failed");
    }
    if (!exists) {
        return {MaterializationDecisionStoreCode::Missing,
                "local materialization decision store does not exist"};
    }
    const auto byteCount = std::filesystem::file_size(path_, error);
    if (error) {
        return decisionFailure(MaterializationDecisionStoreCode::ReadFailed,
                               "local materialization decision store size could not be read");
    }
    if (byteCount == 0u) {
        return decisionFailure(MaterializationDecisionStoreCode::ParseError,
                               "local materialization decision store is empty");
    }
    if (byteCount > kMaximumLocalMaterializationDecisionBytes) {
        return decisionFailure(MaterializationDecisionStoreCode::TooLarge,
                               "local materialization decision store exceeds its bounded size");
    }
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        return decisionFailure(MaterializationDecisionStoreCode::ReadFailed,
                               "local materialization decision store could not be opened");
    }
    std::string bytes(static_cast<std::size_t>(byteCount), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return decisionFailure(MaterializationDecisionStoreCode::ReadFailed,
                               "local materialization decision store could not be read completely");
    }
    char unexpected = '\0';
    if (input.get(unexpected)) {
        return decisionFailure(MaterializationDecisionStoreCode::ReadFailed,
                               "local materialization decision store changed while being read");
    }
    LocalMaterializationDecisionDocument decoded;
    const auto parsed = decodeLocalMaterializationDecisionDocumentJson(bytes, decoded);
    if (!parsed.succeeded()) return parsed;
    document = std::move(decoded);
    return {};
}

MaterializationDecisionStoreDiagnostic LocalMaterializationDecisionStore::save(
    const LocalMaterializationDecisionDocument& document) const {
    if (path_.empty()) {
        return decisionFailure(MaterializationDecisionStoreCode::WriteFailed,
                               "local materialization decision store path is empty");
    }
    std::string bytes;
    const auto encoded = encodeLocalMaterializationDecisionDocumentJson(document, bytes);
    if (!encoded.succeeded()) return encoded;

    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return decisionFailure(MaterializationDecisionStoreCode::WriteFailed,
                                   "local materialization decision directory could not be created");
        }
    }
    const auto temporary = temporaryPath();
    if (!decisionRemoveIfPresentVerified(temporary)) {
        return decisionFailure(MaterializationDecisionStoreCode::CleanupFailed,
                               "stale local materialization staging file could not be removed safely");
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return decisionFailAfterStaging(MaterializationDecisionStoreCode::WriteFailed,
                                            "local materialization staging file could not be opened",
                                            temporary);
        }
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            output.close();
            return decisionFailAfterStaging(MaterializationDecisionStoreCode::WriteFailed,
                                            "local materialization staging write failed",
                                            temporary);
        }
    }
    if (!decisionFlushStagedFile(temporary)) {
        return decisionFailAfterStaging(MaterializationDecisionStoreCode::WriteFailed,
                                        "local materialization staging flush failed",
                                        temporary);
    }
    if (!decisionPublishStagedFile(temporary, path_)) {
        return decisionFailAfterStaging(MaterializationDecisionStoreCode::WriteFailed,
                                        "local materialization atomic publication failed",
                                        temporary);
    }
    if (!decisionRemoveIfPresentVerified(temporary)) {
        return decisionFailure(MaterializationDecisionStoreCode::CleanupFailed,
                               "published local materialization staging cleanup could not be verified");
    }
    return {};
}

MaterializationDecisionStoreDiagnostic LocalMaterializationDecisionStore::remove() const {
    if (path_.empty()) {
        return decisionFailure(MaterializationDecisionStoreCode::RemoveFailed,
                               "local materialization decision store path is empty");
    }
    bool dataExisted = false;
    if (!decisionRemoveIfPresentVerified(path_, &dataExisted)) {
        return decisionFailure(MaterializationDecisionStoreCode::RemoveFailed,
                               "local materialization decision store could not be removed and verified");
    }
    bool stagingExisted = false;
    const auto temporary = temporaryPath();
    if (!decisionRemoveIfPresentVerified(temporary, &stagingExisted)) {
        return decisionFailure(MaterializationDecisionStoreCode::CleanupFailed,
                               "local materialization staging file could not be removed and verified");
    }
    if (!dataExisted && !stagingExisted) {
        return {MaterializationDecisionStoreCode::Missing,
                "local materialization decision store does not exist"};
    }
    return {};
}

TrustedMaterializationDecisionDiagnostic
StoreBackedTrustedMaterializationDecisionSource::resolveCurrent(
    const MaterializationDecisionQuery& query,
    LocalMaterializationDecision& output) {
    if (!boundedIdentifier(query.setupId) || query.instanceIndex >= 2u ||
        !boundedIdentifier(query.gameId) || !boundedIdentifier(query.providerId) ||
        (query.providerAppId && !boundedIdentifier(*query.providerAppId)) ||
        query.providerMetadataRevision == 0u || query.requirementRevision == 0u ||
        !validCompatibilityReference(query.compatibility)) {
        return {TrustedMaterializationDecisionCode::IdentityMismatch,
                "materialization decision query has invalid exact runtime identity"};
    }

    LocalMaterializationDecisionDocument document;
    const auto loaded = store_.load(document);
    if (loaded.code == MaterializationDecisionStoreCode::Missing) {
        return {TrustedMaterializationDecisionCode::NotRequired,
                "no local materialization decision exists"};
    }
    if (!loaded.succeeded()) {
        return {TrustedMaterializationDecisionCode::InvalidStore,
                "local materialization decision store is unavailable or invalid: " + loaded.message};
    }

    const auto found = std::find_if(
        document.decisions.begin(), document.decisions.end(),
        [&](const LocalMaterializationDecision& decision) {
            return decision.setupId == query.setupId &&
                   decision.instanceIndex == query.instanceIndex;
        });
    if (found == document.decisions.end()) {
        return {TrustedMaterializationDecisionCode::NotRequired,
                "setup/instance has no locally approved materialization decision"};
    }
    if (found->origin != LocalMaterializationDecisionOrigin::LocalApproved) {
        return {TrustedMaterializationDecisionCode::UntrustedOrigin,
                "imported/community materialization data is not runtime mutation authority"};
    }
    if (found->gameId != query.gameId || found->providerId != query.providerId ||
        found->providerAppId != query.providerAppId ||
        found->providerMetadataRevision != query.providerMetadataRevision ||
        found->requirementRevision != query.requirementRevision ||
        found->compatibility != query.compatibility) {
        return {TrustedMaterializationDecisionCode::IdentityMismatch,
                "local materialization decision is stale for current game/provider/requirement identity"};
    }
    output = *found;
    return {};
}

std::optional<std::filesystem::path> defaultLocalMaterializationDecisionStorePath(
    std::string* error) {
    const auto root = localAppDataHydraSeatRoot(error);
    if (!root) return std::nullopt;
    return *root / L"materialization-decisions.json";
}

std::optional<std::filesystem::path> defaultInstanceMaterializationRoot(
    std::string* error) {
    const auto root = localAppDataHydraSeatRoot(error);
    if (!root) return std::nullopt;
    return *root / L"instances";
}

std::string_view materializationDecisionStoreCodeName(
    MaterializationDecisionStoreCode code) noexcept {
    switch (code) {
        case MaterializationDecisionStoreCode::Success: return "Success";
        case MaterializationDecisionStoreCode::Missing: return "Missing";
        case MaterializationDecisionStoreCode::TooLarge: return "TooLarge";
        case MaterializationDecisionStoreCode::InvalidDocument: return "InvalidDocument";
        case MaterializationDecisionStoreCode::ParseError: return "ParseError";
        case MaterializationDecisionStoreCode::UnknownField: return "UnknownField";
        case MaterializationDecisionStoreCode::ReadFailed: return "ReadFailed";
        case MaterializationDecisionStoreCode::WriteFailed: return "WriteFailed";
        case MaterializationDecisionStoreCode::RemoveFailed: return "RemoveFailed";
        case MaterializationDecisionStoreCode::CleanupFailed: return "CleanupFailed";
    }
    return "Unknown";
}

std::string_view trustedMaterializationDecisionCodeName(
    TrustedMaterializationDecisionCode code) noexcept {
    switch (code) {
        case TrustedMaterializationDecisionCode::Success: return "Success";
        case TrustedMaterializationDecisionCode::NotRequired: return "NotRequired";
        case TrustedMaterializationDecisionCode::InvalidStore: return "InvalidStore";
        case TrustedMaterializationDecisionCode::UntrustedOrigin: return "UntrustedOrigin";
        case TrustedMaterializationDecisionCode::IdentityMismatch: return "IdentityMismatch";
    }
    return "Unknown";
}

std::string_view localMaterializationDecisionOriginName(
    LocalMaterializationDecisionOrigin origin) noexcept {
    switch (origin) {
        case LocalMaterializationDecisionOrigin::LocalApproved: return "local-approved";
        case LocalMaterializationDecisionOrigin::ImportedCommunity: return "imported-community";
    }
    return "unknown";
}

} // namespace hydra::materialization
