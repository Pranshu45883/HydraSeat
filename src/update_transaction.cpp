#include "hydra/update_transaction.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace hydra::update {
namespace {

constexpr std::size_t kMaximumApprovalIdentityBytes = 65536u;

UpdateDiagnostic fail(UpdateCode code, std::string message,
                      std::optional<installer::InstallerCode> installerCode = std::nullopt) {
    return {code, installerCode, std::move(message)};
}

bool validId(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128u) return false;
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

bool validStartupMode(startup::StartupMode value) noexcept {
    return value == startup::StartupMode::Manual || value == startup::StartupMode::BackgroundIdle ||
           value == startup::StartupMode::AutoActivateValidatedSession;
}

bool validOwnedComponent(installer::OwnedComponent value) noexcept {
    return value == installer::OwnedComponent::MainUi || value == installer::OwnedComponent::Host ||
           value == installer::OwnedComponent::SeatUi || value == installer::OwnedComponent::Watchdog ||
           value == installer::OwnedComponent::Reset || value == installer::OwnedComponent::ProfileCli ||
           value == installer::OwnedComponent::CommunityValidator;
}

bool validInstalledStateForPreview(const installer::InstalledState& state) noexcept {
    if (state.schemaVersion != installer::kInstallerContractVersion ||
        !validVersion(state.installedVersion) || state.installedRevision == 0u ||
        (state.architecture != installer::InstallerArchitecture::X64 &&
         state.architecture != installer::InstallerArchitecture::X86) ||
        !state.uninstallRegistered || !validStartupMode(state.startupMode) ||
        state.ownedComponents.size() != 7u ||
        state.ownedComponents.size() > installer::kMaximumInstallerComponents) {
        return false;
    }
    std::array<bool, 7> seen{};
    for (const auto& component : state.ownedComponents) {
        if (!validOwnedComponent(component.component) || component.version != state.installedVersion ||
            !validVersion(component.version) || !validSha256(component.sha256)) {
            return false;
        }
        const auto index = static_cast<std::size_t>(component.component);
        if (index >= seen.size() || seen[index]) return false;
        seen[index] = true;
    }
    return std::all_of(seen.begin(), seen.end(), [](bool value) { return value; });
}

bool validDirection(UpdateDirection value) noexcept {
    return value == UpdateDirection::Upgrade || value == UpdateDirection::Rollback;
}

void appendIdentityField(std::string& identity, std::string_view value) {
    identity.append(std::to_string(value.size()));
    identity.push_back(':');
    identity.append(value);
    identity.push_back('|');
}

void appendIdentityNumber(std::string& identity, std::uint64_t value) {
    identity.append(std::to_string(value));
    identity.push_back('|');
}

std::vector<const installer::PackageComponent*> canonicalComponents(
    const installer::InstallerPackage& package) {
    std::vector<const installer::PackageComponent*> components;
    components.reserve(package.components.size());
    for (const auto& component : package.components) components.push_back(&component);
    std::sort(components.begin(), components.end(), [](const auto* left, const auto* right) {
        return static_cast<std::uint8_t>(left->component) <
               static_cast<std::uint8_t>(right->component);
    });
    return components;
}

} // namespace

