#include "hydra/acceptance_campaign.hpp"

#include "hydra/internal/strict_json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <type_traits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hydra::acceptance {
namespace {

using JsonValue = internal::json::Value;
using JsonObject = internal::json::Value::Object;
using JsonArray = internal::json::Value::Array;

CampaignDiagnostic fail(CampaignCode code, std::string message) {
    return {code, std::move(message)};
}

bool asciiToken(std::string_view value, std::size_t maximum = kMaximumIdentifierBytes) {
    if (value.empty() || value.size() > maximum) return false;
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

bool hex(std::string_view value, std::size_t size) {
    if (value.size() != size) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool validStage(CampaignStage value) noexcept {
    return static_cast<std::uint8_t>(value) < kCampaignStageCount;
}

bool validState(StageState value) noexcept {
    return static_cast<std::uint8_t>(value) <=
           static_cast<std::uint8_t>(StageState::RecoveryRequired);
}

bool validOrigin(EvidenceOrigin value) noexcept {
    return static_cast<std::uint8_t>(value) <=
           static_cast<std::uint8_t>(EvidenceOrigin::Physical);
}

bool validEvidenceClass(EvidenceClass value) noexcept {
    return static_cast<std::uint8_t>(value) <=
           static_cast<std::uint8_t>(EvidenceClass::SigningDeployment);
}

bool validVerdict(HumanVerdict value) noexcept {
    return static_cast<std::uint8_t>(value) <=
           static_cast<std::uint8_t>(HumanVerdict::Fail);
}

bool validIdentity(const CampaignIdentity& value) {
    const bool validArchitecture = value.architecture == "x64" ||
                                   value.architecture == "x86" ||
                                   value.architecture == "arm64";
    return asciiToken(value.campaignId) && hex(value.rcCommitSha, 40u) &&
           hex(value.releaseArtifactSha256, 64u) && asciiToken(value.releaseArtifactName) &&
           value.releaseRevision != 0u && validArchitecture &&
           hex(value.profileSha256, 64u) && hex(value.installStateSha256, 64u) &&
           asciiToken(value.windowsBuild) &&
           hex(value.topologyFingerprintSha256, 64u) &&
           value.seatIds[0] != 0u && value.seatIds[1] != 0u &&
           value.seatIds[0] != value.seatIds[1] &&
           value.seatIds[0] <= 2u && value.seatIds[1] <= 2u &&
           asciiToken(value.scenarioIdentity) && asciiToken(value.sessionRunId) &&
           value.sessionRunId == value.campaignId;
}

StageRecord* findStage(AcceptanceCampaign& campaign, CampaignStage stage) {
    const auto found = std::find_if(campaign.stages.begin(), campaign.stages.end(),
                                    [stage](const StageRecord& record) {
                                        return record.stage == stage;
                                    });
    return found == campaign.stages.end() ? nullptr : &*found;
}

const StageRecord* findStage(const AcceptanceCampaign& campaign, CampaignStage stage) {
    const auto found = std::find_if(campaign.stages.begin(), campaign.stages.end(),
                                    [stage](const StageRecord& record) {
                                        return record.stage == stage;
                                    });
    return found == campaign.stages.end() ? nullptr : &*found;
}

bool prerequisitesPassed(const AcceptanceCampaign& campaign, CampaignStage stage) {
    const auto target = static_cast<std::uint8_t>(stage);
    for (std::uint8_t raw = 0u; raw < target; ++raw) {
        const auto* record = findStage(campaign, static_cast<CampaignStage>(raw));
        if (record == nullptr || record->state != StageState::Passed) return false;
    }
    return true;
}

CampaignDiagnostic validateEvidence(const AcceptanceCampaign& campaign,
                                    const ChildEvidence& evidence,
                                    std::uint64_t nowUnixSeconds) {
    const bool validArchitecture = evidence.architecture == "x64" ||
                                   evidence.architecture == "x86" ||
                                   evidence.architecture == "arm64";
    if (evidence.schemaVersion != kEvidenceSchemaVersion ||
        evidence.campaignSchemaVersion != kCampaignSchemaVersion ||
        !asciiToken(evidence.evidenceId) || !validStage(evidence.stage) ||
        !validOrigin(evidence.origin) || !validEvidenceClass(evidence.evidenceClass) ||
        !validVerdict(evidence.humanVerdict) ||
        !hex(evidence.contentSha256, 64u) || !asciiToken(evidence.evidenceArtifactName) ||
        !asciiToken(evidence.testName) || evidence.releaseRevision == 0u ||
        !validArchitecture || !hex(evidence.profileSha256, 64u) ||
        !hex(evidence.installStateSha256, 64u) ||
        !asciiToken(evidence.campaignId) || !asciiToken(evidence.sessionRunId) ||
        !asciiToken(evidence.releaseArtifactName) || !asciiToken(evidence.windowsBuild) ||
        !hex(evidence.topologyFingerprintSha256, 64u) ||
        evidence.note.size() > kMaximumNoteBytes) {
        return fail(CampaignCode::EvidenceMalformed,
                    "child evidence contains invalid version, enum, identity, hash, or bounds");
    }
    if (evidence.createdUnixSeconds == 0u ||
        evidence.createdUnixSeconds > nowUnixSeconds + 300u ||
        nowUnixSeconds - std::min(nowUnixSeconds, evidence.createdUnixSeconds) >
            kMaximumEvidenceAgeSeconds) {
        return fail(CampaignCode::EvidenceStale,
                    "child evidence timestamp is stale or implausibly in the future");
    }
    if (evidence.rcCommitSha != campaign.identity.rcCommitSha ||
        evidence.releaseArtifactSha256 != campaign.identity.releaseArtifactSha256 ||
        evidence.releaseArtifactName != campaign.identity.releaseArtifactName ||
        evidence.releaseRevision != campaign.identity.releaseRevision ||
        evidence.architecture != campaign.identity.architecture ||
        evidence.profileSha256 != campaign.identity.profileSha256 ||
        evidence.installStateSha256 != campaign.identity.installStateSha256 ||
        evidence.windowsBuild != campaign.identity.windowsBuild ||
        evidence.topologyFingerprintSha256 != campaign.identity.topologyFingerprintSha256 ||
        evidence.scenarioIdentity != campaign.identity.scenarioIdentity ||
        evidence.campaignId != campaign.identity.campaignId ||
        evidence.sessionRunId != campaign.identity.sessionRunId) {
        return fail(CampaignCode::EvidenceIdentityMismatch,
                    "child evidence is not bound to this exact campaign/session, RC, build/profile, scenario, and input artifacts");
    }
    if (evidence.evidenceClass != evidenceClassForStage(evidence.stage)) {
        return fail(CampaignCode::EvidenceClassMismatch,
                    "child evidence class cannot satisfy this campaign stage");
    }
    if (stageRequiresPhysicalEvidence(evidence.stage) &&
        evidence.origin != EvidenceOrigin::Physical) {
        return fail(CampaignCode::PhysicalEvidenceRequired,
                    "controlled or synthetic origin cannot satisfy this release evidence class");
    }
    if (evidenceClassForStage(evidence.stage) == EvidenceClass::Controlled &&
        evidence.origin != EvidenceOrigin::ControlledProcess) {
        return fail(CampaignCode::EvidenceClassMismatch,
                    "controlled campaign stage requires controlled-process evidence origin");
    }
    return {};
}

void appendEscaped(std::string& output, std::string_view value) {
    output.push_back('"');
    constexpr char digits[] = "0123456789abcdef";
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
                output.push_back(digits[(ch >> 4u) & 0x0fu]);
                output.push_back(digits[ch & 0x0fu]);
            } else {
                output.push_back(static_cast<char>(ch));
            }
        }
    }
    output.push_back('"');
}

