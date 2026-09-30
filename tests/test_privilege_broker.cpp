#include "hydra/privilege_broker.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::privilege;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string artifactId(BrokerResource resource) {
    switch (resource) {
        case BrokerResource::RuntimeService: return "hydraseat-runtime-service";
        case BrokerResource::WatchdogService: return "hydraseat-watchdog-service";
        case BrokerResource::RecoveryTool: return "hydraseat-recovery-tool";
    }
    return "invalid";
}

std::string capability(BrokerResource resource) {
    switch (resource) {
        case BrokerResource::RuntimeService: return "runtime-service";
        case BrokerResource::WatchdogService: return "watchdog-service";
        case BrokerResource::RecoveryTool: return "recovery-tool";
    }
    return "invalid";
}

trust::ArtifactManifest manifest(BrokerResource resource) {
    trust::ArtifactManifest value;
    value.artifactId = artifactId(resource);
    value.artifactClass = trust::ArtifactClass::Executable;
    value.artifactVersion = "1.0.0";
    value.architecture = trust::ArtifactArchitecture::X64;
    value.expectedSha256 = std::string(64u, 'a');
    value.sourceId = "hydraseat-release";
    value.licenseId = "hydraseat-project";
    value.redistributionAllowed = true;
    value.optional = false;
    value.developmentBuild = false;
    value.requiresInstall = true;
    value.requiresRestart = false;
    value.requiresRecoveryPlan = true;
    value.capabilityScope = {capability(resource)};
    return value;
}

trust::ArtifactObservation observation() {
    trust::ArtifactObservation value;
    value.present = true;
    value.artifactVersion = "1.0.0";
    value.architecture = trust::ArtifactArchitecture::X64;
    value.observedSha256 = std::string(64u, 'a');
    value.signature = trust::SignatureState::ValidTrustedPublisher;
    value.publisherIdentity = "0123456789ABCDEF0123456789ABCDEF01234567";
    return value;
}

BrokerPolicy policy() {
    BrokerPolicy value;
    value.artifactTrust.hostArchitecture = trust::ArtifactArchitecture::X64;
    value.artifactTrust.allowUnsignedDevelopmentExecutables = false;
    value.artifactTrust.requireRedistributionPermission = false;
    value.artifactTrust.allowedSourceIds = {"hydraseat-release"};
    value.artifactTrust.allowedCapabilities = {
        "runtime-service", "watchdog-service", "recovery-tool"};
    value.artifactTrust.trustedPublisherIdentities = {
        "0123456789ABCDEF0123456789ABCDEF01234567"};
    return value;
}

AuthenticatedPeer peer() {
    return {"S-1-5-21-1000", "S-1-5-21-1000", 0x12345678u, true};
}

BrokerRequest installRequest(BrokerResource resource = BrokerResource::RuntimeService,
                             std::uint64_t sequence = 1u) {
    BrokerRequest request;
    request.requestId = "request-" + std::to_string(sequence);
    request.channelNonce = 0x12345678u;
    request.sequence = sequence;
    request.resource = resource;
    request.operation = BrokerOperation::Install;
    request.artifactManifest = manifest(resource);
    request.artifactObservation = observation();
    return request;
}

class FakeArtifactAuthority final : public PrivilegedArtifactAuthority {
public:
    bool resolveResult{true};
    int resolveCalls{0};
    std::optional<trust::ArtifactManifest> resolvedManifest;
    std::optional<trust::ArtifactObservation> resolvedObservation;

    bool resolve(BrokerResource resource,
                 trust::ArtifactManifest& outputManifest,
                 trust::ArtifactObservation& outputObservation) noexcept override {
        ++resolveCalls;
        if (!resolveResult) return false;
        outputManifest = resolvedManifest.value_or(manifest(resource));
        outputObservation = resolvedObservation.value_or(observation());
        return true;
    }
};

