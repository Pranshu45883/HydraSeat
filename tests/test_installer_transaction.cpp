#include "hydra/installer_transaction.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::installer;

constexpr std::string_view kPublisher = "0123456789ABCDEF0123456789ABCDEF01234567";
constexpr std::string_view kWrongPublisher = "89ABCDEF0123456789ABCDEF0123456789ABCDEF";

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

struct FixtureContract { OwnedComponent component; const char* artifact; const char* capability; };
constexpr std::array<FixtureContract, 7> contracts{{
    {OwnedComponent::MainUi, "hydraseat-main-ui", "main-ui"},
    {OwnedComponent::Host, "hydraseat-host", "runtime-host"},
    {OwnedComponent::SeatUi, "hydraseat-seat-ui", "seat-ui"},
    {OwnedComponent::Watchdog, "hydraseat-watchdog", "watchdog"},
    {OwnedComponent::Reset, "hydraseat-reset", "recovery-reset"},
    {OwnedComponent::ProfileCli, "hydraseat-profile-cli", "profile-cli"},
    {OwnedComponent::CommunityValidator, "hydraseat-community-validator", "community-validator"},
}};

InstallerPackage package(std::string version = "1.2.3", std::uint64_t revision = 0u) {
    InstallerPackage result;
    result.releaseVersion = std::move(version);
    result.releaseRevision = revision != 0u ? revision : (result.releaseVersion == "1.0.0" ? 100u : 123u);
    result.architecture = InstallerArchitecture::X64;
    for (std::size_t index = 0; index < contracts.size(); ++index) {
        const auto& contract = contracts[index];
        PackageComponent component;
        component.component = contract.component;
        component.manifest.artifactId = contract.artifact;
        component.manifest.artifactClass = trust::ArtifactClass::Executable;
        component.manifest.artifactVersion = result.releaseVersion;
        component.manifest.architecture = trust::ArtifactArchitecture::X64;
        component.manifest.expectedSha256 = std::string(64u, 'a');
        component.manifest.expectedSha256[0] = static_cast<char>('0' + index);
        component.manifest.sourceId = "hydraseat-release";
        component.manifest.licenseId = "hydraseat-project";
        component.manifest.redistributionAllowed = true;
        component.manifest.optional = false;
        component.manifest.developmentBuild = false;
        component.manifest.requiresInstall = true;
        component.manifest.requiresRecoveryPlan = true;
        component.manifest.capabilityScope = {contract.capability};
        component.observation.present = true;
        component.observation.artifactVersion = result.releaseVersion;
        component.observation.architecture = trust::ArtifactArchitecture::X64;
        component.observation.observedSha256 = component.manifest.expectedSha256;
        component.observation.signature = trust::SignatureState::ValidTrustedPublisher;
        component.observation.publisherIdentity = std::string(kPublisher);
        result.components.push_back(std::move(component));
    }
    return result;
}

trust::TrustPolicy policy() {
    trust::TrustPolicy value;
    value.hostArchitecture = trust::ArtifactArchitecture::X64;
    value.requireRedistributionPermission = true;
    value.allowedSourceIds = {"hydraseat-release"};
    for (const auto& contract : contracts) value.allowedCapabilities.push_back(contract.capability);
    value.trustedPublisherIdentities = {std::string(kPublisher)};
    return value;
}

InstalledState stateFrom(const InstallerPackage& pkg, bool profiles = false) {
    InstalledState state;
    state.installedVersion = pkg.releaseVersion;
    state.installedRevision = pkg.releaseRevision;
    state.architecture = pkg.architecture;
    state.uninstallRegistered = true;
    state.startupMode = startup::StartupMode::Manual;
    state.userProfilesPresent = profiles;
    for (const auto& component : pkg.components) {
        state.ownedComponents.push_back({component.component, component.manifest.artifactVersion,
                                         component.observation.observedSha256});
    }
    return state;
}