template <typename Enum>
void appendEnum(std::string& output, Enum value) {
    if constexpr (std::is_same_v<Enum, CampaignStage>) appendEscaped(output, campaignStageName(value));
    else if constexpr (std::is_same_v<Enum, StageState>) appendEscaped(output, stageStateName(value));
    else if constexpr (std::is_same_v<Enum, EvidenceOrigin>) appendEscaped(output, evidenceOriginName(value));
    else appendEscaped(output, humanVerdictName(value));
}

const JsonObject* object(const JsonValue& value) {
    return std::get_if<JsonObject>(&value.value);
}

const JsonArray* array(const JsonValue& value) {
    return std::get_if<JsonArray>(&value.value);
}

const std::string* stringValue(const JsonObject& value, std::string_view key) {
    const auto found = value.find(std::string(key));
    return found == value.end() ? nullptr : std::get_if<std::string>(&found->second.value);
}

const bool* boolValue(const JsonObject& value, std::string_view key) {
    const auto found = value.find(std::string(key));
    return found == value.end() ? nullptr : std::get_if<bool>(&found->second.value);
}

std::optional<std::uint64_t> unsignedValue(const JsonObject& value, std::string_view key) {
    const auto found = value.find(std::string(key));
    if (found == value.end()) return std::nullopt;
    const auto* number = std::get_if<internal::json::Number>(&found->second.value);
    if (number == nullptr || number->text.empty() || number->text.front() == '-') {
        return std::nullopt;
    }
    std::uint64_t output = 0u;
    const auto parsed = std::from_chars(number->text.data(),
                                        number->text.data() + number->text.size(), output);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size()) {
        return std::nullopt;
    }
    return output;
}

bool exactKeys(const JsonObject& objectValue,
               std::initializer_list<std::string_view> expected) {
    if (objectValue.size() != expected.size()) return false;
    return std::all_of(expected.begin(), expected.end(), [&](std::string_view key) {
        return objectValue.contains(std::string(key));
    });
}

template <typename Enum>
std::optional<Enum> parseEnum(std::string_view name);

template <>
std::optional<CampaignStage> parseEnum(std::string_view name) {
    for (std::uint8_t raw = 0u; raw < kCampaignStageCount; ++raw) {
        const auto value = static_cast<CampaignStage>(raw);
        if (campaignStageName(value) == name) return value;
    }
    return std::nullopt;
}

template <>
std::optional<StageState> parseEnum(std::string_view name) {
    for (std::uint8_t raw = 0u; raw <= static_cast<std::uint8_t>(StageState::RecoveryRequired); ++raw) {
        const auto value = static_cast<StageState>(raw);
        if (stageStateName(value) == name) return value;
    }
    return std::nullopt;
}

template <>
std::optional<EvidenceOrigin> parseEnum(std::string_view name) {
    for (std::uint8_t raw = 0u; raw <= static_cast<std::uint8_t>(EvidenceOrigin::Physical); ++raw) {
        const auto value = static_cast<EvidenceOrigin>(raw);
        if (evidenceOriginName(value) == name) return value;
    }
    return std::nullopt;
}

template <>
std::optional<EvidenceClass> parseEnum(std::string_view name) {
    for (std::uint8_t raw = 0u; raw <= static_cast<std::uint8_t>(EvidenceClass::SigningDeployment); ++raw) {
        const auto value = static_cast<EvidenceClass>(raw);
        if (evidenceClassName(value) == name) return value;
    }
    return std::nullopt;
}

template <>
std::optional<HumanVerdict> parseEnum(std::string_view name) {
    for (std::uint8_t raw = 0u; raw <= static_cast<std::uint8_t>(HumanVerdict::Fail); ++raw) {
        const auto value = static_cast<HumanVerdict>(raw);
        if (humanVerdictName(value) == name) return value;
    }
    return std::nullopt;
}