class FakeExecutor final : public PrivilegedMutationExecutor {
public:
    bool captureResult{true};
    bool applyResult{true};
    bool verifyResult{true};
    bool rollbackResult{true};
    bool verifyRollbackResult{true};
    std::string snapshot{"snapshot-1"};

    int captureCalls{0};
    int applyCalls{0};
    int verifyCalls{0};
    int rollbackCalls{0};
    int verifyRollbackCalls{0};
    std::optional<BrokerResource> lastResource;
    std::optional<BrokerOperation> lastOperation;

    bool capture(BrokerResource resource, std::string& snapshotId) noexcept override {
        ++captureCalls;
        lastResource = resource;
        if (!captureResult) return false;
        snapshotId = snapshot;
        return true;
    }

    bool apply(BrokerResource resource,
               BrokerOperation operation,
               const std::optional<trust::ArtifactManifest>&,
               const std::optional<trust::ArtifactObservation>&,
               std::string_view) noexcept override {
        ++applyCalls;
        lastResource = resource;
        lastOperation = operation;
        return applyResult;
    }

    bool verify(BrokerResource resource, BrokerOperation operation) noexcept override {
        ++verifyCalls;
        lastResource = resource;
        lastOperation = operation;
        return verifyResult;
    }

    bool rollback(BrokerResource resource, std::string_view) noexcept override {
        ++rollbackCalls;
        lastResource = resource;
        return rollbackResult;
    }

    bool verifyRollback(BrokerResource resource, std::string_view) noexcept override {
        ++verifyRollbackCalls;
        lastResource = resource;
        return verifyRollbackResult;
    }
};

void testTrustedTypedInstallAndRemove() {
    PrivilegeBrokerSession session(policy());
    FakeArtifactAuthority authority;
    FakeExecutor executor;
    BrokerReceipt receipt;
    check(session.execute(installRequest(), peer(), authority, executor, receipt).succeeded() &&
              receipt.status == BrokerReceiptStatus::Applied &&
              authority.resolveCalls == 1 &&
              executor.captureCalls == 1 && executor.applyCalls == 1 && executor.verifyCalls == 1 &&
              executor.rollbackCalls == 0 && session.nextExpectedSequence() == 2u,
          "trusted typed install captures/applies/verifies once without rollback");

    BrokerRequest remove;
    remove.requestId = "request-2";
    remove.channelNonce = 0x12345678u;
    remove.sequence = 2u;
    remove.resource = BrokerResource::RuntimeService;
    remove.operation = BrokerOperation::Remove;
    check(session.execute(remove, peer(), authority, executor, receipt).succeeded() &&
              receipt.status == BrokerReceiptStatus::Removed &&
              authority.resolveCalls == 1 &&
              session.nextExpectedSequence() == 3u,
          "typed remove needs no caller-selected artifact/path and advances sequence");
}

void testPeerChannelAndReplayAuthorizationFailBeforeExecutor() {
    FakeArtifactAuthority authority;
    FakeExecutor executor;
    BrokerReceipt receipt;

    {
        PrivilegeBrokerSession session(policy());
        auto wrongUser = peer();
        wrongUser.callerUserSid = "S-1-5-21-2000";
        check(session.execute(installRequest(), wrongUser, authority, executor, receipt).code ==
                  BrokerCode::WrongUser && executor.captureCalls == 0,
              "different authenticated Windows user cannot use the broker session");
    }
    {
        PrivilegeBrokerSession session(policy());
        auto wrongChannel = peer();
        wrongChannel.channelNonce = 55u;
        check(session.execute(installRequest(), wrongChannel, authority, executor, receipt).code ==
                  BrokerCode::WrongChannel && executor.captureCalls == 0,
              "request must correlate to the authenticated broker channel");
    }
    {
        PrivilegeBrokerSession session(policy());
        auto notElevated = peer();
        notElevated.brokerProcessElevated = false;
        check(session.execute(installRequest(), notElevated, authority, executor, receipt).code ==
                  BrokerCode::BrokerNotElevated && executor.captureCalls == 0,
              "broker refuses privileged mutation if native broker process is not elevated");
    }
    {
        PrivilegeBrokerSession session(policy(), 2u);
        check(session.execute(installRequest(BrokerResource::RuntimeService, 1u), peer(), authority, executor,
                              receipt).code == BrokerCode::ReplayOrOutOfOrder &&
                  executor.captureCalls == 0,
              "stale/replayed request sequence is rejected before mutation capture");
    }
}