std::string makeApprovalIdentity(const ApplicationUpdateOffer& offer) {
    if (offer.package.components.empty() ||
        offer.package.components.size() > installer::kMaximumInstallerComponents ||
        offer.package.releaseVersion.empty() || offer.package.releaseVersion.size() > 64u ||
        offer.releaseNotesId.empty() || offer.releaseNotesId.size() > 128u) {
        return "app-update:invalid";
    }

    std::string identity{"app-update-v3|"};
    identity.reserve(4096u);
    appendIdentityNumber(identity, offer.schemaVersion);
    appendIdentityField(identity, offer.package.releaseVersion);
    appendIdentityNumber(identity, offer.package.releaseRevision);
    appendIdentityNumber(identity, static_cast<std::uint8_t>(offer.package.architecture));
    appendIdentityField(identity, offer.releaseNotesId);
    appendIdentityNumber(identity, offer.restartRecommended ? 1u : 0u);

    for (const auto* component : canonicalComponents(offer.package)) {
        const auto& manifest = component->manifest;
        const auto& observation = component->observation;
        if (manifest.artifactId.size() > 128u || manifest.artifactVersion.size() > 128u ||
            manifest.expectedSha256.size() > 64u || manifest.sourceId.size() > 128u ||
            manifest.licenseId.size() > 128u || observation.artifactVersion.size() > 128u ||
            observation.observedSha256.size() > 64u ||
            observation.publisherIdentity.size() > trust::kMaximumPublisherIdentityBytes ||
            manifest.capabilityScope.size() > trust::kMaximumArtifactCapabilities) {
            return "app-update:invalid";
        }

        appendIdentityNumber(identity, static_cast<std::uint8_t>(component->component));
        appendIdentityNumber(identity, manifest.schemaVersion);
        appendIdentityField(identity, manifest.artifactId);
        appendIdentityNumber(identity, static_cast<std::uint8_t>(manifest.artifactClass));
        appendIdentityField(identity, manifest.artifactVersion);
        appendIdentityNumber(identity, static_cast<std::uint8_t>(manifest.architecture));
        appendIdentityField(identity, manifest.expectedSha256);
        appendIdentityField(identity, manifest.sourceId);
        appendIdentityField(identity, manifest.licenseId);
        appendIdentityNumber(identity, manifest.redistributionAllowed ? 1u : 0u);
        appendIdentityNumber(identity, manifest.optional ? 1u : 0u);
        appendIdentityNumber(identity, manifest.developmentBuild ? 1u : 0u);
        appendIdentityNumber(identity, manifest.requiresInstall ? 1u : 0u);
        appendIdentityNumber(identity, manifest.requiresRestart ? 1u : 0u);
        appendIdentityNumber(identity, manifest.requiresRecoveryPlan ? 1u : 0u);

        auto capabilities = manifest.capabilityScope;
        std::sort(capabilities.begin(), capabilities.end());
        appendIdentityNumber(identity, capabilities.size());
        for (const auto& capability : capabilities) {
            if (capability.size() > 128u) return "app-update:invalid";
            appendIdentityField(identity, capability);
        }

        appendIdentityNumber(identity, observation.present ? 1u : 0u);
        appendIdentityField(identity, observation.artifactVersion);
        appendIdentityNumber(identity, static_cast<std::uint8_t>(observation.architecture));
        appendIdentityField(identity, observation.observedSha256);
        appendIdentityNumber(identity, static_cast<std::uint8_t>(observation.signature));
        appendIdentityField(identity, observation.publisherIdentity);
        if (identity.size() > kMaximumApprovalIdentityBytes) return "app-update:invalid";
    }
    return identity;
}

UpdateDiagnostic previewApplicationUpdate(
    const installer::InstalledState& current,
    const ApplicationUpdateOffer& offer,
    UpdateDirection direction,
    const installer::InstallerEnvironment& environment,
    const trust::TrustPolicy& trustPolicy,
    UpdatePreview& preview) {
    if (offer.schemaVersion != kUpdateContractVersion || !validDirection(direction) ||
        !validId(offer.releaseNotesId) || !validInstalledStateForPreview(current)) {
        return fail(UpdateCode::InvalidOffer,
                    "application update offer/current installation metadata is invalid");
    }
    if (current.architecture != environment.architecture ||
        offer.package.architecture != current.architecture) {
        return fail(UpdateCode::ArchitectureMismatch,
                    "application update architecture does not match the installed architecture");
    }

    const auto packageValidation =
        installer::validateInstallerPackage(offer.package, environment, trustPolicy);
    if (!packageValidation.succeeded()) {
        return fail(packageValidation.code == installer::InstallerCode::ArchitectureMismatch
                        ? UpdateCode::ArchitectureMismatch
                        : UpdateCode::TrustRejected,
                    "application update package failed installer trust validation: " +
                        packageValidation.message,
                    packageValidation.code);
    }

    if (direction == UpdateDirection::Upgrade &&
        offer.package.releaseRevision <= current.installedRevision) {
        return fail(UpdateCode::NotNewer,
                    "application update revision must be newer than the installed revision");
    }
    if (direction == UpdateDirection::Rollback &&
        offer.package.releaseRevision >= current.installedRevision) {
        return fail(UpdateCode::InvalidRollbackTarget,
                    "rollback package revision must be older than the installed revision");
    }

    const auto approvalIdentity = makeApprovalIdentity(offer);
    if (approvalIdentity == "app-update:invalid" ||
        approvalIdentity.size() > kMaximumApprovalIdentityBytes) {
        return fail(UpdateCode::InvalidOffer,
                    "application update approval identity exceeds the bounded canonical contract");
    }

    UpdatePreview candidate;
    candidate.direction = direction;
    candidate.currentVersion = current.installedVersion;
    candidate.currentRevision = current.installedRevision;
    candidate.targetVersion = offer.package.releaseVersion;
    candidate.targetRevision = offer.package.releaseRevision;
    candidate.releaseNotesId = offer.releaseNotesId;
    candidate.restartRecommended = offer.restartRecommended;
    candidate.approvalIdentity = approvalIdentity;
    preview = std::move(candidate);
    return {};
}