class FakeExecutor final : public InstallerExecutor {
public:
    std::optional<InstalledState> current;
    std::string transactionId{"installer-transaction"};
    bool beginOk{true};
    bool stageOk{true};
    bool commitOk{true};
    bool verifyOk{true};
    bool rollbackOk{true};
    bool verifyRollbackOk{true};
    bool finishCommittedOk{true};
    bool finishRolledBackOk{true};
    bool finishAbortedOk{true};
    bool mutateBeforeCommitFailure{true};
    bool mutationActive{false};
    int beginCalls{0};
    int stageCalls{0};
    int commitCalls{0};
    int verifyCalls{0};
    int rollbackCalls{0};
    int verifyRollbackCalls{0};
    int finishCalls{0};
    std::optional<InstallerTransactionDisposition> lastDisposition;

    class Transaction final : public InstallerMutationTransaction {
    public:
        explicit Transaction(FakeExecutor& owner) : owner_(owner), previous_(owner.current) {}
        ~Transaction() override { owner_.mutationActive = false; }

        std::string_view transactionId() const noexcept override { return owner_.transactionId; }
        const std::optional<InstalledState>& previousState() const noexcept override { return previous_; }

        bool stageInstallOrRepair(InstallerAction, const InstallerPackage& pkg,
                                  const InstallerOptions& options) noexcept override {
            ++owner_.stageCalls;
            if (!owner_.stageOk) return false;
            const bool profiles = previous_ ? previous_->userProfilesPresent : false;
            staged_ = stateFrom(pkg, profiles);
            staged_->startupMode = options.startupMode;
            return true;
        }

        bool stageUninstall(const InstallerOptions&) noexcept override {
            ++owner_.stageCalls;
            if (!owner_.stageOk) return false;
            staged_.reset();
            return true;
        }

        bool commit() noexcept override {
            ++owner_.commitCalls;
            if (owner_.commitOk) {
                owner_.current = staged_;
                return true;
            }
            if (owner_.mutateBeforeCommitFailure) owner_.current = staged_;
            return false;
        }

        bool verify(const std::optional<InstalledState>& expected) noexcept override {
            ++owner_.verifyCalls;
            return owner_.verifyOk && owner_.current == expected;
        }

        bool rollback() noexcept override {
            ++owner_.rollbackCalls;
            if (!owner_.rollbackOk) return false;
            owner_.current = previous_;
            return true;
        }

        bool verifyRollback() noexcept override {
            ++owner_.verifyRollbackCalls;
            return owner_.verifyRollbackOk && owner_.current == previous_;
        }

        bool finish(InstallerTransactionDisposition disposition) noexcept override {
            ++owner_.finishCalls;
            bool ok = false;
            switch (disposition) {
                case InstallerTransactionDisposition::Committed: ok = owner_.finishCommittedOk; break;
                case InstallerTransactionDisposition::RolledBack: ok = owner_.finishRolledBackOk; break;
                case InstallerTransactionDisposition::Aborted: ok = owner_.finishAbortedOk; break;
            }
            if (ok) owner_.lastDisposition = disposition;
            return ok;
        }

    private:
        FakeExecutor& owner_;
        std::optional<InstalledState> previous_;
        std::optional<InstalledState> staged_;
    };

    std::unique_ptr<InstallerMutationTransaction> beginMutation() noexcept override {
        ++beginCalls;
        if (!beginOk || mutationActive) return {};
        mutationActive = true;
        return std::make_unique<Transaction>(*this);
    }
};