bool decodeEvidence(const JsonValue& value, ChildEvidence& output) {
    const auto* root = object(value);
    if (root == nullptr || !exactKeys(*root, {"schema_version", "evidence_id", "stage",
        "origin", "created_unix", "content_sha256", "evidence_artifact_name", "test_name",
        "rc_commit_sha", "release_artifact_sha256", "release_revision", "architecture",
        "profile_sha256", "install_state_sha256", "scenario_identity", "automated_passed",
        "human_verdict", "note", "campaign_schema_version", "campaign_id", "session_run_id",
        "release_artifact_name", "windows_build", "topology_fingerprint_sha256",
        "evidence_class"})) return false;
    const auto schema = unsignedValue(*root, "schema_version");
    const auto campaignSchema = unsignedValue(*root, "campaign_schema_version");
    const auto created = unsignedValue(*root, "created_unix");
    const auto revision = unsignedValue(*root, "release_revision");
    const auto* id = stringValue(*root, "evidence_id");
    const auto* stage = stringValue(*root, "stage");
    const auto* origin = stringValue(*root, "origin");
    const auto* evidenceClass = stringValue(*root, "evidence_class");
    const auto* sha = stringValue(*root, "content_sha256");
    const auto* evidenceArtifact = stringValue(*root, "evidence_artifact_name");
    const auto* testName = stringValue(*root, "test_name");
    const auto* commit = stringValue(*root, "rc_commit_sha");
    const auto* artifact = stringValue(*root, "release_artifact_sha256");
    const auto* artifactName = stringValue(*root, "release_artifact_name");
    const auto* architecture = stringValue(*root, "architecture");
    const auto* profile = stringValue(*root, "profile_sha256");
    const auto* installState = stringValue(*root, "install_state_sha256");
    const auto* scenario = stringValue(*root, "scenario_identity");
    const auto* campaignId = stringValue(*root, "campaign_id");
    const auto* sessionRunId = stringValue(*root, "session_run_id");
    const auto* windowsBuild = stringValue(*root, "windows_build");
    const auto* topology = stringValue(*root, "topology_fingerprint_sha256");
    const auto* passed = boolValue(*root, "automated_passed");
    const auto* verdict = stringValue(*root, "human_verdict");
    const auto* note = stringValue(*root, "note");
    if (!schema || *schema > std::numeric_limits<std::uint32_t>::max() ||
        !campaignSchema || *campaignSchema > std::numeric_limits<std::uint32_t>::max() ||
        !created || !revision || !id || !stage || !origin || !evidenceClass || !sha ||
        !evidenceArtifact || !testName || !commit || !artifact || !artifactName ||
        !architecture || !profile || !installState || !scenario || !campaignId ||
        !sessionRunId || !windowsBuild || !topology || !passed || !verdict || !note) return false;
    const auto parsedStage = parseEnum<CampaignStage>(*stage);
    const auto parsedOrigin = parseEnum<EvidenceOrigin>(*origin);
    const auto parsedClass = parseEnum<EvidenceClass>(*evidenceClass);
    const auto parsedVerdict = parseEnum<HumanVerdict>(*verdict);
    if (!parsedStage || !parsedOrigin || !parsedClass || !parsedVerdict) return false;
    ChildEvidence candidate;
    candidate.schemaVersion = static_cast<std::uint32_t>(*schema);
    candidate.evidenceId = *id;
    candidate.stage = *parsedStage;
    candidate.origin = *parsedOrigin;
    candidate.createdUnixSeconds = *created;
    candidate.contentSha256 = *sha;
    candidate.evidenceArtifactName = *evidenceArtifact;
    candidate.testName = *testName;
    candidate.rcCommitSha = *commit;
    candidate.releaseArtifactSha256 = *artifact;
    candidate.releaseRevision = *revision;
    candidate.architecture = *architecture;
    candidate.profileSha256 = *profile;
    candidate.installStateSha256 = *installState;
    candidate.scenarioIdentity = *scenario;
    candidate.automatedPassed = *passed;
    candidate.humanVerdict = *parsedVerdict;
    candidate.note = *note;
    candidate.campaignSchemaVersion = static_cast<std::uint32_t>(*campaignSchema);
    candidate.campaignId = *campaignId;
    candidate.sessionRunId = *sessionRunId;
    candidate.releaseArtifactName = *artifactName;
    candidate.windowsBuild = *windowsBuild;
    candidate.topologyFingerprintSha256 = *topology;
    candidate.evidenceClass = *parsedClass;
    output = std::move(candidate);
    return true;
}

bool flushFile(const std::filesystem::path& path) {
#if defined(_WIN32)
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return ok;
#else
    const int file = ::open(path.c_str(), O_RDONLY);
    if (file < 0) return false;
    const bool ok = ::fsync(file) == 0;
    ::close(file);
    return ok;
#endif
}

bool removeIfPresentVerified(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const bool present = std::filesystem::exists(path, error);
    if (error) return false;
    if (!present) return true;
    if (!std::filesystem::remove(path, error) || error) return false;
    error.clear();
    return !std::filesystem::exists(path, error) && !error;
}

CampaignDiagnostic failAfterStaging(std::string message,
                                    const std::filesystem::path& staged) {
    if (!removeIfPresentVerified(staged)) {
        message += "; staged campaign cleanup could not be verified";
    }
    return fail(CampaignCode::StorageFailed, std::move(message));
}

CampaignDiagnostic readBounded(const std::filesystem::path& path, std::string& bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0u || size > kMaximumCampaignBytes) {
        return fail(CampaignCode::StorageFailed, "campaign file is missing, empty, or oversized");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return fail(CampaignCode::StorageFailed, "campaign file could not be opened");
    bytes.resize(static_cast<std::size_t>(size));
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        return fail(CampaignCode::StorageFailed, "campaign file read was incomplete");
    }
    return {};
}

} // namespace

AcceptanceCampaign makeCampaign(const CampaignIdentity& identity,
                                  std::uint64_t nowUnixSeconds,
                                  CampaignDiagnostic* diagnostic) {
    AcceptanceCampaign campaign;
    auto boundIdentity = identity;
    if (boundIdentity.sessionRunId.empty()) boundIdentity.sessionRunId = boundIdentity.campaignId;
    if (!validIdentity(boundIdentity) || nowUnixSeconds == 0u) {
        if (diagnostic) *diagnostic = fail(CampaignCode::InvalidIdentity,
                                           "campaign identity is invalid or unbounded");
        return campaign;
    }
    campaign.identity = std::move(boundIdentity);
    campaign.createdUnixSeconds = nowUnixSeconds;
    campaign.updatedUnixSeconds = nowUnixSeconds;
    campaign.stages.reserve(kCampaignStageCount);
    for (std::uint8_t raw = 0u; raw < kCampaignStageCount; ++raw) {
        StageRecord record;
        record.stage = static_cast<CampaignStage>(raw);
        campaign.stages.push_back(std::move(record));
    }
    if (diagnostic) *diagnostic = {};
    return campaign;
}

