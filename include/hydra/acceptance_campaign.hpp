#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::acceptance {

inline constexpr std::uint32_t kCampaignSchemaVersion = 2u;
inline constexpr std::uint32_t kEvidenceSchemaVersion = 2u;
inline constexpr std::size_t kCampaignStageCount = 13u;
inline constexpr std::size_t kMaximumCampaignBytes = 1024u * 1024u;
inline constexpr std::size_t kMaximumEvidencePerStage = 16u;
inline constexpr std::size_t kMaximumIdentifierBytes = 128u;
inline constexpr std::size_t kMaximumNoteBytes = 8192u;
inline constexpr std::uint64_t kMaximumEvidenceAgeSeconds = 7u * 24u * 60u * 60u;

enum class CampaignStage : std::uint8_t {
    Preflight = 0,
    Phase3Physical,
    DisplayReconnect,
    DifferentGames,
    SameTitle,
    SeatIndependence,
    ReturnToWindows,
    InstallRepairUninstall,
    RebootStartup,
    UpdateRollback,
    Offline,
    FaultRecovery,
    PerformanceSoak,
};

enum class StageState : std::uint8_t {
    Pending = 0,
    Running,
    AwaitingManualReview,
    Passed,
    Failed,
    RecoveryRequired,
};

enum class EvidenceOrigin : std::uint8_t {
    Synthetic = 0,
    ControlledProcess,
    Physical,
};

// Evidence class is independent from collection origin. A Physical origin does
// not by itself prove real-game, clean-machine, manual, or signing evidence.
enum class EvidenceClass : std::uint8_t {
    Synthetic = 0,
    Controlled,
    Physical,
    Manual,
    RealGame,
    CleanMachineInstall,
    SigningDeployment,
    Unspecified = 0xffu,
};

enum class HumanVerdict : std::uint8_t {
    Pending = 0,
    Pass,
    Fail,
};

enum class CampaignCode : std::uint8_t {
    Success = 0,
    InvalidIdentity,
    InvalidCampaign,
    InvalidStage,
    InvalidTransition,
    EvidenceMalformed,
    EvidenceStale,
    EvidenceIdentityMismatch,
    EvidenceDuplicate,
    EvidenceClassMismatch,
    PhysicalEvidenceRequired,
    ManualVerdictRequired,
    DocumentTooLarge,
    DecodeFailed,
    StorageFailed,
    NoRecoverableDocument,
};

struct CampaignIdentity {
    std::string campaignId;
    std::string rcCommitSha;
    std::string releaseArtifactSha256;
    std::string releaseArtifactName;
    std::uint64_t releaseRevision{0u};
    std::string architecture;
    std::string profileSha256;
    std::string installStateSha256;
    std::string windowsBuild;
    std::string topologyFingerprintSha256;
    std::array<std::uint32_t, 2> seatIds{1u, 2u};
    std::string scenarioIdentity;
    // One logical session/run identity. The runner also binds this token to the
    // acceptance-session directory; campaignctl fills it from campaignId when omitted.
    std::string sessionRunId;

    bool operator==(const CampaignIdentity&) const = default;
};

struct ChildEvidence {
    std::uint32_t schemaVersion{kEvidenceSchemaVersion};
    std::string evidenceId;
    CampaignStage stage{CampaignStage::Preflight};
    EvidenceOrigin origin{EvidenceOrigin::Synthetic};
    std::uint64_t createdUnixSeconds{0u};
    std::string contentSha256;
    std::string evidenceArtifactName;
    std::string testName;
    std::string rcCommitSha;
    std::string releaseArtifactSha256;
    std::uint64_t releaseRevision{0u};
    std::string architecture;
    std::string profileSha256;
    std::string installStateSha256;
    std::string scenarioIdentity;
    bool automatedPassed{false};
    HumanVerdict humanVerdict{HumanVerdict::Pending};
    std::string note;
    // These are frozen by the authoritative campaign boundary. Callers may
    // provide them for compare-and-reject; omitted values are filled from the campaign.
    std::uint32_t campaignSchemaVersion{kCampaignSchemaVersion};
    std::string campaignId;
    std::string sessionRunId;
    std::string releaseArtifactName;
    std::string windowsBuild;
    std::string topologyFingerprintSha256;
    EvidenceClass evidenceClass{EvidenceClass::Unspecified};

    bool operator==(const ChildEvidence&) const = default;
};

struct StageRecord {
    CampaignStage stage{CampaignStage::Preflight};
    StageState state{StageState::Pending};
    std::uint32_t attempt{0u};
    std::uint64_t startedUnixSeconds{0u};
    std::uint64_t completedUnixSeconds{0u};
    std::vector<ChildEvidence> evidence;
    std::string diagnostic;

    bool operator==(const StageRecord&) const = default;
};

struct AcceptanceCampaign {
    std::uint32_t schemaVersion{kCampaignSchemaVersion};
    CampaignIdentity identity;
    std::uint64_t createdUnixSeconds{0u};
    std::uint64_t updatedUnixSeconds{0u};
    std::vector<StageRecord> stages;

    bool operator==(const AcceptanceCampaign&) const = default;
};

struct CampaignDiagnostic {
    CampaignCode code{CampaignCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == CampaignCode::Success; }
};

AcceptanceCampaign makeCampaign(const CampaignIdentity& identity,
                                  std::uint64_t nowUnixSeconds,
                                  CampaignDiagnostic* diagnostic = nullptr);
CampaignDiagnostic validateCampaign(const AcceptanceCampaign& campaign,
                                      std::uint64_t nowUnixSeconds);
CampaignDiagnostic startStage(AcceptanceCampaign& campaign,
                              CampaignStage stage,
                              std::uint64_t nowUnixSeconds);
CampaignDiagnostic attachEvidence(AcceptanceCampaign& campaign,
                                  const ChildEvidence& evidence,
                                  std::uint64_t nowUnixSeconds);
CampaignDiagnostic recordManualVerdict(AcceptanceCampaign& campaign,
                                       CampaignStage stage,
                                       HumanVerdict verdict,
                                       std::string_view note,
                                       std::uint64_t nowUnixSeconds);
CampaignDiagnostic recoverInterrupted(AcceptanceCampaign& campaign,
                                      std::uint64_t nowUnixSeconds);

std::string encodeCampaignJson(const AcceptanceCampaign& campaign);
CampaignDiagnostic decodeCampaignJson(std::string_view text,
                                      AcceptanceCampaign& output,
                                      std::uint64_t nowUnixSeconds);

class CampaignStore final {
public:
    explicit CampaignStore(std::filesystem::path path) : path_(std::move(path)) {}

    CampaignDiagnostic write(const AcceptanceCampaign& campaign) const;
    CampaignDiagnostic load(AcceptanceCampaign& output,
                            std::uint64_t nowUnixSeconds) const;
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

bool stageRequiresPhysicalEvidence(CampaignStage stage) noexcept;
bool stageRequiresManualReview(CampaignStage stage) noexcept;
EvidenceClass evidenceClassForStage(CampaignStage stage) noexcept;
std::string_view campaignStageName(CampaignStage value) noexcept;
std::string_view stageStateName(StageState value) noexcept;
std::string_view evidenceOriginName(EvidenceOrigin value) noexcept;
std::string_view evidenceClassName(EvidenceClass value) noexcept;
std::string_view humanVerdictName(HumanVerdict value) noexcept;
std::string_view campaignCodeName(CampaignCode value) noexcept;

} // namespace hydra::acceptance
