#pragma once

#include "hydra/compatibility_result.hpp"
#include "hydra/crash_journal.hpp"
#include "hydra/session_metrics.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::support {

inline constexpr std::uint32_t kSupportBundleSchemaVersion = 1u;
inline constexpr std::uint32_t kSupportBundleCanonicalizationVersion = 1u;
inline constexpr std::size_t kMaximumSupportEvents = 256u;
inline constexpr std::size_t kMaximumSupportBundleBytes = 512u * 1024u;

struct RecoverySummary {
    recovery::CrashJournalPhase phase{recovery::CrashJournalPhase::Preparing};
    recovery::CrashJournalFinalResult finalResult{recovery::CrashJournalFinalResult::None};
    std::uint64_t runtimeGeneration{0};
    std::uint64_t recordCount{0};
    std::uint64_t snapshotCount{0};

    bool operator==(const RecoverySummary&) const = default;
};

struct SupportEvent {
    std::string eventCode;
    SeatId seatId{0};
    std::uint64_t generation{0};

    bool operator==(const SupportEvent&) const = default;
};

struct SupportBundle {
    std::uint32_t schemaVersion{kSupportBundleSchemaVersion};
    std::string hydraSeatVersion;
    std::string hydraSeatBuild;
    std::string windowsBuildClass;
    std::string architecture;
    std::optional<metrics::SessionMetricsReport> sessionMetrics;
    std::optional<RecoverySummary> recovery;
    std::optional<compat::CompatibilityResult> compatibility;
    std::vector<SupportEvent> events;
    bool credentialsExcluded{true};
    bool playerNamesExcluded{true};
    bool personalPathsExcluded{true};
    bool rawTypedTextExcluded{true};
    bool unrelatedProcessDataExcluded{true};
    bool deviceSerialsExcluded{true};

    bool operator==(const SupportBundle& other) const;
};

struct SupportBundleInput {
    std::string hydraSeatVersion;
    std::string hydraSeatBuild;
    std::string windowsBuildClass;
    std::string architecture;
    const metrics::SessionMetricsReport* sessionMetrics{nullptr};
    const recovery::CrashJournalState* crashJournal{nullptr};
    const compat::CompatibilityResult* compatibility{nullptr};
    std::vector<SupportEvent> events;
};

enum class SupportCode : std::uint8_t {
    Success = 0,
    InvalidEnvironment,
    InvalidMetrics,
    InvalidRecovery,
    InvalidCompatibility,
    InvalidEvent,
    TooManyEvents,
    RedactionRequired,
    TooLarge,
    PreviewRequired,
    PreviewMismatch,
    StaleApproval,
    PayloadIdentityMismatch,
};

struct SupportDiagnostic {
    SupportCode code{SupportCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == SupportCode::Success; }
};

SupportDiagnostic buildSupportBundle(const SupportBundleInput& input, SupportBundle& output);
SupportDiagnostic encodeSupportBundleJson(const SupportBundle& bundle, std::string& output);
std::string buildSupportBundlePreview(const SupportBundle& bundle);

struct SupportExportPreview {
    std::uint32_t canonicalizationVersion{kSupportBundleCanonicalizationVersion};
    std::string humanSummary;
    std::string exactJson;
    std::string payloadIdentity;
    std::uint64_t generation{0u};

    bool operator==(const SupportExportPreview&) const = default;
};

class SupportExportSession final {
public:
    SupportDiagnostic prepare(const SupportBundle& bundle, SupportExportPreview& preview);
    SupportDiagnostic approve(const SupportExportPreview& approvedPreview);
    SupportDiagnostic exportApproved(std::string& output) const;

    bool hasPreview() const noexcept { return prepared_; }
    bool approved() const noexcept { return approved_; }

private:
    bool prepared_{false};
    bool approved_{false};
    std::string exactJson_;
    std::string payloadIdentity_;
    std::uint64_t nextGeneration_{1u};
    std::uint64_t preparedGeneration_{0u};
};

std::string_view supportCodeName(SupportCode code) noexcept;

} // namespace hydra::support
