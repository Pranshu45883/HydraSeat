#pragma once

#include "hydra/artifact_trust.hpp"
#include "hydra/startup_policy.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::installer {

inline constexpr std::uint32_t kInstallerContractVersion = 1u;
inline constexpr std::size_t kMaximumInstallerComponents = 16u;
inline constexpr std::size_t kMaximumInstallerTransactionIdBytes = 96u;

enum class InstallerArchitecture : std::uint8_t { X64 = 0, X86 = 1 };
enum class InstallerAction : std::uint8_t { Install = 0, Repair = 1, Uninstall = 2 };
enum class OwnedComponent : std::uint8_t {
    MainUi = 0,
    Host = 1,
    SeatUi = 2,
    Watchdog = 3,
    Reset = 4,
    ProfileCli = 5,
    CommunityValidator = 6,
};

struct PackageComponent {
    OwnedComponent component{OwnedComponent::MainUi};
    trust::ArtifactManifest manifest;
    trust::ArtifactObservation observation;

    bool operator==(const PackageComponent&) const = default;
};

struct InstallerPackage {
    std::uint32_t schemaVersion{kInstallerContractVersion};
    std::string releaseVersion;
    std::uint64_t releaseRevision{0};
    InstallerArchitecture architecture{InstallerArchitecture::X64};
    std::vector<PackageComponent> components;

    bool operator==(const InstallerPackage&) const = default;
};

struct InstallerEnvironment {
    bool supportedWindows{true};
    InstallerArchitecture architecture{InstallerArchitecture::X64};
    bool prerequisiteReady{true};
    bool elevatedBrokerAvailable{true};
    bool hydraOwnedRiskyStateActive{false};
    bool hostOrSeatUiRunning{false};
};

struct InstallerOptions {
    startup::StartupMode startupMode{startup::StartupMode::Manual};
    bool startupApproved{false};
    bool offerFirstRunWizard{true};
    bool retainUserProfilesOnUninstall{true};
};

struct InstalledComponentState {
    OwnedComponent component{OwnedComponent::MainUi};
    std::string version;
    std::string sha256;

    bool operator==(const InstalledComponentState&) const = default;
};

struct InstalledState {
    std::uint32_t schemaVersion{kInstallerContractVersion};
    std::string installedVersion;
    std::uint64_t installedRevision{0};
    InstallerArchitecture architecture{InstallerArchitecture::X64};
    std::vector<InstalledComponentState> ownedComponents;
    bool uninstallRegistered{false};
    startup::StartupMode startupMode{startup::StartupMode::Manual};
    bool userProfilesPresent{false};

    bool operator==(const InstalledState&) const = default;
};

struct InstalledArtifactIdentity {
    OwnedComponent component{OwnedComponent::MainUi};
    std::string artifactId;
    std::string version;
    std::string sha256;
    std::string publisherIdentity;

    bool operator==(const InstalledArtifactIdentity&) const = default;
};

enum class InstallerCode : std::uint8_t {
    Success = 0,
    InvalidPackage,
    UnsupportedWindows,
    ArchitectureMismatch,
    PrerequisiteMissing,
    ElevationUnavailable,
    ActiveOwnedState,
    ProcessStillRunning,
    StartupApprovalRequired,
    PackageTrustRejected,
    MissingInstalledState,
    CaptureFailed,
    ExpectedPreviousMismatch,
    StageFailed,
    ApplyFailedRolledBack,
    VerifyFailedRolledBack,
    TransactionFinalizeFailed,
    RollbackFailed,
    RollbackVerifyFailed,
};

struct InstallerDiagnostic {
    InstallerCode code{InstallerCode::Success};
    std::string message;
    bool succeeded() const noexcept { return code == InstallerCode::Success; }
};

enum class InstallerReceiptStatus : std::uint8_t {
    Rejected = 0,
    Applied = 1,
    Removed = 2,
    RolledBack = 3,
    RecoveryRequired = 4,
};

struct InstallerReceipt {
    InstallerAction action{InstallerAction::Install};
    InstallerReceiptStatus status{InstallerReceiptStatus::Rejected};
    std::string transactionId;
    std::string releaseVersion;
    std::optional<InstalledState> previousState;
    std::optional<InstalledState> resultingState;
    std::vector<InstalledArtifactIdentity> committedArtifacts;
    bool rollbackAttempted{false};
    bool rollbackVerified{false};
    bool firstRunWizardOffered{false};
    bool userProfilesRetained{true};

    bool operator==(const InstallerReceipt&) const = default;
};

enum class InstallerTransactionDisposition : std::uint8_t {
    Committed = 0,
    RolledBack = 1,
    Aborted = 2,
};

// One instance owns the authoritative installer mutation lock and recovery journal
// for its entire lifetime. beginMutation() must capture previousState() while that
// lock is held. stage* must not publish partial files. commit() publishes only the
// staged candidate. finish() durably terminalizes the journal; destruction releases
// the lock but must not erase unresolved recovery evidence.
class InstallerMutationTransaction {
public:
    virtual ~InstallerMutationTransaction() = default;
    virtual std::string_view transactionId() const noexcept = 0;
    virtual const std::optional<InstalledState>& previousState() const noexcept = 0;
    virtual bool stageInstallOrRepair(InstallerAction action,
                                      const InstallerPackage& package,
                                      const InstallerOptions& options) noexcept = 0;
    virtual bool stageUninstall(const InstallerOptions& options) noexcept = 0;
    virtual bool commit() noexcept = 0;
    virtual bool verify(const std::optional<InstalledState>& expected) noexcept = 0;
    virtual bool rollback() noexcept = 0;
    virtual bool verifyRollback() noexcept = 0;
    virtual bool finish(InstallerTransactionDisposition disposition) noexcept = 0;
};

// Native wrapper owns fixed Program Files/ProgramData/uninstall-registration paths.
// The returned transaction must retain the same authoritative mutation lock from
// capture through compare-and-swap, staging, commit, verification, and finalization.
class InstallerExecutor {
public:
    virtual ~InstallerExecutor() = default;
    virtual std::unique_ptr<InstallerMutationTransaction> beginMutation() noexcept = 0;
};

InstallerDiagnostic validateInstallerPackage(
    const InstallerPackage& package,
    const InstallerEnvironment& environment,
    const trust::TrustPolicy& trustPolicy);

InstallerDiagnostic executeInstallerTransaction(
    InstallerAction action,
    const std::optional<InstallerPackage>& package,
    const InstallerEnvironment& environment,
    const InstallerOptions& options,
    const trust::TrustPolicy& trustPolicy,
    InstallerExecutor& executor,
    InstallerReceipt& receipt);

// Update/rollback path: expectedPrevious was captured during preview/approval. The
// exact authoritative installed state is re-read after beginMutation() acquires the
// same lock used for commit. A mismatch aborts before staging or mutation.
InstallerDiagnostic executeInstallerTransactionCompareAndSwap(
    InstallerAction action,
    const std::optional<InstallerPackage>& package,
    const InstalledState& expectedPrevious,
    const InstallerEnvironment& environment,
    const InstallerOptions& options,
    const trust::TrustPolicy& trustPolicy,
    InstallerExecutor& executor,
    InstallerReceipt& receipt);

std::string_view ownedComponentName(OwnedComponent value) noexcept;
std::string_view installerActionName(InstallerAction value) noexcept;
std::string_view installerCodeName(InstallerCode value) noexcept;
std::string_view installerReceiptStatusName(InstallerReceiptStatus value) noexcept;

} // namespace hydra::installer
