#include "hydra/installer_transaction.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <utility>

namespace hydra::installer {
namespace {

InstallerDiagnostic fail(InstallerCode code, std::string message) {
    return {code, std::move(message)};
}

bool validArchitecture(InstallerArchitecture value) noexcept {
    return value == InstallerArchitecture::X64 || value == InstallerArchitecture::X86;
}

bool validStartupMode(startup::StartupMode value) noexcept {
    return value == startup::StartupMode::Manual || value == startup::StartupMode::BackgroundIdle ||
           value == startup::StartupMode::AutoActivateValidatedSession;
}

bool validAction(InstallerAction value) noexcept {
    return value == InstallerAction::Install || value == InstallerAction::Repair ||
           value == InstallerAction::Uninstall;
}

bool validVersion(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64u) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '+')) {
            return false;
        }
    }
    return true;
}

bool validSha256(std::string_view value) noexcept {
    if (value.size() != 64u) return false;
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

bool validTransactionId(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumInstallerTransactionIdBytes) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')) {
            return false;
        }
    }
    return true;
}

struct ComponentContract {
    OwnedComponent component;
    std::string_view artifactId;
    std::string_view capability;
};

constexpr std::array<ComponentContract, 7> kContracts{{
    {OwnedComponent::MainUi, "hydraseat-main-ui", "main-ui"},
    {OwnedComponent::Host, "hydraseat-host", "runtime-host"},
    {OwnedComponent::SeatUi, "hydraseat-seat-ui", "seat-ui"},
    {OwnedComponent::Watchdog, "hydraseat-watchdog", "watchdog"},
    {OwnedComponent::Reset, "hydraseat-reset", "recovery-reset"},
    {OwnedComponent::ProfileCli, "hydraseat-profile-cli", "profile-cli"},
    {OwnedComponent::CommunityValidator, "hydraseat-community-validator", "community-validator"},
}};

const ComponentContract* contractFor(OwnedComponent component) noexcept {
    const auto found = std::find_if(kContracts.begin(), kContracts.end(),
                                    [component](const auto& item) {
                                        return item.component == component;
                                    });
    return found == kContracts.end() ? nullptr : &*found;
}

trust::ArtifactArchitecture trustArchitecture(InstallerArchitecture architecture) noexcept {
    return architecture == InstallerArchitecture::X64 ? trust::ArtifactArchitecture::X64
                                                       : trust::ArtifactArchitecture::X86;
}

InstallerDiagnostic validatePackageInternal(const InstallerPackage& package,
                                            const InstallerEnvironment& environment,
                                            const trust::TrustPolicy& policy) {
    if (package.schemaVersion != kInstallerContractVersion || !validVersion(package.releaseVersion) ||
        package.releaseRevision == 0u || !validArchitecture(package.architecture) ||
        package.components.size() != kContracts.size() ||
        package.components.size() > kMaximumInstallerComponents) {
        return fail(InstallerCode::InvalidPackage,
                    "installer package schema/version/component set is invalid");
    }
    if (package.architecture != environment.architecture) {
        return fail(InstallerCode::ArchitectureMismatch,
                    "installer package architecture does not match the supported Windows target");
    }

    std::set<OwnedComponent> seen;
    for (const auto& component : package.components) {
        const auto* contract = contractFor(component.component);
        if (contract == nullptr || !seen.insert(component.component).second) {
            return fail(InstallerCode::InvalidPackage,
                        "installer package has an unknown or duplicate owned component");
        }
        const auto& manifest = component.manifest;
        if (manifest.artifactId != contract->artifactId ||
            manifest.artifactClass != trust::ArtifactClass::Executable ||
            manifest.artifactVersion != package.releaseVersion ||
            manifest.architecture != trustArchitecture(package.architecture) ||
            !manifest.requiresInstall || !manifest.requiresRecoveryPlan ||
            manifest.capabilityScope.size() != 1u ||
            manifest.capabilityScope.front() != contract->capability) {
            return fail(InstallerCode::InvalidPackage,
                        "owned component manifest does not match the fixed installer contract");
        }
        const auto evaluation = trust::evaluateArtifact(manifest, component.observation, policy);
        if (evaluation.decision != trust::TrustDecision::Accept) {
            return fail(InstallerCode::PackageTrustRejected,
                        std::string("owned component trust rejected: ") +
                            std::string(trust::trustCodeName(evaluation.code)));
        }
    }
    if (seen.size() != kContracts.size()) {
        return fail(InstallerCode::InvalidPackage,
                    "installer package is missing a required owned component");
    }
    return {};
}