CampaignDiagnostic validateCampaign(const AcceptanceCampaign& campaign,
                                      std::uint64_t nowUnixSeconds) {
    if (campaign.schemaVersion != kCampaignSchemaVersion ||
        !validIdentity(campaign.identity) || campaign.createdUnixSeconds == 0u ||
        campaign.updatedUnixSeconds < campaign.createdUnixSeconds ||
        campaign.updatedUnixSeconds > nowUnixSeconds + 300u ||
        campaign.stages.size() != kCampaignStageCount) {
        return fail(CampaignCode::InvalidCampaign,
                    "campaign header, identity, timestamps, or stage count is invalid");
    }
    std::array<bool, kCampaignStageCount> seen{};
    std::set<std::string> evidenceIds;
    std::set<std::string> evidenceArtifacts;
    std::set<std::string> testNames;
    for (const auto& stage : campaign.stages) {
        if (!validStage(stage.stage) || !validState(stage.state) ||
            stage.diagnostic.size() > kMaximumNoteBytes ||
            stage.evidence.size() > kMaximumEvidencePerStage) {
            return fail(CampaignCode::InvalidCampaign,
                        "campaign stage enum, diagnostic, or evidence bound is invalid");
        }
        const auto index = static_cast<std::size_t>(stage.stage);
        if (seen[index]) return fail(CampaignCode::InvalidCampaign, "duplicate campaign stage");
        seen[index] = true;
        if (stage.state == StageState::Pending) {
            if (stage.attempt != 0u || stage.startedUnixSeconds != 0u ||
                stage.completedUnixSeconds != 0u || !stage.evidence.empty()) {
                return fail(CampaignCode::InvalidCampaign, "pending stage carries execution state");
            }
        } else if (stage.attempt == 0u || stage.startedUnixSeconds == 0u ||
                   stage.startedUnixSeconds > nowUnixSeconds + 300u) {
            return fail(CampaignCode::InvalidCampaign, "started stage lacks valid attempt/time");
        }
        if ((stage.state == StageState::Passed || stage.state == StageState::Failed ||
             stage.state == StageState::RecoveryRequired) &&
            stage.completedUnixSeconds < stage.startedUnixSeconds) {
            return fail(CampaignCode::InvalidCampaign, "terminal stage completion time is invalid");
        }
        if ((stage.state == StageState::Running || stage.state == StageState::RecoveryRequired) &&
            !stage.evidence.empty()) {
            return fail(CampaignCode::InvalidCampaign,
                        "running/recovery-required stage cannot retain completed evidence");
        }
        for (const auto& evidence : stage.evidence) {
            const auto valid = validateEvidence(campaign, evidence, nowUnixSeconds);
            if (!valid.succeeded() || evidence.stage != stage.stage) return valid.succeeded()
                ? fail(CampaignCode::InvalidCampaign, "evidence is attached to the wrong stage")
                : valid;
            if (!evidenceIds.insert(evidence.evidenceId).second ||
                !evidenceArtifacts.insert(evidence.evidenceArtifactName).second ||
                !testNames.insert(evidence.testName).second) {
                return fail(CampaignCode::EvidenceDuplicate,
                            "duplicate evidence id, artifact, or test identity");
            }
        }
        const bool hasEvidence = !stage.evidence.empty();
        const bool automatedPassed = hasEvidence &&
            std::all_of(stage.evidence.begin(), stage.evidence.end(),
                        [](const ChildEvidence& item) { return item.automatedPassed; });
        const bool allManualPending = hasEvidence &&
            std::all_of(stage.evidence.begin(), stage.evidence.end(),
                        [](const ChildEvidence& item) {
                            return item.humanVerdict == HumanVerdict::Pending;
                        });
        const bool allManualPassed = hasEvidence &&
            std::all_of(stage.evidence.begin(), stage.evidence.end(),
                        [](const ChildEvidence& item) {
                            return item.humanVerdict == HumanVerdict::Pass;
                        });
        if (stage.state == StageState::AwaitingManualReview &&
            (!stageRequiresManualReview(stage.stage) || !automatedPassed || !allManualPending)) {
            return fail(CampaignCode::ManualVerdictRequired,
                        "manual-review stage is missing pending successful evidence");
        }
        if (stage.state == StageState::Passed &&
            (!automatedPassed ||
             (stageRequiresManualReview(stage.stage) && !allManualPassed))) {
            return fail(CampaignCode::ManualVerdictRequired,
                        "passed stage lacks successful evidence and final manual verdict");
        }
    }
    return {};
}

CampaignDiagnostic startStage(AcceptanceCampaign& campaign, CampaignStage stage,
                              std::uint64_t nowUnixSeconds) {
    const auto valid = validateCampaign(campaign, nowUnixSeconds);
    if (!valid.succeeded()) return valid;
    auto* record = findStage(campaign, stage);
    if (record == nullptr || !prerequisitesPassed(campaign, stage)) {
        return fail(CampaignCode::InvalidTransition,
                    "stage is unknown or prior stages have not passed");
    }
    if (record->state != StageState::Pending && record->state != StageState::Failed &&
        record->state != StageState::RecoveryRequired) {
        return fail(CampaignCode::InvalidTransition, "stage cannot be started from its current state");
    }
    StageRecord candidate;
    candidate.stage = stage;
    candidate.state = StageState::Running;
    candidate.attempt = record->attempt + 1u;
    if (candidate.attempt == 0u) {
        return fail(CampaignCode::InvalidTransition, "stage attempt counter is exhausted");
    }
    candidate.startedUnixSeconds = nowUnixSeconds;
    *record = std::move(candidate);
    campaign.updatedUnixSeconds = nowUnixSeconds;
    return {};
}