void testNoGeneralCommandOrCallerSelectedPathSurface() {
    PrivilegeBrokerSession session(policy());
    FakeArtifactAuthority authority;
    FakeExecutor executor;
    BrokerReceipt receipt;
    auto request = installRequest();
    request.requestId = "cmd.exe /c whoami";
    check(session.execute(request, peer(), authority, executor, receipt).code ==
              BrokerCode::InvalidRequestIdentity && executor.captureCalls == 0,
          "shell-like text cannot hide in the only caller-provided request identity field");

    request = installRequest();
    request.resource = static_cast<BrokerResource>(255u);
    check(session.execute(request, peer(), authority, executor, receipt).code == BrokerCode::InvalidResource &&
              executor.captureCalls == 0,
          "unknown resource cannot be converted into an arbitrary file/registry/service target");

    request = installRequest();
    request.operation = static_cast<BrokerOperation>(255u);
    check(session.execute(request, peer(), authority, executor, receipt).code == BrokerCode::InvalidOperation &&
              executor.captureCalls == 0,
          "unknown operation cannot turn the broker into a general administrator executor");
}

void testResourceArtifactAndTrustPolicyAreBrokerOwned() {
    FakeArtifactAuthority authority;
    FakeExecutor executor;
    BrokerReceipt receipt;

    {
        PrivilegeBrokerSession session(policy());
        auto request = installRequest(BrokerResource::WatchdogService);
        request.artifactManifest->artifactId = "other.exe";
        check(session.execute(request, peer(), authority, executor, receipt).code ==
                  BrokerCode::ArtifactIdentityMismatch && executor.captureCalls == 0,
              "caller cannot substitute another executable for a fixed broker resource");
    }
    {
        PrivilegeBrokerSession session(policy());
        auto request = installRequest();
        request.artifactManifest->capabilityScope = {"watchdog-service"};
        check(session.execute(request, peer(), authority, executor, receipt).code ==
                  BrokerCode::ArtifactCapabilityMismatch && executor.captureCalls == 0,
              "artifact capabilities must exactly match the compiled resource contract");
    }
    {
        PrivilegeBrokerSession session(policy());
        auto request = installRequest();
        request.artifactObservation->observedSha256 = std::string(64u, 'b');
        check(session.execute(request, peer(), authority, executor, receipt).code ==
                  BrokerCode::ArtifactTrustRejected && executor.captureCalls == 0,
              "tampered executable is rejected by broker-owned P8 trust policy before mutation");
    }
    {
        PrivilegeBrokerSession session(policy());
        auto request = installRequest();
        request.artifactObservation->signature = trust::SignatureState::UnknownPublisher;
        check(session.execute(request, peer(), authority, executor, receipt).code ==
                  BrokerCode::ArtifactTrustRejected && executor.captureCalls == 0,
              "unknown publisher executable cannot be installed by privileged broker");
    }
    {
        PrivilegeBrokerSession session(policy());
        auto request = installRequest();
        request.operation = BrokerOperation::Remove;
        check(session.execute(request, peer(), authority, executor, receipt).code ==
                  BrokerCode::UnexpectedArtifact && executor.captureCalls == 0,
              "remove cannot smuggle a caller-selected executable into the elevated boundary");
    }
}