InstalledState stateFromPackage(const InstallerPackage& package,
                                const InstallerOptions& options,
                                bool userProfilesPresent) {
    InstalledState state;
    state.installedVersion = package.releaseVersion;
    state.installedRevision = package.releaseRevision;
    state.architecture = package.architecture;
    state.uninstallRegistered = true;
    state.startupMode = options.startupMode;
    state.userProfilesPresent = userProfilesPresent;
    state.ownedComponents.reserve(package.components.size());
    for (const auto& component : package.components) {
        state.ownedComponents.push_back(
            {component.component, component.manifest.artifactVersion,
             component.observation.observedSha256});
    }
    std::sort(state.ownedComponents.begin(), state.ownedComponents.end(),
              [](const auto& left, const auto& right) {
                  return static_cast<std::uint8_t>(left.component) <
                         static_cast<std::uint8_t>(right.component);
              });
    return state;
}

bool validInstalledState(const InstalledState& state) noexcept {
    if (state.schemaVersion != kInstallerContractVersion || !validVersion(state.installedVersion) ||
        state.installedRevision == 0u || !validArchitecture(state.architecture) ||
        !validStartupMode(state.startupMode) || state.ownedComponents.size() != kContracts.size() ||
        !state.uninstallRegistered) {
        return false;
    }
    std::set<OwnedComponent> seen;
    for (const auto& component : state.ownedComponents) {
        if (contractFor(component.component) == nullptr ||
            !seen.insert(component.component).second || !validVersion(component.version) ||
            component.version != state.installedVersion || !validSha256(component.sha256)) {
            return false;
        }
    }
    return seen.size() == kContracts.size();
}

bool sameInstalledState(const InstalledState& left, const InstalledState& right) {
    if (left.schemaVersion != right.schemaVersion ||
        left.installedVersion != right.installedVersion ||
        left.installedRevision != right.installedRevision || left.architecture != right.architecture ||
        left.uninstallRegistered != right.uninstallRegistered || left.startupMode != right.startupMode ||
        left.userProfilesPresent != right.userProfilesPresent ||
        left.ownedComponents.size() != right.ownedComponents.size()) {
        return false;
    }
    auto leftComponents = left.ownedComponents;
    auto rightComponents = right.ownedComponents;
    const auto byComponent = [](const auto& a, const auto& b) {
        return static_cast<std::uint8_t>(a.component) < static_cast<std::uint8_t>(b.component);
    };
    std::sort(leftComponents.begin(), leftComponents.end(), byComponent);
    std::sort(rightComponents.begin(), rightComponents.end(), byComponent);
    return leftComponents == rightComponents;
}

std::vector<InstalledArtifactIdentity> artifactIdentitiesFromPackage(const InstallerPackage& package) {
    std::vector<InstalledArtifactIdentity> identities;
    identities.reserve(package.components.size());
    for (const auto& component : package.components) {
        identities.push_back({component.component, component.manifest.artifactId,
                              component.manifest.artifactVersion,
                              component.observation.observedSha256,
                              component.observation.publisherIdentity});
    }
    std::sort(identities.begin(), identities.end(), [](const auto& left, const auto& right) {
        return static_cast<std::uint8_t>(left.component) < static_cast<std::uint8_t>(right.component);
    });
    return identities;
}

