#pragma once

#include "hydra/community_submission.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::community {

inline constexpr std::uint32_t kCompatibilityPrivacySettingsSchemaVersion = 1u;
inline constexpr std::uint32_t kCompatibilityLocalHistorySchemaVersion = 1u;
inline constexpr std::uint32_t kCompatibilityCanonicalizationVersion = 1u;
inline constexpr std::size_t kMaximumCompatibilityPrivacySettingsBytes = 1024u;
inline constexpr std::size_t kMaximumRetainedCompatibilityResults = 64u;
inline constexpr std::size_t kMaximumCompatibilityLocalHistoryBytes =
    kMaximumRetainedCompatibilityResults * (compat::kMaximumCompatibilityResultBytes + 1u) + 128u;
inline constexpr std::size_t kDefaultRetainedCompatibilityResults = 32u;

struct CompatibilityPrivacySettings {
    bool communitySharingEnabled{false};
    std::size_t retainedLocalResults{kDefaultRetainedCompatibilityResults};

    bool operator==(const CompatibilityPrivacySettings&) const = default;
};

// Approval binds to both the exact canonical bytes and a one-use preview generation.
// payloadIdentity is the deterministic SubmissionSession identity of exactRedactedJson.
struct CompatibilitySharePreview {
    std::uint32_t canonicalizationVersion{kCompatibilityCanonicalizationVersion};
    std::string exactRedactedJson;
    std::string payloadIdentity;
    std::string submissionId;
    bool protectedExperimental{false};
    std::uint64_t generation{0u};

    bool operator==(const CompatibilitySharePreview&) const = default;
};

// Local export is a separate consent boundary from community submission. The
// exact canonical bytes are frozen at prepare time and public export methods
// return only those already-approved bytes; they never reserialize the result.
struct CompatibilityLocalExportPreview {
    std::uint32_t canonicalizationVersion{kCompatibilityCanonicalizationVersion};
    std::string resultId;
    std::string exactRedactedJson;
    std::string payloadIdentity;
    std::uint64_t generation{0u};

    bool operator==(const CompatibilityLocalExportPreview&) const = default;
};

enum class CompatibilityShareState : std::uint8_t {
    TestNotRun = 0,
    LocalResultAvailable = 1,
    PreviewReady = 2,
    UserDeclined = 3,
    SubmitPending = 4,
    SubmitSucceeded = 5,
    SubmitFailed = 6,
    Superseded = 7,
};

struct CompatibilityShareEntry {
    compat::CompatibilityResult result;
    CompatibilityShareState state{CompatibilityShareState::LocalResultAvailable};
    std::optional<CompatibilitySharePreview> preview;
    std::optional<SubmissionReceipt> receipt;
    std::optional<SubmissionCode> lastSubmissionCode;
    std::optional<std::string> supersededByResultId;

    bool protectedExperimental() const noexcept { return result.protectedExperimental; }
};

enum class ShareModelCode : std::uint8_t {
    Success = 0,
    InvalidResult,
    NoActiveResult,
    InvalidState,
    PreviewRequired,
    SubmissionFailed,
    InvalidPrivacySettings,
    SharingDisabled,
    ResultNotFound,
    ExportFailed,
    InvalidLocalHistory,
    LocalHistoryTooLarge,
    StaleApproval,
    PayloadIdentityMismatch,
};

struct ShareModelDiagnostic {
    ShareModelCode code{ShareModelCode::Success};
    std::optional<SubmissionCode> submissionCode;
    std::string message;

    bool succeeded() const noexcept { return code == ShareModelCode::Success; }
};

// UI-independent local-first state machine. Results live in history independently
// of transport state, so failed/declined submissions cannot erase technical truth.
class CompatibilityShareModel final {
public:
    ShareModelDiagnostic setPrivacySettings(const CompatibilityPrivacySettings& settings);
    ShareModelDiagnostic loadPrivacySettingsJson(std::string_view json);
    ShareModelDiagnostic exportPrivacySettingsJson(std::string& json) const;
    ShareModelDiagnostic recordLocalResult(const compat::CompatibilityResult& result);
    ShareModelDiagnostic preparePreview(CompatibilitySharePreview& preview);
    ShareModelDiagnostic declineSharing();
    ShareModelDiagnostic beginSubmission(const CompatibilitySharePreview& approvedPreview);
    ShareModelDiagnostic completeSubmission(CommunitySubmissionTransport& transport,
                                            SubmissionReceipt& receipt);
    ShareModelDiagnostic prepareActiveLocalExport(CompatibilityLocalExportPreview& preview);
    ShareModelDiagnostic prepareLocalExport(std::string_view resultId,
                                            CompatibilityLocalExportPreview& preview);
    ShareModelDiagnostic approveLocalExport(const CompatibilityLocalExportPreview& approvedPreview);
    ShareModelDiagnostic declineLocalExport();
    ShareModelDiagnostic exportActiveLocalResult(std::string& exactJson) const;
    ShareModelDiagnostic exportLocalResult(std::string_view resultId, std::string& exactJson) const;
    ShareModelDiagnostic loadLocalHistoryJsonl(std::string_view jsonl);
    ShareModelDiagnostic exportLocalHistoryJsonl(std::string& jsonl) const;
    ShareModelDiagnostic deleteLocalResult(std::string_view resultId);
    ShareModelDiagnostic clearLocalResults();

    CompatibilityShareState state() const noexcept;
    const CompatibilityShareEntry* active() const noexcept;
    const std::vector<CompatibilityShareEntry>& history() const noexcept { return history_; }
    const CompatibilityPrivacySettings& privacySettings() const noexcept { return privacySettings_; }

private:
    CompatibilityShareEntry* activeMutable() noexcept;
    void enforceRetentionLimit() noexcept;
    void resetLocalExportApproval() noexcept;

    struct LocalExportSession {
        std::optional<std::string> resultId;
        std::string exactJson;
        std::string payloadIdentity;
        std::uint64_t generation{0u};
        bool approved{false};
    };

    std::vector<CompatibilityShareEntry> history_;
    std::optional<std::size_t> activeIndex_;
    SubmissionSession submission_;
    LocalExportSession localExport_;
    CompatibilityPrivacySettings privacySettings_;
    std::uint64_t nextPreviewGeneration_{1u};
    std::uint64_t nextLocalExportGeneration_{1u};
};

std::string_view compatibilityShareStateName(CompatibilityShareState value) noexcept;
std::string_view shareModelCodeName(ShareModelCode value) noexcept;

} // namespace hydra::community