void testInstallRepairUninstallOwnedLifecycle() {
    const auto pkg = package();
    FakeExecutor executor;
    InstallerReceipt receipt;
    InstallerOptions options;
    const auto installResult = executeInstallerTransaction(
        InstallerAction::Install, pkg, InstallerEnvironment{}, options, policy(), executor, receipt);
    if (!installResult.succeeded()) {
        std::cerr << "install fixture diagnostic: " << installerCodeName(installResult.code)
                  << " / " << installResult.message << '\n';
    }
    check(installResult.succeeded() && executor.current.has_value() &&
              executor.current->installedVersion == "1.2.3" &&
              executor.current->ownedComponents.size() == 7u &&
              executor.current->uninstallRegistered && receipt.firstRunWizardOffered &&
              receipt.status == InstallerReceiptStatus::Applied,
          "clean install applies exactly the seven reviewed owned components and registers uninstall");
    if (!executor.current) return;

    executor.current->userProfilesPresent = true;
    check(executeInstallerTransaction(InstallerAction::Repair, pkg, InstallerEnvironment{}, options,
                                      policy(), executor, receipt).succeeded() &&
              executor.current->userProfilesPresent,
          "repair restores reviewed package state while preserving user profile retention state");

    options.retainUserProfilesOnUninstall = true;
    check(executeInstallerTransaction(InstallerAction::Uninstall, std::nullopt, InstallerEnvironment{},
                                      options, policy(), executor, receipt).succeeded() &&
              !executor.current && receipt.userProfilesRetained &&
              receipt.status == InstallerReceiptStatus::Removed,
          "uninstall removes owned install state while honoring explicit profile retention choice");
}

void testTrustAndEnvironmentFailuresDoNotApply() {
    InstallerReceipt receipt;
    InstallerOptions options;
    const auto trusted = package();

    auto runEnvironmentFailure = [&](InstallerEnvironment env, InstallerCode expected) {
        FakeExecutor executor;
        const auto result = executeInstallerTransaction(InstallerAction::Install, trusted, env, options,
                                                        policy(), executor, receipt);
        check(result.code == expected && executor.beginCalls == 0 && executor.stageCalls == 0,
              "environment/preflight failure must occur before installer mutation boundary");
    };
    auto env = InstallerEnvironment{}; env.supportedWindows = false;
    runEnvironmentFailure(env, InstallerCode::UnsupportedWindows);
    env = InstallerEnvironment{}; env.prerequisiteReady = false;
    runEnvironmentFailure(env, InstallerCode::PrerequisiteMissing);
    env = InstallerEnvironment{}; env.elevatedBrokerAvailable = false;
    runEnvironmentFailure(env, InstallerCode::ElevationUnavailable);
    env = InstallerEnvironment{}; env.hydraOwnedRiskyStateActive = true;
    runEnvironmentFailure(env, InstallerCode::ActiveOwnedState);
    env = InstallerEnvironment{}; env.hostOrSeatUiRunning = true;
    runEnvironmentFailure(env, InstallerCode::ProcessStillRunning);

    FakeExecutor executor;
    auto tampered = trusted;
    tampered.components[2].observation.observedSha256 = std::string(64u, 'f');
    check(executeInstallerTransaction(InstallerAction::Install, tampered, InstallerEnvironment{}, options,
                                      policy(), executor, receipt).code ==
              InstallerCode::PackageTrustRejected && executor.beginCalls == 0,
          "signature/hash mismatch is rejected before installer mutation boundary");

    auto wrongPublisher = trusted;
    wrongPublisher.components[2].observation.publisherIdentity = std::string(kWrongPublisher);
    check(executeInstallerTransaction(InstallerAction::Install, wrongPublisher, InstallerEnvironment{}, options,
                                      policy(), executor, receipt).code ==
              InstallerCode::PackageTrustRejected && executor.beginCalls == 0,
          "valid signature status from an unexpected exact publisher is rejected");

    auto wrongIdentity = trusted;
    wrongIdentity.components[0].manifest.artifactId = "foreign-tool";
    check(executeInstallerTransaction(InstallerAction::Install, wrongIdentity, InstallerEnvironment{},
                                      options, policy(), executor, receipt).code ==
              InstallerCode::InvalidPackage && executor.beginCalls == 0,
          "package cannot substitute a foreign executable for a fixed owned component");

    auto wrongType = trusted;
    wrongType.components[0].manifest.artifactClass = trust::ArtifactClass::ProviderHelper;
    check(executeInstallerTransaction(InstallerAction::Install, wrongType, InstallerEnvironment{},
                                      options, policy(), executor, receipt).code ==
              InstallerCode::InvalidPackage && executor.beginCalls == 0,
          "package cannot substitute the wrong artifact type for the fixed product contract");
}