InstallerDiagnostic executeInstallerTransactionInternal(
    InstallerAction action,
    const std::optional<InstallerPackage>& package,
    const InstalledState* expectedPrevious,
    const InstallerEnvironment& environment,
    const InstallerOptions& options,
    const trust::TrustPolicy& trustPolicy,
    InstallerExecutor& executor,
    InstallerReceipt& receipt) {
    receipt = {};
    if (!validAction(action)) return fail(InstallerCode::InvalidPackage, "unknown installer action");
    if (!environment.supportedWindows) {
        return fail(InstallerCode::UnsupportedWindows, "Windows version/build is unsupported");
    }
    if (!validArchitecture(environment.architecture)) {
        return fail(InstallerCode::ArchitectureMismatch, "Windows architecture is unsupported");
    }
    if (!environment.prerequisiteReady) {
        return fail(InstallerCode::PrerequisiteMissing, "required Windows prerequisite is unavailable");
    }
    if (!environment.elevatedBrokerAvailable) {
        return fail(InstallerCode::ElevationUnavailable,
                    "installer requires the narrow elevated mutation boundary");
    }
    if (environment.hydraOwnedRiskyStateActive) {
        return fail(InstallerCode::ActiveOwnedState,
                    "installer refuses to mutate files while HydraSeat-owned risky state is active");
    }
    if (environment.hostOrSeatUiRunning) {
        return fail(InstallerCode::ProcessStillRunning,
                    "return to ordinary Windows and close HydraSeat UI/Host before install changes");
    }
    if (!validStartupMode(options.startupMode)) {
        return fail(InstallerCode::InvalidPackage, "installer options contain an invalid startup mode");
    }
    if (options.startupMode != startup::StartupMode::Manual && !options.startupApproved) {
        return fail(InstallerCode::StartupApprovalRequired,
                    "background startup registration requires explicit user approval");
    }
    if (expectedPrevious != nullptr && !validInstalledState(*expectedPrevious)) {
        return fail(InstallerCode::InvalidPackage,
                    "compare-and-swap expected previous install state is invalid");
    }

    if (action == InstallerAction::Uninstall) {
        if (package) return fail(InstallerCode::InvalidPackage, "uninstall cannot carry a replacement package");
    } else {
        if (!package) return fail(InstallerCode::InvalidPackage, "install/repair requires an exact package");
        const auto packageValidation = validatePackageInternal(*package, environment, trustPolicy);
        if (!packageValidation.succeeded()) return packageValidation;
    }

    auto transaction = executor.beginMutation();
    if (!transaction) {
        return fail(InstallerCode::CaptureFailed,
                    "installer could not enter the authoritative mutation lock/transaction boundary");
    }
    const auto transactionIdView = transaction->transactionId();
    if (!validTransactionId(transactionIdView)) {
        if (!transaction->finish(InstallerTransactionDisposition::Aborted)) {
            receipt.status = InstallerReceiptStatus::RecoveryRequired;
            return fail(InstallerCode::TransactionFinalizeFailed,
                        "installer transaction identity was invalid and abort finalization failed");
        }
        return fail(InstallerCode::CaptureFailed, "installer transaction identity is invalid or unbounded");
    }

    const auto& authoritativePrevious = transaction->previousState();
    if (authoritativePrevious && !validInstalledState(*authoritativePrevious)) {
        if (!transaction->finish(InstallerTransactionDisposition::Aborted)) {
            receipt.status = InstallerReceiptStatus::RecoveryRequired;
            return fail(InstallerCode::TransactionFinalizeFailed,
                        "captured installed state was invalid and transaction abort finalization failed");
        }
        return fail(InstallerCode::CaptureFailed,
                    "captured installed-state record is invalid or outside the owned contract");
    }

    const std::optional<InstalledState> previous = authoritativePrevious;
    InstallerReceipt candidate;
    candidate.action = action;
    candidate.transactionId = std::string(transactionIdView);
    candidate.releaseVersion = package ? package->releaseVersion
                                       : (previous ? previous->installedVersion : std::string{});
    candidate.previousState = previous;
    candidate.resultingState = previous;
    candidate.userProfilesRetained = previous ? previous->userProfilesPresent : true;

    const auto abortWithoutMutation = [&](InstallerCode code, std::string message) -> InstallerDiagnostic {
        if (!transaction->finish(InstallerTransactionDisposition::Aborted)) {
            candidate.status = InstallerReceiptStatus::RecoveryRequired;
            receipt = candidate;
            return fail(InstallerCode::TransactionFinalizeFailed,
                        "installer made no committed change but could not finalize its recovery journal");
        }
        candidate.status = InstallerReceiptStatus::Rejected;
        receipt = candidate;
        return fail(code, std::move(message));
    };

    if (expectedPrevious != nullptr &&
        (!previous || !sameInstalledState(*expectedPrevious, *previous))) {
        return abortWithoutMutation(
            InstallerCode::ExpectedPreviousMismatch,
            "authoritative installed state changed after preview/approval; compare-and-swap rejected");
    }
    if (action == InstallerAction::Install && previous) {
        return abortWithoutMutation(
            InstallerCode::InvalidPackage,
            "existing installation requires Repair/Update rather than a second Install");
    }
    if ((action == InstallerAction::Repair || action == InstallerAction::Uninstall) && !previous) {
        return abortWithoutMutation(
            InstallerCode::MissingInstalledState,
            "repair/uninstall requires an existing HydraSeat-owned install record");
    }

    std::optional<InstalledState> expected;
    if (action != InstallerAction::Uninstall) {
        const bool profiles = previous ? previous->userProfilesPresent : false;
        expected = stateFromPackage(*package, options, profiles);
    }

    const bool staged = action == InstallerAction::Uninstall
                            ? transaction->stageUninstall(options)
                            : transaction->stageInstallOrRepair(action, *package, options);
    if (!staged) {
        return abortWithoutMutation(
            InstallerCode::StageFailed,
            "installer staging failed before any committed installed state was changed");
    }

    const auto rollback = [&](InstallerCode originalFailure, std::string message) -> InstallerDiagnostic {
        candidate.rollbackAttempted = true;
        candidate.committedArtifacts.clear();
        if (!transaction->rollback()) {
            candidate.status = InstallerReceiptStatus::RecoveryRequired;
            candidate.rollbackVerified = false;
            candidate.resultingState.reset();
            receipt = candidate;
            return fail(InstallerCode::RollbackFailed,
                        "installer mutation failed and rollback could not restore prior owned state");
        }
        if (!transaction->verifyRollback()) {
            candidate.status = InstallerReceiptStatus::RecoveryRequired;
            candidate.rollbackVerified = false;
            candidate.resultingState.reset();
            receipt = candidate;
            return fail(InstallerCode::RollbackVerifyFailed,
                        "installer rollback ran but prior owned state was not verified");
        }
        candidate.rollbackVerified = true;
        candidate.resultingState = previous;
        if (!transaction->finish(InstallerTransactionDisposition::RolledBack)) {
            candidate.status = InstallerReceiptStatus::RecoveryRequired;
            receipt = candidate;
            return fail(InstallerCode::TransactionFinalizeFailed,
                        "installer restored prior state but could not finalize the rollback journal");
        }
        candidate.status = InstallerReceiptStatus::RolledBack;
        receipt = candidate;
        return fail(originalFailure, std::move(message));
    };

    if (!transaction->commit()) {
        return rollback(InstallerCode::ApplyFailedRolledBack,
                        "installer commit failed and prior owned state was restored");
    }
    if (!transaction->verify(expected)) {
        return rollback(InstallerCode::VerifyFailedRolledBack,
                        "installer verification failed and prior owned state was restored");
    }
    if (!transaction->finish(InstallerTransactionDisposition::Committed)) {
        return rollback(InstallerCode::TransactionFinalizeFailed,
                        "installer commit finalization failed and prior owned state was restored");
    }

    candidate.status = action == InstallerAction::Uninstall ? InstallerReceiptStatus::Removed
                                                             : InstallerReceiptStatus::Applied;
    candidate.resultingState = expected;
    candidate.committedArtifacts = package ? artifactIdentitiesFromPackage(*package)
                                           : std::vector<InstalledArtifactIdentity>{};
    candidate.firstRunWizardOffered = action == InstallerAction::Install && options.offerFirstRunWizard;
    candidate.userProfilesRetained = action != InstallerAction::Uninstall
                                         ? (expected ? expected->userProfilesPresent : true)
                                         : options.retainUserProfilesOnUninstall;
    receipt = std::move(candidate);
    return {};
}

} // namespace

