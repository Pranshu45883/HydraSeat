#include "hydra/privilege_broker.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace hydra::privilege {
namespace {

BrokerDiagnostic fail(BrokerCode code, std::string message) {
    return {code, std::move(message)};
}

bool validOpaqueIdentity(std::string_view value, std::size_t maximum) noexcept {
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

bool validResource(BrokerResource value) noexcept {
    return value == BrokerResource::RuntimeService || value == BrokerResource::WatchdogService ||
           value == BrokerResource::RecoveryTool;
}

bool validOperation(BrokerOperation value) noexcept {
    return value == BrokerOperation::Install || value == BrokerOperation::Repair ||
           value == BrokerOperation::Remove;
}

struct ResourceContract {
    std::string_view artifactId;
    std::string_view requiredCapability;
};

ResourceContract resourceContract(BrokerResource resource) noexcept {
    switch (resource) {
        case BrokerResource::RuntimeService:
            return {"hydraseat-runtime-service", "runtime-service"};
        case BrokerResource::WatchdogService:
            return {"hydraseat-watchdog-service", "watchdog-service"};
        case BrokerResource::RecoveryTool:
            return {"hydraseat-recovery-tool", "recovery-tool"};
    }
    return {};
}

bool contains(const std::vector<std::string>& values, std::string_view expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

BrokerDiagnostic validateArtifactForResource(const BrokerRequest& request,
                                             const BrokerPolicy& policy) {
    if (request.operation == BrokerOperation::Remove) {
        if (request.artifactManifest || request.artifactObservation) {
            return fail(BrokerCode::UnexpectedArtifact,
                        "remove request cannot carry caller-selected artifact metadata");
        }
        return {};
    }

    if (!request.artifactManifest || !request.artifactObservation) {
        return fail(BrokerCode::MissingArtifact,
                    "install/repair requires one exact trusted artifact observation");
    }

    const auto& manifest = *request.artifactManifest;
    const auto contract = resourceContract(request.resource);
    if (manifest.artifactId != contract.artifactId) {
        return fail(BrokerCode::ArtifactIdentityMismatch,
                    "artifact identity does not match the broker-owned resource contract");
    }
    if (manifest.artifactClass != trust::ArtifactClass::Executable) {
        return fail(BrokerCode::ArtifactClassMismatch,
                    "privileged v1 owned component install accepts only executable artifacts");
    }
    if (!manifest.requiresInstall || !manifest.requiresRecoveryPlan) {
        return fail(BrokerCode::ArtifactTrustRejected,
                    "privileged installed artifact must declare install and recovery requirements");
    }
    if (!contains(manifest.capabilityScope, contract.requiredCapability) ||
        manifest.capabilityScope.size() != 1u) {
        return fail(BrokerCode::ArtifactCapabilityMismatch,
                    "artifact capability scope does not exactly match the broker resource");
    }

    const auto trustEvaluation =
        trust::evaluateArtifact(manifest, *request.artifactObservation, policy.artifactTrust);
    if (trustEvaluation.decision != trust::TrustDecision::Accept) {
        return fail(BrokerCode::ArtifactTrustRejected,
                    std::string("artifact trust rejected: ") +
                        std::string(trust::trustCodeName(trustEvaluation.code)));
    }
    return {};
}

} // namespace

BrokerDiagnostic PrivilegeBrokerSession::authorize(const BrokerRequest& request,
                                                    const AuthenticatedPeer& peer) const {
    if (request.schemaVersion != kPrivilegeBrokerSchemaVersion) {
        return fail(BrokerCode::InvalidSchema, "unsupported privilege broker request schema");
    }
    if (!validOpaqueIdentity(request.requestId, kMaximumBrokerRequestIdBytes) ||
        request.channelNonce == 0u || request.sequence == 0u) {
        return fail(BrokerCode::InvalidRequestIdentity,
                    "request identity/correlation fields are invalid or unbounded");
    }
    if (!validOpaqueIdentity(peer.callerUserSid, kMaximumBrokerIdentityBytes) ||
        !validOpaqueIdentity(peer.brokerOwnerUserSid, kMaximumBrokerIdentityBytes) ||
        peer.channelNonce == 0u) {
        return fail(BrokerCode::InvalidPeerIdentity,
                    "authenticated peer identity/correlation is invalid");
    }
    if (!peer.brokerProcessElevated) {
        return fail(BrokerCode::BrokerNotElevated,
                    "privilege request cannot run in a non-elevated broker process");
    }
    if (peer.callerUserSid != peer.brokerOwnerUserSid) {
        return fail(BrokerCode::WrongUser,
                    "authenticated caller is not the broker-owning user");
    }
    if (request.channelNonce != peer.channelNonce) {
        return fail(BrokerCode::WrongChannel,
                    "request correlation does not match the authenticated broker channel");
    }
    if (request.sequence != nextSequence_) {
        return fail(BrokerCode::ReplayOrOutOfOrder,
                    "request sequence is stale, replayed, or out of order");
    }
    if (!validResource(request.resource)) {
        return fail(BrokerCode::InvalidResource,
                    "request resource is outside the compiled broker allowlist");
    }
    if (!validOperation(request.operation)) {
        return fail(BrokerCode::InvalidOperation,
                    "request operation is outside the compiled broker allowlist");
    }
    return validateArtifactForResource(request, policy_);
}

BrokerDiagnostic PrivilegeBrokerSession::execute(const BrokerRequest& request,
                                                 const AuthenticatedPeer& peer,
                                                 PrivilegedArtifactAuthority& artifactAuthority,
                                                 PrivilegedMutationExecutor& executor,
                                                 BrokerReceipt& receipt) {
    const auto authorized = authorize(request, peer);
    if (!authorized.succeeded()) return authorized;

    std::optional<trust::ArtifactManifest> authoritativeManifest;
    std::optional<trust::ArtifactObservation> authoritativeObservation;
    if (request.operation != BrokerOperation::Remove) {
        trust::ArtifactManifest resolvedManifest;
        trust::ArtifactObservation resolvedObservation;
        if (!artifactAuthority.resolve(request.resource, resolvedManifest,
                                       resolvedObservation)) {
            return fail(BrokerCode::ArtifactAuthorityUnavailable,
                        "broker-owned artifact manifest or file observation is unavailable");
        }

        BrokerRequest authoritativeRequest = request;
        authoritativeRequest.artifactManifest = resolvedManifest;
        authoritativeRequest.artifactObservation = resolvedObservation;
        const auto authoritativeTrust =
            validateArtifactForResource(authoritativeRequest, policy_);
        if (!authoritativeTrust.succeeded()) return authoritativeTrust;

        if (request.artifactManifest != authoritativeRequest.artifactManifest ||
            request.artifactObservation != authoritativeRequest.artifactObservation) {
            return fail(BrokerCode::ArtifactAuthorityMismatch,
                        "caller artifact claim does not exactly match broker-owned evidence");
        }
        authoritativeManifest = std::move(resolvedManifest);
        authoritativeObservation = std::move(resolvedObservation);
    }

    std::string snapshotId;
    if (!executor.capture(request.resource, snapshotId) ||
        !validOpaqueIdentity(snapshotId, kMaximumBrokerRequestIdBytes)) {
        return fail(BrokerCode::CaptureFailed,
                    "broker-owned mutation state could not be captured before apply");
    }

    // The authenticated/authorized request is now consumed even if the executor
    // later fails; explicit retry must use a new monotonic request sequence.
    ++nextSequence_;

    BrokerReceipt candidate;
    candidate.requestId = request.requestId;
    candidate.sequence = request.sequence;
    candidate.resource = request.resource;
    candidate.operation = request.operation;

    const auto rollbackAfterFailure = [&](BrokerCode appliedFailure) -> BrokerDiagnostic {
        candidate.rollbackAttempted = true;
        if (!executor.rollback(request.resource, snapshotId)) {
            candidate.status = BrokerReceiptStatus::RecoveryRequired;
            candidate.rollbackVerified = false;
            receipt = candidate;
            return fail(BrokerCode::RollbackFailed,
                        "privileged mutation failed and broker-owned rollback failed");
        }
        if (!executor.verifyRollback(request.resource, snapshotId)) {
            candidate.status = BrokerReceiptStatus::RecoveryRequired;
            candidate.rollbackVerified = false;
            receipt = candidate;
            return fail(BrokerCode::RollbackVerifyFailed,
                        "privileged rollback ran but post-rollback verification failed");
        }
        candidate.status = BrokerReceiptStatus::RolledBack;
        candidate.rollbackVerified = true;
        receipt = candidate;
        return fail(appliedFailure,
                    appliedFailure == BrokerCode::ApplyFailedRolledBack
                        ? "privileged mutation apply failed and prior state was verified restored"
                        : "privileged mutation verification failed and prior state was verified restored");
    };

    if (!executor.apply(request.resource, request.operation, authoritativeManifest,
                        authoritativeObservation, snapshotId)) {
        return rollbackAfterFailure(BrokerCode::ApplyFailedRolledBack);
    }
    if (!executor.verify(request.resource, request.operation)) {
        return rollbackAfterFailure(BrokerCode::VerifyFailedRolledBack);
    }

    candidate.status = request.operation == BrokerOperation::Remove
                           ? BrokerReceiptStatus::Removed
                           : BrokerReceiptStatus::Applied;
    receipt = std::move(candidate);
    return {};
}

std::string_view brokerResourceName(BrokerResource value) noexcept {
    switch (value) {
        case BrokerResource::RuntimeService: return "RuntimeService";
        case BrokerResource::WatchdogService: return "WatchdogService";
        case BrokerResource::RecoveryTool: return "RecoveryTool";
    }
    return "Unknown";
}

std::string_view brokerOperationName(BrokerOperation value) noexcept {
    switch (value) {
        case BrokerOperation::Install: return "Install";
        case BrokerOperation::Repair: return "Repair";
        case BrokerOperation::Remove: return "Remove";
    }
    return "Unknown";
}

std::string_view brokerCodeName(BrokerCode value) noexcept {
    switch (value) {
        case BrokerCode::Success: return "Success";
        case BrokerCode::InvalidSchema: return "InvalidSchema";
        case BrokerCode::InvalidRequestIdentity: return "InvalidRequestIdentity";
        case BrokerCode::InvalidPeerIdentity: return "InvalidPeerIdentity";
        case BrokerCode::BrokerNotElevated: return "BrokerNotElevated";
        case BrokerCode::WrongUser: return "WrongUser";
        case BrokerCode::WrongChannel: return "WrongChannel";
        case BrokerCode::ReplayOrOutOfOrder: return "ReplayOrOutOfOrder";
        case BrokerCode::InvalidResource: return "InvalidResource";
        case BrokerCode::InvalidOperation: return "InvalidOperation";
        case BrokerCode::UnexpectedArtifact: return "UnexpectedArtifact";
        case BrokerCode::MissingArtifact: return "MissingArtifact";
        case BrokerCode::ArtifactIdentityMismatch: return "ArtifactIdentityMismatch";
        case BrokerCode::ArtifactClassMismatch: return "ArtifactClassMismatch";
        case BrokerCode::ArtifactCapabilityMismatch: return "ArtifactCapabilityMismatch";
        case BrokerCode::ArtifactTrustRejected: return "ArtifactTrustRejected";
        case BrokerCode::ArtifactAuthorityUnavailable: return "ArtifactAuthorityUnavailable";
        case BrokerCode::ArtifactAuthorityMismatch: return "ArtifactAuthorityMismatch";
        case BrokerCode::CaptureFailed: return "CaptureFailed";
        case BrokerCode::ApplyFailedRolledBack: return "ApplyFailedRolledBack";
        case BrokerCode::VerifyFailedRolledBack: return "VerifyFailedRolledBack";
        case BrokerCode::RollbackFailed: return "RollbackFailed";
        case BrokerCode::RollbackVerifyFailed: return "RollbackVerifyFailed";
    }
    return "Unknown";
}

std::string_view brokerReceiptStatusName(BrokerReceiptStatus value) noexcept {
    switch (value) {
        case BrokerReceiptStatus::Applied: return "Applied";
        case BrokerReceiptStatus::Removed: return "Removed";
        case BrokerReceiptStatus::RolledBack: return "RolledBack";
        case BrokerReceiptStatus::RecoveryRequired: return "RecoveryRequired";
    }
    return "Unknown";
}

} // namespace hydra::privilege