CampaignDiagnostic attachEvidence(AcceptanceCampaign& campaign,
                                  const ChildEvidence& evidence,
                                  std::uint64_t nowUnixSeconds) {
    const auto validCampaign = validateCampaign(campaign, nowUnixSeconds);
    if (!validCampaign.succeeded()) return validCampaign;
    auto* record = findStage(campaign, evidence.stage);
    if (record == nullptr || record->state != StageState::Running) {
        return fail(CampaignCode::InvalidTransition, "evidence requires a running matching stage");
    }
    auto boundEvidence = evidence;
    if (boundEvidence.campaignId.empty()) boundEvidence.campaignId = campaign.identity.campaignId;
    if (boundEvidence.sessionRunId.empty()) boundEvidence.sessionRunId = campaign.identity.sessionRunId;
    if (boundEvidence.releaseArtifactName.empty()) {
        boundEvidence.releaseArtifactName = campaign.identity.releaseArtifactName;
    }
    if (boundEvidence.windowsBuild.empty()) boundEvidence.windowsBuild = campaign.identity.windowsBuild;
    if (boundEvidence.topologyFingerprintSha256.empty()) {
        boundEvidence.topologyFingerprintSha256 = campaign.identity.topologyFingerprintSha256;
    }
    if (boundEvidence.evidenceClass == EvidenceClass::Unspecified) {
        boundEvidence.evidenceClass = evidenceClassForStage(boundEvidence.stage);
    }
    const auto validEvidence = validateEvidence(campaign, boundEvidence, nowUnixSeconds);
    if (!validEvidence.succeeded()) return validEvidence;
    if (boundEvidence.humanVerdict != HumanVerdict::Pending) {
        return fail(CampaignCode::ManualVerdictRequired,
                    "attached evidence cannot pre-authorize a manual verdict");
    }
    for (const auto& stage : campaign.stages) {
        if (std::any_of(stage.evidence.begin(), stage.evidence.end(), [&](const ChildEvidence& item) {
                return item.evidenceId == boundEvidence.evidenceId ||
                       item.evidenceArtifactName == boundEvidence.evidenceArtifactName ||
                       item.testName == boundEvidence.testName;
            })) {
            return fail(CampaignCode::EvidenceDuplicate,
                        "duplicate evidence id, artifact, or test identity");
        }
    }
    if (record->evidence.size() >= kMaximumEvidencePerStage) {
        return fail(CampaignCode::EvidenceMalformed, "stage evidence count exceeds bound");
    }
    record->evidence.push_back(boundEvidence);
    record->completedUnixSeconds = nowUnixSeconds;
    if (!boundEvidence.automatedPassed || boundEvidence.humanVerdict == HumanVerdict::Fail) {
        record->state = StageState::Failed;
        record->diagnostic = "evidence reported failure";
    } else if (stageRequiresManualReview(boundEvidence.stage)) {
        record->state = StageState::AwaitingManualReview;
        record->diagnostic = "manual review is required";
    } else {
        record->state = StageState::Passed;
        record->diagnostic.clear();
    }
    campaign.updatedUnixSeconds = nowUnixSeconds;
    return {};
}

CampaignDiagnostic recordManualVerdict(AcceptanceCampaign& campaign,
                                       CampaignStage stage,
                                       HumanVerdict verdict,
                                       std::string_view note,
                                       std::uint64_t nowUnixSeconds) {
    const auto valid = validateCampaign(campaign, nowUnixSeconds);
    if (!valid.succeeded()) return valid;
    auto* record = findStage(campaign, stage);
    if (record == nullptr || record->state != StageState::AwaitingManualReview ||
        verdict == HumanVerdict::Pending || note.empty() || note.size() > kMaximumNoteBytes) {
        return fail(CampaignCode::ManualVerdictRequired,
                    "bounded explicit manual pass/fail verdict is required");
    }
    if (record->evidence.empty() ||
        std::any_of(record->evidence.begin(), record->evidence.end(),
                    [](const ChildEvidence& item) { return !item.automatedPassed; })) {
        return fail(CampaignCode::ManualVerdictRequired,
                    "manual approval cannot override missing or failed automated evidence");
    }
    if (verdict == HumanVerdict::Pass && stageRequiresPhysicalEvidence(stage) &&
        std::none_of(record->evidence.begin(), record->evidence.end(),
                     [](const ChildEvidence& item) {
                         return item.origin == EvidenceOrigin::Physical;
                     })) {
        return fail(CampaignCode::PhysicalEvidenceRequired,
                    "manual approval cannot promote non-physical evidence");
    }
    for (auto& evidence : record->evidence) evidence.humanVerdict = verdict;
    record->state = verdict == HumanVerdict::Pass ? StageState::Passed : StageState::Failed;
    record->completedUnixSeconds = nowUnixSeconds;
    record->diagnostic = std::string(note);
    campaign.updatedUnixSeconds = nowUnixSeconds;
    return {};
}

CampaignDiagnostic recoverInterrupted(AcceptanceCampaign& campaign,
                                      std::uint64_t nowUnixSeconds) {
    const auto valid = validateCampaign(campaign, nowUnixSeconds);
    if (!valid.succeeded()) return valid;
    bool recovered = false;
    for (auto& stage : campaign.stages) {
        if (stage.state == StageState::Running) {
            stage.state = StageState::RecoveryRequired;
            stage.completedUnixSeconds = nowUnixSeconds;
            stage.diagnostic = "campaign was interrupted while the stage was running";
            recovered = true;
        }
    }
    if (!recovered) return fail(CampaignCode::InvalidTransition, "no interrupted stage exists");
    campaign.updatedUnixSeconds = nowUnixSeconds;
    return {};
}