UpdateDiagnostic applyApplicationUpdate(
    const installer::InstalledState& current,
    const ApplicationUpdateOffer& offer,
    UpdateDirection direction,
    const UpdateApproval& approval,
    const installer::InstallerEnvironment& environment,
    const installer::InstallerOptions& options,
    const trust::TrustPolicy& trustPolicy,
    installer::InstallerExecutor& executor,
    installer::InstallerReceipt& receipt) {
    UpdatePreview preview;
    const auto inspected = previewApplicationUpdate(current, offer, direction, environment,
                                                    trustPolicy, preview);
    if (!inspected.succeeded()) return inspected;
    if (!approval.userApproved) {
        return fail(UpdateCode::ApprovalRequired,
                    "application executable update/rollback requires explicit user approval");
    }
    if (approval.approvalIdentity != preview.approvalIdentity) {
        return fail(UpdateCode::ApprovalMismatch,
                    "application update approval does not match the exact target revision");
    }
    if (options.startupMode != current.startupMode) {
        return fail(UpdateCode::InvalidOffer,
                    "application update cannot silently change the user's startup mode");
    }

    const auto installed = installer::executeInstallerTransactionCompareAndSwap(
        installer::InstallerAction::Repair, offer.package, current, environment, options, trustPolicy,
        executor, receipt);
    if (installed.code == installer::InstallerCode::ExpectedPreviousMismatch) {
        return fail(UpdateCode::StaleInstalledState,
                    "installed HydraSeat state changed after update preview; stale approval cannot overwrite it",
                    installed.code);
    }
    if (!installed.succeeded()) {
        return fail(UpdateCode::InstallerRejected,
                    "application update transaction failed: " + installed.message,
                    installed.code);
    }
    return {};
}

std::string_view updateDirectionName(UpdateDirection value) noexcept {
    switch (value) {
        case UpdateDirection::Upgrade: return "Upgrade";
        case UpdateDirection::Rollback: return "Rollback";
    }
    return "Unknown";
}

std::string_view updateCodeName(UpdateCode value) noexcept {
    switch (value) {
        case UpdateCode::Success: return "Success";
        case UpdateCode::InvalidOffer: return "InvalidOffer";
        case UpdateCode::ArchitectureMismatch: return "ArchitectureMismatch";
        case UpdateCode::NotNewer: return "NotNewer";
        case UpdateCode::InvalidRollbackTarget: return "InvalidRollbackTarget";
        case UpdateCode::TrustRejected: return "TrustRejected";
        case UpdateCode::ApprovalRequired: return "ApprovalRequired";
        case UpdateCode::ApprovalMismatch: return "ApprovalMismatch";
        case UpdateCode::StaleInstalledState: return "StaleInstalledState";
        case UpdateCode::InstallerRejected: return "InstallerRejected";
    }
    return "Unknown";
}

} // namespace hydra::update