void testStartupAndExistingStateRules() {
    InstallerReceipt receipt;
    InstallerOptions options;
    options.startupMode = startup::StartupMode::BackgroundIdle;
    options.startupApproved = false;
    FakeExecutor executor;
    check(executeInstallerTransaction(InstallerAction::Install, package(), InstallerEnvironment{}, options,
                                      policy(), executor, receipt).code ==
              InstallerCode::StartupApprovalRequired && executor.beginCalls == 0,
          "installer cannot silently enable background startup");

    options.startupApproved = true;
    executor.current = stateFrom(package("1.0.0", 100u));
    check(executeInstallerTransaction(InstallerAction::Install, package(), InstallerEnvironment{},
                                      options, policy(), executor, receipt).code ==
              InstallerCode::InvalidPackage && executor.stageCalls == 0,
          "existing install must use Repair/Update rather than overlapping second install");

    executor.current.reset();
    check(executeInstallerTransaction(InstallerAction::Repair, package(), InstallerEnvironment{}, options,
                                      policy(), executor, receipt).code ==
              InstallerCode::MissingInstalledState && executor.stageCalls == 0,
          "repair without owned install record fails closed");

    FakeExecutor malformedStateExecutor;
    malformedStateExecutor.current = stateFrom(package("1.0.0", 100u));
    malformedStateExecutor.current->ownedComponents[0].sha256 = std::string(64u, 'g');
    InstallerOptions manualOptions;
    check(executeInstallerTransaction(InstallerAction::Repair, package(), InstallerEnvironment{},
                                      manualOptions, policy(), malformedStateExecutor, receipt).code ==
              InstallerCode::CaptureFailed && malformedStateExecutor.stageCalls == 0,
          "captured install state rejects non-hex SHA-256 metadata before any repair mutation");

    FakeExecutor mixedVersionExecutor;
    mixedVersionExecutor.current = stateFrom(package("1.0.0", 100u));
    mixedVersionExecutor.current->ownedComponents[0].version = "0.9.0";
    check(executeInstallerTransaction(InstallerAction::Repair, package(), InstallerEnvironment{},
                                      manualOptions, policy(), mixedVersionExecutor, receipt).code ==
              InstallerCode::CaptureFailed && mixedVersionExecutor.stageCalls == 0,
          "captured install state rejects mixed component versions before repair mutation");

    FakeExecutor invalidStoredStartupExecutor;
    invalidStoredStartupExecutor.current = stateFrom(package("1.0.0", 100u));
    invalidStoredStartupExecutor.current->startupMode = static_cast<startup::StartupMode>(0xffu);
    check(executeInstallerTransaction(InstallerAction::Repair, package(), InstallerEnvironment{},
                                      manualOptions, policy(), invalidStoredStartupExecutor, receipt).code ==
              InstallerCode::CaptureFailed && invalidStoredStartupExecutor.stageCalls == 0,
          "captured install state rejects an invalid persisted startup mode");

    InstallerOptions invalidOptions;
    invalidOptions.startupMode = static_cast<startup::StartupMode>(0xffu);
    invalidOptions.startupApproved = true;
    FakeExecutor invalidOptionExecutor;
    check(executeInstallerTransaction(InstallerAction::Install, package(), InstallerEnvironment{},
                                      invalidOptions, policy(), invalidOptionExecutor, receipt).code ==
              InstallerCode::InvalidPackage && invalidOptionExecutor.beginCalls == 0,
          "installer rejects an invalid requested startup mode before mutation boundary");
}