std::string encodeCampaignJson(const AcceptanceCampaign& campaign) {
    std::string out;
    out.reserve(16384u);
    out += "{\"schema_version\":" + std::to_string(campaign.schemaVersion) + ",\"identity\":{";
    out += "\"campaign_id\":"; appendEscaped(out, campaign.identity.campaignId);
    out += ",\"rc_commit_sha\":"; appendEscaped(out, campaign.identity.rcCommitSha);
    out += ",\"release_artifact_sha256\":"; appendEscaped(out, campaign.identity.releaseArtifactSha256);
    out += ",\"release_artifact_name\":"; appendEscaped(out, campaign.identity.releaseArtifactName);
    out += ",\"release_revision\":" + std::to_string(campaign.identity.releaseRevision);
    out += ",\"architecture\":"; appendEscaped(out, campaign.identity.architecture);
    out += ",\"profile_sha256\":"; appendEscaped(out, campaign.identity.profileSha256);
    out += ",\"install_state_sha256\":"; appendEscaped(out, campaign.identity.installStateSha256);
    out += ",\"windows_build\":"; appendEscaped(out, campaign.identity.windowsBuild);
    out += ",\"topology_fingerprint_sha256\":"; appendEscaped(out, campaign.identity.topologyFingerprintSha256);
    out += ",\"seat_ids\":[" + std::to_string(campaign.identity.seatIds[0]) + ',' +
           std::to_string(campaign.identity.seatIds[1]) + "],\"scenario_identity\":";
    appendEscaped(out, campaign.identity.scenarioIdentity);
    out += ",\"session_run_id\":"; appendEscaped(out, campaign.identity.sessionRunId);
    out += "},\"created_unix\":" + std::to_string(campaign.createdUnixSeconds) +
           ",\"updated_unix\":" + std::to_string(campaign.updatedUnixSeconds) + ",\"stages\":[";
    for (std::size_t index = 0u; index < campaign.stages.size(); ++index) {
        const auto& stage = campaign.stages[index];
        if (index != 0u) out.push_back(',');
        out += "{\"stage\":"; appendEnum(out, stage.stage);
        out += ",\"state\":"; appendEnum(out, stage.state);
        out += ",\"attempt\":" + std::to_string(stage.attempt) +
               ",\"started_unix\":" + std::to_string(stage.startedUnixSeconds) +
               ",\"completed_unix\":" + std::to_string(stage.completedUnixSeconds) +
               ",\"evidence\":[";
        for (std::size_t evidenceIndex = 0u; evidenceIndex < stage.evidence.size(); ++evidenceIndex) {
            const auto& evidence = stage.evidence[evidenceIndex];
            if (evidenceIndex != 0u) out.push_back(',');
            out += "{\"schema_version\":" + std::to_string(evidence.schemaVersion) +
                   ",\"evidence_id\":"; appendEscaped(out, evidence.evidenceId);
            out += ",\"stage\":"; appendEnum(out, evidence.stage);
            out += ",\"origin\":"; appendEnum(out, evidence.origin);
            out += ",\"created_unix\":" + std::to_string(evidence.createdUnixSeconds) +
                   ",\"content_sha256\":"; appendEscaped(out, evidence.contentSha256);
            out += ",\"evidence_artifact_name\":"; appendEscaped(out, evidence.evidenceArtifactName);
            out += ",\"test_name\":"; appendEscaped(out, evidence.testName);
            out += ",\"rc_commit_sha\":"; appendEscaped(out, evidence.rcCommitSha);
            out += ",\"release_artifact_sha256\":"; appendEscaped(out, evidence.releaseArtifactSha256);
            out += ",\"release_revision\":" + std::to_string(evidence.releaseRevision);
            out += ",\"architecture\":"; appendEscaped(out, evidence.architecture);
            out += ",\"profile_sha256\":"; appendEscaped(out, evidence.profileSha256);
            out += ",\"install_state_sha256\":"; appendEscaped(out, evidence.installStateSha256);
            out += ",\"scenario_identity\":"; appendEscaped(out, evidence.scenarioIdentity);
            out += ",\"automated_passed\":";
            out += evidence.automatedPassed ? "true" : "false";
            out += ",\"human_verdict\":"; appendEnum(out, evidence.humanVerdict);
            out += ",\"note\":"; appendEscaped(out, evidence.note);
            out += ",\"campaign_schema_version\":" + std::to_string(evidence.campaignSchemaVersion);
            out += ",\"campaign_id\":"; appendEscaped(out, evidence.campaignId);
            out += ",\"session_run_id\":"; appendEscaped(out, evidence.sessionRunId);
            out += ",\"release_artifact_name\":"; appendEscaped(out, evidence.releaseArtifactName);
            out += ",\"windows_build\":"; appendEscaped(out, evidence.windowsBuild);
            out += ",\"topology_fingerprint_sha256\":";
            appendEscaped(out, evidence.topologyFingerprintSha256);
            out += ",\"evidence_class\":"; appendEscaped(out, evidenceClassName(evidence.evidenceClass));
            out.push_back('}');
        }
        out += "],\"diagnostic\":"; appendEscaped(out, stage.diagnostic);
        out.push_back('}');
    }
    out += "]}";
    return out;
}

