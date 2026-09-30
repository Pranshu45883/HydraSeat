#include "hydra/support_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

namespace hydra::support {
namespace {

SupportDiagnostic fail(SupportCode code, std::string message) {
    return {code, std::move(message)};
}

bool validId(std::string_view value, std::size_t maximum = 128u) noexcept {
    if (value.empty() || value.size() > maximum) return false;
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

bool validWindowsBuildClass(std::string_view value) noexcept {
    if (!validId(value, 64u)) return false;
    return value.starts_with("win10-") || value.starts_with("win11-");
}

bool validArchitecture(std::string_view value) noexcept {
    return value == "x64" || value == "x86" || value == "arm64";
}

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t fnv1a(std::string_view value, std::uint64_t seed) noexcept {
    std::uint64_t hash = seed;
    for (const char raw : value) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
        hash *= kFnvPrime;
    }
    return hash;
}

std::string hex64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::string payloadIdentity(std::string_view exactJson) {
    const auto first = fnv1a(exactJson, kFnvOffset);
    const auto second = fnv1a(exactJson, kFnvOffset ^ 0x9e3779b97f4a7c15ull);
    return "support-v1-" + hex64(first) + hex64(second);
}

bool validJournalPhase(recovery::CrashJournalPhase value) noexcept {
    return value == recovery::CrashJournalPhase::Preparing ||
           value == recovery::CrashJournalPhase::Applying ||
           value == recovery::CrashJournalPhase::Active ||
           value == recovery::CrashJournalPhase::RollingBack ||
           value == recovery::CrashJournalPhase::Clean ||
           value == recovery::CrashJournalPhase::RecoveryRequired;
}

bool validJournalResult(recovery::CrashJournalFinalResult value) noexcept {
    return value == recovery::CrashJournalFinalResult::None ||
           value == recovery::CrashJournalFinalResult::Clean ||
           value == recovery::CrashJournalFinalResult::Failed ||
           value == recovery::CrashJournalFinalResult::RecoveryRequired;
}

bool validMetrics(const metrics::SessionMetricsReport& report) noexcept {
    if (report.schemaVersion != metrics::kSessionMetricsSchemaVersion ||
        report.planFingerprint == 0u || report.seats.empty() ||
        report.seats.size() > metrics::kMaximumSessionMetricSeats) {
        return false;
    }
    std::set<SeatId> seats;
    for (const auto& seat : report.seats) {
        if (seat.seatId == 0u || !seats.insert(seat.seatId).second) return false;
    }
    return true;
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

std::string_view phaseName(recovery::CrashJournalPhase value) noexcept {
    switch (value) {
        case recovery::CrashJournalPhase::Preparing: return "Preparing";
        case recovery::CrashJournalPhase::Applying: return "Applying";
        case recovery::CrashJournalPhase::Active: return "Active";
        case recovery::CrashJournalPhase::RollingBack: return "RollingBack";
        case recovery::CrashJournalPhase::Clean: return "Clean";
        case recovery::CrashJournalPhase::RecoveryRequired: return "RecoveryRequired";
    }
    return "Unknown";
}

std::string_view finalName(recovery::CrashJournalFinalResult value) noexcept {
    switch (value) {
        case recovery::CrashJournalFinalResult::None: return "None";
        case recovery::CrashJournalFinalResult::Clean: return "Clean";
        case recovery::CrashJournalFinalResult::Failed: return "Failed";
        case recovery::CrashJournalFinalResult::RecoveryRequired: return "RecoveryRequired";
    }
    return "Unknown";
}

SupportDiagnostic validateBundle(const SupportBundle& bundle) {
    if (bundle.schemaVersion != kSupportBundleSchemaVersion ||
        !validId(bundle.hydraSeatVersion) || !validId(bundle.hydraSeatBuild) ||
        !validWindowsBuildClass(bundle.windowsBuildClass) ||
        !validArchitecture(bundle.architecture)) {
        return fail(SupportCode::InvalidEnvironment,
                    "support bundle environment must use bounded public release/build classes");
    }
    if (!bundle.credentialsExcluded || !bundle.playerNamesExcluded ||
        !bundle.personalPathsExcluded || !bundle.rawTypedTextExcluded ||
        !bundle.unrelatedProcessDataExcluded || !bundle.deviceSerialsExcluded) {
        return fail(SupportCode::RedactionRequired,
                    "support bundle must retain the complete mandatory redaction contract");
    }
    if (bundle.sessionMetrics && !validMetrics(*bundle.sessionMetrics)) {
        return fail(SupportCode::InvalidMetrics,
                    "support bundle session metrics are invalid or unbounded");
    }
    if (bundle.recovery &&
        (!validJournalPhase(bundle.recovery->phase) ||
         !validJournalResult(bundle.recovery->finalResult))) {
        return fail(SupportCode::InvalidRecovery,
                    "support bundle recovery summary contains invalid enum state");
    }
    if (bundle.compatibility) {
        const auto compatibility = compat::validateCompatibilityResult(*bundle.compatibility);
        if (!compatibility.succeeded()) {
            return fail(SupportCode::InvalidCompatibility,
                        "support bundle compatibility result is invalid: " + compatibility.message);
        }
    }
    if (bundle.events.size() > kMaximumSupportEvents) {
        return fail(SupportCode::TooManyEvents, "support bundle has too many event codes");
    }
    std::set<std::tuple<std::string, SeatId, std::uint64_t>> seen;
    for (const auto& event : bundle.events) {
        if (!validId(event.eventCode) || event.seatId > 2u || event.generation == 0u) {
            return fail(SupportCode::InvalidEvent,
                        "support event must be a bounded code plus optional Seat 1/2 and generation");
        }
        if (!seen.emplace(event.eventCode, event.seatId, event.generation).second) {
            return fail(SupportCode::InvalidEvent, "duplicate support event tuple");
        }
    }
    return {};
}

} // namespace

bool SupportBundle::operator==(const SupportBundle& other) const {
    if (schemaVersion != other.schemaVersion || hydraSeatVersion != other.hydraSeatVersion ||
        hydraSeatBuild != other.hydraSeatBuild || windowsBuildClass != other.windowsBuildClass ||
        architecture != other.architecture || recovery != other.recovery ||
        compatibility != other.compatibility || events != other.events ||
        credentialsExcluded != other.credentialsExcluded ||
        playerNamesExcluded != other.playerNamesExcluded ||
        personalPathsExcluded != other.personalPathsExcluded ||
        rawTypedTextExcluded != other.rawTypedTextExcluded ||
        unrelatedProcessDataExcluded != other.unrelatedProcessDataExcluded ||
        deviceSerialsExcluded != other.deviceSerialsExcluded ||
        sessionMetrics.has_value() != other.sessionMetrics.has_value()) {
        return false;
    }
    if (!sessionMetrics) return true;
    return metrics::encodeSessionMetricsReportJson(*sessionMetrics) ==
           metrics::encodeSessionMetricsReportJson(*other.sessionMetrics);
}

SupportDiagnostic buildSupportBundle(const SupportBundleInput& input, SupportBundle& output) {
    SupportBundle candidate;
    candidate.hydraSeatVersion = input.hydraSeatVersion;
    candidate.hydraSeatBuild = input.hydraSeatBuild;
    candidate.windowsBuildClass = input.windowsBuildClass;
    candidate.architecture = input.architecture;
    if (input.sessionMetrics != nullptr) candidate.sessionMetrics = *input.sessionMetrics;
    if (input.crashJournal != nullptr) {
        if (input.crashJournal->records.size() > recovery::kCrashJournalMaxRecords ||
            input.crashJournal->snapshots.size() > recovery::kCrashJournalMaxSnapshots) {
            return fail(SupportCode::InvalidRecovery,
                        "crash journal exceeds declared recovery bounds");
        }
        candidate.recovery = RecoverySummary{
            input.crashJournal->phase,
            input.crashJournal->finalResult,
            input.crashJournal->runtimeGeneration,
            static_cast<std::uint64_t>(input.crashJournal->records.size()),
            static_cast<std::uint64_t>(input.crashJournal->snapshots.size()),
        };
    }
    if (input.compatibility != nullptr) candidate.compatibility = *input.compatibility;
    candidate.events = input.events;
    std::sort(candidate.events.begin(), candidate.events.end(), [](const auto& left, const auto& right) {
        if (left.generation != right.generation) return left.generation < right.generation;
        if (left.seatId != right.seatId) return left.seatId < right.seatId;
        return left.eventCode < right.eventCode;
    });
    const auto validation = validateBundle(candidate);
    if (!validation.succeeded()) return validation;
    output = std::move(candidate);
    return {};
}

SupportDiagnostic encodeSupportBundleJson(const SupportBundle& bundle, std::string& output) {
    const auto validation = validateBundle(bundle);
    if (!validation.succeeded()) return validation;
    try {
        std::string json;
        json.reserve(4096u);
        json += "{\"schema_version\":1,\"environment\":{\"hydraseat_version\":";
        appendEscaped(json, bundle.hydraSeatVersion);
        json += ",\"hydraseat_build\":";
        appendEscaped(json, bundle.hydraSeatBuild);
        json += ",\"windows_build_class\":";
        appendEscaped(json, bundle.windowsBuildClass);
        json += ",\"architecture\":";
        appendEscaped(json, bundle.architecture);
        json += "},\"session_metrics\":";
        if (bundle.sessionMetrics) json += metrics::encodeSessionMetricsReportJson(*bundle.sessionMetrics);
        else json += "null";
        json += ",\"recovery\":";
        if (bundle.recovery) {
            json += "{\"phase\":";
            appendEscaped(json, phaseName(bundle.recovery->phase));
            json += ",\"final_result\":";
            appendEscaped(json, finalName(bundle.recovery->finalResult));
            json += ",\"runtime_generation\":" + std::to_string(bundle.recovery->runtimeGeneration) +
                    ",\"record_count\":" + std::to_string(bundle.recovery->recordCount) +
                    ",\"snapshot_count\":" + std::to_string(bundle.recovery->snapshotCount) + "}";
        } else {
            json += "null";
        }
        json += ",\"compatibility\":";
        if (bundle.compatibility) {
            std::string compatibilityJson;
            const auto encoded = compat::encodeCompatibilityResultJson(
                *bundle.compatibility, compatibilityJson);
            if (!encoded.succeeded()) {
                return fail(SupportCode::InvalidCompatibility, encoded.message);
            }
            json += compatibilityJson;
        } else {
            json += "null";
        }
        json += ",\"events\":[";
        for (std::size_t index = 0u; index < bundle.events.size(); ++index) {
            if (index != 0u) json.push_back(',');
            const auto& event = bundle.events[index];
            json += "{\"code\":";
            appendEscaped(json, event.eventCode);
            json += ",\"seat_id\":" + std::to_string(event.seatId) +
                    ",\"generation\":" + std::to_string(event.generation) + "}";
        }
        json += "],\"redaction\":{\"credentials_excluded\":true,\"player_names_excluded\":true,"
                "\"personal_paths_excluded\":true,\"raw_typed_text_excluded\":true,"
                "\"unrelated_process_data_excluded\":true,\"device_serials_excluded\":true}}";
        if (json.size() > kMaximumSupportBundleBytes) {
            return fail(SupportCode::TooLarge, "support bundle exceeds the bounded maximum");
        }
        output = std::move(json);
        return {};
    } catch (...) {
        return fail(SupportCode::TooLarge, "support bundle encoding allocation failed");
    }
}

std::string buildSupportBundlePreview(const SupportBundle& bundle) {
    const auto validation = validateBundle(bundle);
    if (!validation.succeeded()) return "Invalid support bundle: " + validation.message;
    std::ostringstream preview;
    preview << "HydraSeat support bundle v" << bundle.schemaVersion << '\n'
            << "Environment: HydraSeat " << bundle.hydraSeatVersion << " (" << bundle.hydraSeatBuild
            << "), Windows " << bundle.windowsBuildClass << ", " << bundle.architecture << '\n'
            << "Session metrics: " << (bundle.sessionMetrics ? "included (privacy-safe aggregate)" : "not included") << '\n'
            << "Recovery summary: " << (bundle.recovery ? "included (phase/counts only)" : "not included") << '\n'
            << "Compatibility result: " << (bundle.compatibility ? "included (public schema)" : "not included") << '\n'
            << "Event codes: " << bundle.events.size() << '\n'
            << "Redaction: credentials, Player names, personal paths, raw typed text, unrelated process data, and device serials excluded\n";
    return preview.str();
}

SupportDiagnostic SupportExportSession::prepare(const SupportBundle& bundle,
                                                SupportExportPreview& preview) {
    prepared_ = false;
    approved_ = false;
    exactJson_.clear();
    payloadIdentity_.clear();
    preparedGeneration_ = 0u;
    if (nextGeneration_ == 0u) {
        return fail(SupportCode::StaleApproval,
                    "support preview generation is exhausted; restart the export flow");
    }

    std::string exactJson;
    const auto encoded = encodeSupportBundleJson(bundle, exactJson);
    if (!encoded.succeeded()) return encoded;

    SupportExportPreview candidate;
    candidate.humanSummary = buildSupportBundlePreview(bundle);
    candidate.exactJson = exactJson;
    candidate.payloadIdentity = payloadIdentity(exactJson);
    candidate.generation = nextGeneration_++;
    exactJson_ = std::move(exactJson);
    payloadIdentity_ = candidate.payloadIdentity;
    preparedGeneration_ = candidate.generation;
    prepared_ = true;
    preview = std::move(candidate);
    return {};
}

SupportDiagnostic SupportExportSession::approve(const SupportExportPreview& approvedPreview) {
    if (!prepared_) {
        return fail(SupportCode::PreviewRequired,
                    "support bundle must be previewed before export approval");
    }
    if (approvedPreview.generation == 0u ||
        approvedPreview.generation != preparedGeneration_) {
        approved_ = false;
        return fail(SupportCode::StaleApproval,
                    "support approval refers to a stale preview generation");
    }
    if (approvedPreview.canonicalizationVersion != kSupportBundleCanonicalizationVersion ||
        approvedPreview.payloadIdentity != payloadIdentity_ ||
        approvedPreview.exactJson != exactJson_) {
        approved_ = false;
        return fail(SupportCode::PayloadIdentityMismatch,
                    "approved support preview identity or bytes do not match the export payload");
    }
    approved_ = true;
    return {};
}

SupportDiagnostic SupportExportSession::exportApproved(std::string& output) const {
    if (!prepared_ || !approved_) {
        return fail(SupportCode::PreviewRequired,
                    "support bundle export requires explicit approval of the exact preview");
    }
    output = exactJson_;
    return {};
}

std::string_view supportCodeName(SupportCode code) noexcept {
    switch (code) {
        case SupportCode::Success: return "Success";
        case SupportCode::InvalidEnvironment: return "InvalidEnvironment";
        case SupportCode::InvalidMetrics: return "InvalidMetrics";
        case SupportCode::InvalidRecovery: return "InvalidRecovery";
        case SupportCode::InvalidCompatibility: return "InvalidCompatibility";
        case SupportCode::InvalidEvent: return "InvalidEvent";
        case SupportCode::TooManyEvents: return "TooManyEvents";
        case SupportCode::RedactionRequired: return "RedactionRequired";
        case SupportCode::TooLarge: return "TooLarge";
        case SupportCode::PreviewRequired: return "PreviewRequired";
        case SupportCode::PreviewMismatch: return "PreviewMismatch";
        case SupportCode::StaleApproval: return "StaleApproval";
        case SupportCode::PayloadIdentityMismatch: return "PayloadIdentityMismatch";
    }
    return "Unknown";
}

} // namespace hydra::support