void testCompareAndSwapStagingCompetitionAndReceipt() {
    const auto previousPackage = package("1.0.0", 100u);
    const auto previous = stateFrom(previousPackage);
    const auto target = package("1.1.0", 110u);
    InstallerOptions options;
    InstallerReceipt receipt;

    FakeExecutor matching;
    matching.current = previous;
    const auto committed = executeInstallerTransactionCompareAndSwap(
        InstallerAction::Repair, target, previous, InstallerEnvironment{}, options, policy(), matching, receipt);
    const auto expectedTarget = stateFrom(target);
    check(committed.succeeded() && matching.current == expectedTarget && matching.stageCalls == 1 &&
              matching.commitCalls == 1 && receipt.status == InstallerReceiptStatus::Applied &&
              receipt.previousState == std::optional<InstalledState>{previous} &&
              receipt.resultingState == std::optional<InstalledState>{expectedTarget} &&
              receipt.committedArtifacts.size() == contracts.size() &&
              receipt.committedArtifacts.front().artifactId == "hydraseat-main-ui" &&
              receipt.committedArtifacts.front().sha256 == target.components.front().observation.observedSha256 &&
              receipt.committedArtifacts.front().publisherIdentity == kPublisher,
          "matching expected previous identity commits and receipt records exact old/new artifact states");

    FakeExecutor changed;
    changed.current = stateFrom(package("1.2.0", 120u));
    const auto stale = executeInstallerTransactionCompareAndSwap(
        InstallerAction::Repair, target, previous, InstallerEnvironment{}, options, policy(), changed, receipt);
    check(stale.code == InstallerCode::ExpectedPreviousMismatch && changed.stageCalls == 0 &&
              changed.commitCalls == 0 && changed.rollbackCalls == 0 &&
              changed.current->installedRevision == 120u && receipt.status == InstallerReceiptStatus::Rejected,
          "authoritative previous state changed before commit is rejected before staging");

    FakeExecutor locked;
    locked.current = previous;
    auto heldMutation = locked.beginMutation();
    const auto lockContender = executeInstallerTransactionCompareAndSwap(
        InstallerAction::Repair, target, previous, InstallerEnvironment{}, options, policy(), locked, receipt);
    check(heldMutation && lockContender.code == InstallerCode::CaptureFailed && locked.stageCalls == 0 &&
              locked.commitCalls == 0 && locked.current == previous,
          "concurrent updater cannot enter the authoritative mutation boundary while another transaction owns it");
    heldMutation.reset();

    FakeExecutor competing;
    competing.current = previous;
    InstallerReceipt firstReceipt;
    InstallerReceipt secondReceipt;
    const auto first = executeInstallerTransactionCompareAndSwap(
        InstallerAction::Repair, target, previous, InstallerEnvironment{}, options, policy(), competing,
        firstReceipt);
    const auto secondTarget = package("1.2.0", 120u);
    const auto second = executeInstallerTransactionCompareAndSwap(
        InstallerAction::Repair, secondTarget, previous, InstallerEnvironment{}, options, policy(), competing,
        secondReceipt);
    check(first.succeeded() && second.code == InstallerCode::ExpectedPreviousMismatch &&
              competing.current && competing.current->installedRevision == 110u &&
              competing.stageCalls == 1 && competing.commitCalls == 1,
          "two competing expected-previous transactions allow one winner and fail the stale writer closed");

    FakeExecutor stagingFailure;
    stagingFailure.current = previous;
    stagingFailure.stageOk = false;
    const auto stageFailed = executeInstallerTransactionCompareAndSwap(
        InstallerAction::Repair, target, previous, InstallerEnvironment{}, options, policy(), stagingFailure,
        receipt);
    check(stageFailed.code == InstallerCode::StageFailed && stagingFailure.current == previous &&
              stagingFailure.commitCalls == 0 && stagingFailure.rollbackCalls == 0 &&
              receipt.status == InstallerReceiptStatus::Rejected,
          "staging failure leaves the previously committed state untouched");
}