CampaignDiagnostic decodeCampaignJson(std::string_view text, AcceptanceCampaign& output,
                                      std::uint64_t nowUnixSeconds) {
    if (text.empty() || text.size() > kMaximumCampaignBytes) {
        return fail(CampaignCode::DocumentTooLarge, "campaign JSON is empty or oversized");
    }
    try {
        const auto parsed = internal::json::parse(text, {12u, 4096u});
        const auto* root = object(parsed);
        if (root == nullptr || !exactKeys(*root, {"schema_version", "identity", "created_unix",
            "updated_unix", "stages"})) throw std::runtime_error("invalid root fields");
        const auto schema = unsignedValue(*root, "schema_version");
        const auto created = unsignedValue(*root, "created_unix");
        const auto updated = unsignedValue(*root, "updated_unix");
        const auto identityIt = root->find("identity");
        const auto stagesIt = root->find("stages");
        if (!schema || *schema > std::numeric_limits<std::uint32_t>::max() || !created || !updated ||
            identityIt == root->end() || stagesIt == root->end()) throw std::runtime_error("invalid root values");
        const auto* identity = object(identityIt->second);
        const auto* stages = array(stagesIt->second);
        if (identity == nullptr || stages == nullptr || !exactKeys(*identity, {"campaign_id",
            "rc_commit_sha", "release_artifact_sha256", "release_artifact_name",
            "release_revision", "architecture", "profile_sha256", "install_state_sha256",
            "windows_build", "topology_fingerprint_sha256", "seat_ids", "scenario_identity",
            "session_run_id"})) {
            throw std::runtime_error("invalid identity");
        }
        const auto* campaignId = stringValue(*identity, "campaign_id");
        const auto* commit = stringValue(*identity, "rc_commit_sha");
        const auto* artifact = stringValue(*identity, "release_artifact_sha256");
        const auto* artifactName = stringValue(*identity, "release_artifact_name");
        const auto revision = unsignedValue(*identity, "release_revision");
        const auto* architecture = stringValue(*identity, "architecture");
        const auto* profile = stringValue(*identity, "profile_sha256");
        const auto* installState = stringValue(*identity, "install_state_sha256");
        const auto* build = stringValue(*identity, "windows_build");
        const auto* topology = stringValue(*identity, "topology_fingerprint_sha256");
        const auto* scenario = stringValue(*identity, "scenario_identity");
        const auto* sessionRun = stringValue(*identity, "session_run_id");
        const auto seatIt = identity->find("seat_ids");
        const auto* seats = seatIt == identity->end() ? nullptr : array(seatIt->second);
        if (!campaignId || !commit || !artifact || !artifactName || !revision || !architecture ||
            !profile || !installState || !build || !topology || !scenario || !sessionRun ||
            seats == nullptr || seats->size() != 2u) throw std::runtime_error("invalid identity values");
        const auto seatObject = [](const JsonValue& value) -> std::optional<std::uint32_t> {
            const auto* number = std::get_if<internal::json::Number>(&value.value);
            if (!number || number->text.empty() || number->text.front() == '-') return std::nullopt;
            std::uint32_t result = 0u;
            const auto parsedNumber = std::from_chars(number->text.data(),
                number->text.data() + number->text.size(), result);
            return parsedNumber.ec == std::errc{} && parsedNumber.ptr ==
                number->text.data() + number->text.size() ? std::optional(result) : std::nullopt;
        };
        const auto seat1 = seatObject((*seats)[0]);
        const auto seat2 = seatObject((*seats)[1]);
        if (!seat1 || !seat2) throw std::runtime_error("invalid seat ids");

        AcceptanceCampaign candidate;
        candidate.schemaVersion = static_cast<std::uint32_t>(*schema);
        candidate.identity = {*campaignId, *commit, *artifact, *artifactName, *revision,
                              *architecture, *profile, *installState, *build, *topology,
                              {*seat1, *seat2}, *scenario, *sessionRun};
        candidate.createdUnixSeconds = *created;
        candidate.updatedUnixSeconds = *updated;
        candidate.stages.reserve(stages->size());
        for (const auto& stageValue : *stages) {
            const auto* stageObject = object(stageValue);
            if (!stageObject || !exactKeys(*stageObject, {"stage", "state", "attempt",
                "started_unix", "completed_unix", "evidence", "diagnostic"})) {
                throw std::runtime_error("invalid stage fields");
            }
            const auto* stageName = stringValue(*stageObject, "stage");
            const auto* stateName = stringValue(*stageObject, "state");
            const auto attempt = unsignedValue(*stageObject, "attempt");
            const auto started = unsignedValue(*stageObject, "started_unix");
            const auto completed = unsignedValue(*stageObject, "completed_unix");
            const auto* diagnostic = stringValue(*stageObject, "diagnostic");
            const auto evidenceIt = stageObject->find("evidence");
            const auto* evidenceArray = evidenceIt == stageObject->end() ? nullptr : array(evidenceIt->second);
            const auto parsedStage = stageName ? parseEnum<CampaignStage>(*stageName) : std::nullopt;
            const auto parsedState = stateName ? parseEnum<StageState>(*stateName) : std::nullopt;
            if (!parsedStage || !parsedState || !attempt || *attempt > std::numeric_limits<std::uint32_t>::max() ||
                !started || !completed || !diagnostic || !evidenceArray) throw std::runtime_error("invalid stage values");
            StageRecord record;
            record.stage = *parsedStage;
            record.state = *parsedState;
            record.attempt = static_cast<std::uint32_t>(*attempt);
            record.startedUnixSeconds = *started;
            record.completedUnixSeconds = *completed;
            record.diagnostic = *diagnostic;
            record.evidence.reserve(evidenceArray->size());
            for (const auto& evidenceValue : *evidenceArray) {
                ChildEvidence evidence;
                if (!decodeEvidence(evidenceValue, evidence)) throw std::runtime_error("invalid evidence");
                record.evidence.push_back(std::move(evidence));
            }
            candidate.stages.push_back(std::move(record));
        }
        const auto valid = validateCampaign(candidate, nowUnixSeconds);
        if (!valid.succeeded()) return valid;
        output = std::move(candidate);
        return {};
    } catch (const std::exception& error) {
        return fail(CampaignCode::DecodeFailed,
                    std::string("strict campaign decode failed: ") + error.what());
    }
}

CampaignDiagnostic CampaignStore::write(const AcceptanceCampaign& campaign) const {
    const auto bytes = encodeCampaignJson(campaign);
    if (bytes.empty() || bytes.size() > kMaximumCampaignBytes || path_.empty()) {
        return fail(CampaignCode::DocumentTooLarge, "encoded campaign is empty, oversized, or has no path");
    }
    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) return fail(CampaignCode::StorageFailed, "campaign directory could not be created");
    const auto staged = std::filesystem::path(path_.string() + ".new");
    const auto backup = std::filesystem::path(path_.string() + ".bak");
    if (!removeIfPresentVerified(staged)) {
        return fail(CampaignCode::StorageFailed,
                    "stale staged campaign could not be removed safely");
    }
    {
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.put('\n');
        output.flush();
        if (!output) return failAfterStaging("staged campaign write failed", staged);
    }
    if (!flushFile(staged)) return failAfterStaging("staged campaign flush failed", staged);
    if (!removeIfPresentVerified(backup)) {
        return failAfterStaging("campaign backup cleanup failed", staged);
    }
    error.clear();
    if (std::filesystem::exists(path_, error)) {
        error.clear();
        std::filesystem::rename(path_, backup, error);
        if (error) return failAfterStaging("campaign backup rotation failed", staged);
    }
    error.clear();
    std::filesystem::rename(staged, path_, error);
    if (error) {
        std::error_code restoreError;
        if (std::filesystem::exists(backup, restoreError)) {
            restoreError.clear();
            std::filesystem::rename(backup, path_, restoreError);
        }
        return failAfterStaging("campaign atomic replacement failed", staged);
    }
    if (!removeIfPresentVerified(staged)) {
        return fail(CampaignCode::StorageFailed,
                    "published campaign staging cleanup could not be verified");
    }
    return {};
}