InstallerDiagnostic validateInstallerPackage(
    const InstallerPackage& package,
    const InstallerEnvironment& environment,
    const trust::TrustPolicy& trustPolicy) {
    if (!environment.supportedWindows) {
        return fail(InstallerCode::UnsupportedWindows, "Windows version/build is unsupported");
    }
    if (!validArchitecture(environment.architecture)) {
        return fail(InstallerCode::ArchitectureMismatch, "Windows architecture is unsupported");
    }
    return validatePackageInternal(package, environment, trustPolicy);
}

InstallerDiagnostic executeInstallerTransaction(
    InstallerAction action,
    const std::optional<InstallerPackage>& package,
    const InstallerEnvironment& environment,
    const InstallerOptions& options,
    const trust::TrustPolicy& trustPolicy,
    InstallerExecutor& executor,
    InstallerReceipt& receipt) {
    return executeInstallerTransactionInternal(action, package, nullptr, environment, options,
                                               trustPolicy, executor, receipt);
}

InstallerDiagnostic executeInstallerTransactionCompareAndSwap(
    InstallerAction action,
    const std::optional<InstallerPackage>& package,
    const InstalledState& expectedPrevious,
    const InstallerEnvironment& environment,
    const InstallerOptions& options,
    const trust::TrustPolicy& trustPolicy,
    InstallerExecutor& executor,
    InstallerReceipt& receipt) {
    return executeInstallerTransactionInternal(action, package, &expectedPrevious, environment, options,
                                               trustPolicy, executor, receipt);
}