void testCallerCannotForgeSelfConsistentArtifactEvidence() {
    FakeExecutor executor;
    BrokerReceipt receipt;

    {
        PrivilegeBrokerSession session(policy());
        FakeArtifactAuthority authority;
        auto request = installRequest();
        request.artifactManifest->expectedSha256 = std::string(64u, 'b');
        request.artifactObservation->observedSha256 = std::string(64u, 'b');
        const auto result = session.execute(request, peer(), authority, executor, receipt);
        check(result.code == BrokerCode::ArtifactAuthorityMismatch &&
                  authority.resolveCalls == 1 && executor.captureCalls == 0,
              "self-consistent caller hash/signature claims cannot replace broker-owned evidence");
    }
    {
        PrivilegeBrokerSession session(policy());
        FakeArtifactAuthority authority;
        authority.resolveResult = false;
        const auto result = session.execute(
            installRequest(), peer(), authority, executor, receipt);
        check(result.code == BrokerCode::ArtifactAuthorityUnavailable &&
                  executor.captureCalls == 0 && session.nextExpectedSequence() == 1u,
              "failed broker-owned observation prevents capture and remains safely retryable");
    }
    {
        PrivilegeBrokerSession session(policy());
        FakeArtifactAuthority authority;
        authority.resolvedObservation = observation();
        authority.resolvedObservation->observedSha256 = std::string(64u, 'c');
        const auto result = session.execute(
            installRequest(), peer(), authority, executor, receipt);
        check(result.code == BrokerCode::ArtifactTrustRejected &&
                  executor.captureCalls == 0,
              "tampered broker-observed artifact fails trust before privileged capture");
    }
}

void testFailuresRollbackOrEscalateRecovery() {
    BrokerReceipt receipt;
    FakeArtifactAuthority authority;

    {
        PrivilegeBrokerSession session(policy());
        FakeExecutor executor;
        executor.applyResult = false;
        const auto result = session.execute(installRequest(), peer(), authority, executor, receipt);
        check(result.code == BrokerCode::ApplyFailedRolledBack &&
                  receipt.status == BrokerReceiptStatus::RolledBack &&
                  receipt.rollbackAttempted && receipt.rollbackVerified &&
                  executor.rollbackCalls == 1 && executor.verifyRollbackCalls == 1,
              "partial apply failure restores and verifies prior broker-owned state");
    }
    {
        PrivilegeBrokerSession session(policy());
        FakeExecutor executor;
        executor.verifyResult = false;
        const auto result = session.execute(installRequest(), peer(), authority, executor, receipt);
        check(result.code == BrokerCode::VerifyFailedRolledBack &&
                  receipt.status == BrokerReceiptStatus::RolledBack &&
                  executor.rollbackCalls == 1 && executor.verifyRollbackCalls == 1,
              "post-apply verification failure restores prior state");
    }
    {
        PrivilegeBrokerSession session(policy());
        FakeExecutor executor;
        executor.applyResult = false;
        executor.rollbackResult = false;
        const auto result = session.execute(installRequest(), peer(), authority, executor, receipt);
        check(result.code == BrokerCode::RollbackFailed &&
                  receipt.status == BrokerReceiptStatus::RecoveryRequired &&
                  receipt.rollbackAttempted && !receipt.rollbackVerified,
              "rollback failure is explicit RecoveryRequired rather than false success");
    }
    {
        PrivilegeBrokerSession session(policy());
        FakeExecutor executor;
        executor.captureResult = false;
        const auto result = session.execute(installRequest(), peer(), authority, executor, receipt);
        check(result.code == BrokerCode::CaptureFailed && session.nextExpectedSequence() == 1u &&
                  executor.applyCalls == 0,
              "capture failure performs no mutation and does not consume retry sequence");
    }
}

} // namespace

int main() {
    testTrustedTypedInstallAndRemove();
    testPeerChannelAndReplayAuthorizationFailBeforeExecutor();
    testNoGeneralCommandOrCallerSelectedPathSurface();
    testResourceArtifactAndTrustPolicyAreBrokerOwned();
    testCallerCannotForgeSelfConsistentArtifactEvidence();
    testFailuresRollbackOrEscalateRecovery();
    if (failures != 0) {
        std::cerr << failures << " privilege broker test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Privilege broker tests passed.\n";
    return EXIT_SUCCESS;
}