CampaignDiagnostic CampaignStore::load(AcceptanceCampaign& output,
                                        std::uint64_t nowUnixSeconds) const {
    std::string bytes;
    auto read = readBounded(path_, bytes);
    if (read.succeeded()) {
        auto decoded = decodeCampaignJson(bytes, output, nowUnixSeconds);
        if (decoded.succeeded()) return decoded;
    }
    const auto backup = std::filesystem::path(path_.string() + ".bak");
    std::string backupBytes;
    const auto backupRead = readBounded(backup, backupBytes);
    if (!backupRead.succeeded()) {
        return fail(CampaignCode::NoRecoverableDocument,
                    "neither current nor backup campaign is valid and bounded");
    }
    AcceptanceCampaign candidate;
    const auto decoded = decodeCampaignJson(backupBytes, candidate, nowUnixSeconds);
    if (!decoded.succeeded()) return fail(CampaignCode::NoRecoverableDocument,
                                          "campaign backup also failed strict validation");
    output = std::move(candidate);
    return {};
}

EvidenceClass evidenceClassForStage(CampaignStage stage) noexcept {
    switch (stage) {
    case CampaignStage::Preflight:
    case CampaignStage::Offline:
        return EvidenceClass::Controlled;
    case CampaignStage::Phase3Physical:
    case CampaignStage::DisplayReconnect:
        return EvidenceClass::Physical;
    case CampaignStage::DifferentGames:
    case CampaignStage::SameTitle:
    case CampaignStage::SeatIndependence:
    case CampaignStage::ReturnToWindows:
    case CampaignStage::PerformanceSoak:
        return EvidenceClass::RealGame;
    case CampaignStage::InstallRepairUninstall:
    case CampaignStage::RebootStartup:
    case CampaignStage::UpdateRollback:
        return EvidenceClass::CleanMachineInstall;
    case CampaignStage::FaultRecovery:
        return EvidenceClass::Manual;
    }
    return EvidenceClass::Unspecified;
}

bool stageRequiresPhysicalEvidence(CampaignStage stage) noexcept {
    const auto evidenceClass = evidenceClassForStage(stage);
    return evidenceClass == EvidenceClass::Physical ||
           evidenceClass == EvidenceClass::Manual ||
           evidenceClass == EvidenceClass::RealGame ||
           evidenceClass == EvidenceClass::CleanMachineInstall ||
           evidenceClass == EvidenceClass::SigningDeployment;
}

bool stageRequiresManualReview(CampaignStage stage) noexcept {
    return stage != CampaignStage::Preflight && stage != CampaignStage::Offline;
}

std::string_view campaignStageName(CampaignStage value) noexcept {
    switch (value) {
    case CampaignStage::Preflight: return "Preflight";
    case CampaignStage::Phase3Physical: return "Phase3Physical";
    case CampaignStage::DisplayReconnect: return "DisplayReconnect";
    case CampaignStage::DifferentGames: return "DifferentGames";
    case CampaignStage::SameTitle: return "SameTitle";
    case CampaignStage::SeatIndependence: return "SeatIndependence";
    case CampaignStage::ReturnToWindows: return "ReturnToWindows";
    case CampaignStage::InstallRepairUninstall: return "InstallRepairUninstall";
    case CampaignStage::RebootStartup: return "RebootStartup";
    case CampaignStage::UpdateRollback: return "UpdateRollback";
    case CampaignStage::Offline: return "Offline";
    case CampaignStage::FaultRecovery: return "FaultRecovery";
    case CampaignStage::PerformanceSoak: return "PerformanceSoak";
    }
    return "Unknown";
}

std::string_view stageStateName(StageState value) noexcept {
    switch (value) {
    case StageState::Pending: return "Pending";
    case StageState::Running: return "Running";
    case StageState::AwaitingManualReview: return "AwaitingManualReview";
    case StageState::Passed: return "Passed";
    case StageState::Failed: return "Failed";
    case StageState::RecoveryRequired: return "RecoveryRequired";
    }
    return "Unknown";
}

std::string_view evidenceOriginName(EvidenceOrigin value) noexcept {
    switch (value) {
    case EvidenceOrigin::Synthetic: return "Synthetic";
    case EvidenceOrigin::ControlledProcess: return "ControlledProcess";
    case EvidenceOrigin::Physical: return "Physical";
    }
    return "Unknown";
}

std::string_view evidenceClassName(EvidenceClass value) noexcept {
    switch (value) {
    case EvidenceClass::Synthetic: return "Synthetic";
    case EvidenceClass::Controlled: return "Controlled";
    case EvidenceClass::Physical: return "Physical";
    case EvidenceClass::Manual: return "Manual";
    case EvidenceClass::RealGame: return "RealGame";
    case EvidenceClass::CleanMachineInstall: return "CleanMachineInstall";
    case EvidenceClass::SigningDeployment: return "SigningDeployment";
    case EvidenceClass::Unspecified: return "Unspecified";
    }
    return "Unknown";
}

std::string_view humanVerdictName(HumanVerdict value) noexcept {
    switch (value) {
    case HumanVerdict::Pending: return "Pending";
    case HumanVerdict::Pass: return "Pass";
    case HumanVerdict::Fail: return "Fail";
    }
    return "Unknown";
}

std::string_view campaignCodeName(CampaignCode value) noexcept {
    switch (value) {
    case CampaignCode::Success: return "Success";
    case CampaignCode::InvalidIdentity: return "InvalidIdentity";
    case CampaignCode::InvalidCampaign: return "InvalidCampaign";
    case CampaignCode::InvalidStage: return "InvalidStage";
    case CampaignCode::InvalidTransition: return "InvalidTransition";
    case CampaignCode::EvidenceMalformed: return "EvidenceMalformed";
    case CampaignCode::EvidenceStale: return "EvidenceStale";
    case CampaignCode::EvidenceIdentityMismatch: return "EvidenceIdentityMismatch";
    case CampaignCode::EvidenceDuplicate: return "EvidenceDuplicate";
    case CampaignCode::EvidenceClassMismatch: return "EvidenceClassMismatch";
    case CampaignCode::PhysicalEvidenceRequired: return "PhysicalEvidenceRequired";
    case CampaignCode::ManualVerdictRequired: return "ManualVerdictRequired";
    case CampaignCode::DocumentTooLarge: return "DocumentTooLarge";
    case CampaignCode::DecodeFailed: return "DecodeFailed";
    case CampaignCode::StorageFailed: return "StorageFailed";
    case CampaignCode::NoRecoverableDocument: return "NoRecoverableDocument";
    }
    return "Unknown";
}

} // namespace hydra::acceptance