std::string_view ownedComponentName(OwnedComponent value) noexcept {
    switch (value) {
        case OwnedComponent::MainUi: return "MainUi";
        case OwnedComponent::Host: return "Host";
        case OwnedComponent::SeatUi: return "SeatUi";
        case OwnedComponent::Watchdog: return "Watchdog";
        case OwnedComponent::Reset: return "Reset";
        case OwnedComponent::ProfileCli: return "ProfileCli";
        case OwnedComponent::CommunityValidator: return "CommunityValidator";
    }
    return "Unknown";
}

std::string_view installerActionName(InstallerAction value) noexcept {
    switch (value) {
        case InstallerAction::Install: return "Install";
        case InstallerAction::Repair: return "Repair";
        case InstallerAction::Uninstall: return "Uninstall";
    }
    return "Unknown";
}

std::string_view installerCodeName(InstallerCode value) noexcept {
    switch (value) {
        case InstallerCode::Success: return "Success";
        case InstallerCode::InvalidPackage: return "InvalidPackage";
        case InstallerCode::UnsupportedWindows: return "UnsupportedWindows";
        case InstallerCode::ArchitectureMismatch: return "ArchitectureMismatch";
        case InstallerCode::PrerequisiteMissing: return "PrerequisiteMissing";
        case InstallerCode::ElevationUnavailable: return "ElevationUnavailable";
        case InstallerCode::ActiveOwnedState: return "ActiveOwnedState";
        case InstallerCode::ProcessStillRunning: return "ProcessStillRunning";
        case InstallerCode::StartupApprovalRequired: return "StartupApprovalRequired";
        case InstallerCode::PackageTrustRejected: return "PackageTrustRejected";
        case InstallerCode::MissingInstalledState: return "MissingInstalledState";
        case InstallerCode::CaptureFailed: return "CaptureFailed";
        case InstallerCode::ExpectedPreviousMismatch: return "ExpectedPreviousMismatch";
        case InstallerCode::StageFailed: return "StageFailed";
        case InstallerCode::ApplyFailedRolledBack: return "ApplyFailedRolledBack";
        case InstallerCode::VerifyFailedRolledBack: return "VerifyFailedRolledBack";
        case InstallerCode::TransactionFinalizeFailed: return "TransactionFinalizeFailed";
        case InstallerCode::RollbackFailed: return "RollbackFailed";
        case InstallerCode::RollbackVerifyFailed: return "RollbackVerifyFailed";
    }
    return "Unknown";
}

std::string_view installerReceiptStatusName(InstallerReceiptStatus value) noexcept {
    switch (value) {
        case InstallerReceiptStatus::Rejected: return "Rejected";
        case InstallerReceiptStatus::Applied: return "Applied";
        case InstallerReceiptStatus::Removed: return "Removed";
        case InstallerReceiptStatus::RolledBack: return "RolledBack";
        case InstallerReceiptStatus::RecoveryRequired: return "RecoveryRequired";
    }
    return "Unknown";
}

} // namespace hydra::installer