void testCommitVerifyAndRecoveryFailures() {
    const auto previous = stateFrom(package("1.0.0", 100u));
    const auto target = package("1.1.0", 110u);
    InstallerOptions options;
    InstallerReceipt receipt;

    {
        FakeExecutor executor;
        executor.current = previous;
        executor.commitOk = false;
        const auto result = executeInstallerTransactionCompareAndSwap(
            InstallerAction::Repair, target, previous, InstallerEnvironment{}, options, policy(), executor,
            receipt);
        check(result.code == InstallerCode::ApplyFailedRolledBack && executor.current == previous &&
                  executor.rollbackCalls == 1 && receipt.rollbackAttempted && receipt.rollbackVerified &&
                  receipt.status == InstallerReceiptStatus::RolledBack,
              "commit failure restores and verifies the previous known-good state");
    }
    {
        FakeExecutor executor;
        executor.current = previous;
        executor.verifyOk = false;
        const auto result = executeInstallerTransactionCompareAndSwap(
            InstallerAction::Repair, target, previous, InstallerEnvironment{}, options, policy(), executor,
            receipt);
        check(result.code == InstallerCode::VerifyFailedRolledBack && executor.current == previous &&
                  receipt.rollbackVerified && receipt.status == InstallerReceiptStatus::RolledBack,
              "post-commit verification failure rolls back the committed candidate");
    }
    {
        FakeExecutor executor;
        executor.current = previous;
        executor.commitOk = false;
        executor.rollbackOk = false;
        const auto result = executeInstallerTransactionCompareAndSwap(
            InstallerAction::Repair, target, previous, InstallerEnvironment{}, options, policy(), executor,
            receipt);
        check(result.code == InstallerCode::RollbackFailed && receipt.rollbackAttempted &&
                  !receipt.rollbackVerified && receipt.status == InstallerReceiptStatus::RecoveryRequired &&
                  !receipt.resultingState,
              "failed rollback is explicit recovery-required with unresolved state retained for recovery");
    }
    {
        FakeExecutor executor;
        executor.current = previous;
        executor.finishCommittedOk = false;
        const auto result = executeInstallerTransactionCompareAndSwap(
            InstallerAction::Repair, target, previous, InstallerEnvironment{}, options, policy(), executor,
            receipt);
        check(result.code == InstallerCode::TransactionFinalizeFailed && executor.current == previous &&
                  receipt.status == InstallerReceiptStatus::RolledBack && receipt.rollbackVerified,
              "failed durable commit finalization restores prior state instead of reporting false success");
    }
}

void testArchitectureAndUninstallPackageRules() {
    InstallerReceipt receipt;
    InstallerOptions options;
    FakeExecutor executor;
    auto env = InstallerEnvironment{};
    env.architecture = InstallerArchitecture::X86;
    check(executeInstallerTransaction(InstallerAction::Install, package(), env, options, policy(),
                                      executor, receipt).code == InstallerCode::ArchitectureMismatch,
          "x64 package cannot silently install on x86 target");

    executor.current = stateFrom(package());
    check(executeInstallerTransaction(InstallerAction::Uninstall, package(), InstallerEnvironment{},
                                      options, policy(), executor, receipt).code ==
              InstallerCode::InvalidPackage,
          "uninstall cannot carry replacement package metadata or touch unrelated package files");
}

} // namespace

int main() {
    testInstallRepairUninstallOwnedLifecycle();
    testTrustAndEnvironmentFailuresDoNotApply();
    testStartupAndExistingStateRules();
    testCompareAndSwapStagingCompetitionAndReceipt();
    testCommitVerifyAndRecoveryFailures();
    testArchitectureAndUninstallPackageRules();
    if (failures) {
        std::cerr << failures << " installer transaction test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Installer transaction tests passed.\n";
    return EXIT_SUCCESS;
}
