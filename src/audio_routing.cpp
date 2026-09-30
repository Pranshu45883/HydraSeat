#include "hydra/audio_routing.hpp"

#include <algorithm>
#include <cwctype>
#include <set>
#include <tuple>
#include <utility>

namespace hydra::audio {
namespace {

std::wstring canonical(std::wstring_view value) {
    std::wstring result(value);
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    return result;
}

bool sameEndpoint(std::wstring_view left, std::wstring_view right) {
    return canonical(left) == canonical(right);
}

bool processTreeContainsPid(
    const process::ProcessTreeSnapshot& tree,
    std::uint32_t processId) noexcept {
    if (processId == 0) return false;
    if (tree.root.processId == processId) return true;
    return std::any_of(
        tree.processes.begin(), tree.processes.end(),
        [&](const process::ProcessRecord& record) {
            return !record.exited &&
                   record.identity.processId == processId;
        });
}

bool processTreeHasExactIdentity(
    const process::ProcessTreeSnapshot& tree,
    std::uint32_t processId,
    std::uint64_t creationTime100ns) noexcept {
    if (processId == 0 || creationTime100ns == 0) return false;
    if (tree.root.processId == processId &&
        tree.root.creationTime100ns == creationTime100ns) {
        return true;
    }
    return std::any_of(
        tree.processes.begin(), tree.processes.end(),
        [&](const process::ProcessRecord& record) {
            return !record.exited &&
                   record.identity.processId == processId &&
                   record.identity.creationTime100ns == creationTime100ns;
        });
}

bool endpointAvailable(
    std::span<const windows::AudioRenderEndpoint> endpoints,
    std::wstring_view endpointId,
    bool& found) {
    found = false;
    const auto wanted = canonical(endpointId);
    for (const auto& endpoint : endpoints) {
        if (canonical(endpoint.endpointId) != wanted) continue;
        found = true;
        return endpoint.isAvailable();
    }
    return false;
}

bool sameObservedProcess(
    const windows::AudioSessionObservation& left,
    const windows::AudioSessionObservation& right) noexcept {
    if (!left.processIdentity || !right.processIdentity) return false;
    return left.processIdentity->pid == right.processIdentity->pid &&
           left.processIdentity->creationIdentity ==
               right.processIdentity->creationIdentity;
}

bool verifyCapturedEndpoints(
    const OwnedSessionEvidence& before,
    const OwnedSessionEvidence& after,
    std::string& error) {
    if (before.duplicateOwnedProcessSessions ||
        after.duplicateOwnedProcessSessions) {
        error =
            "audio rollback cannot verify duplicate sessions for one exact process";
        return false;
    }

    for (const auto& original : before.ownedSessions) {
        const auto found = std::find_if(
            after.ownedSessions.begin(), after.ownedSessions.end(),
            [&](const windows::AudioSessionObservation& current) {
                return sameObservedProcess(original, current);
            });
        if (found == after.ownedSessions.end()) {
            error =
                "an originally owned audio session disappeared during rollback verification";
            return false;
        }
        if (!sameEndpoint(found->endpointId, original.endpointId)) {
            error =
                "an owned audio session did not return to its captured endpoint";
            return false;
        }
    }
    return true;
}

void setError(std::string* output, std::string value) {
    if (output) *output = std::move(value);
}

} // namespace

windows::AudioSessionInventoryResult
NativeSessionObservationSource::enumerate() noexcept {
#if defined(_WIN32)
    return windows::AudioSessionObserver::enumerateSessions();
#else
    windows::AudioSessionInventoryResult result;
    result.isComplete = false;
    result.error = windows::AudioSessionObserverError{
        windows::AudioSessionObserverError::Code::UnexpectedWindowsState, 0};
    return result;
#endif
}

bool sessionMatchesOwnedProcess(
    const process::ProcessTreeSnapshot& processTree,
    const windows::AudioSessionObservation& session) noexcept {
    return session.processIdentity.has_value() &&
           processTreeHasExactIdentity(
               processTree,
               session.processIdentity->pid,
               session.processIdentity->creationIdentity);
}

OwnedSessionEvidence collectOwnedSessionEvidence(
    const RouteRequest& request,
    std::span<const windows::AudioSessionObservation> sessions) {
    OwnedSessionEvidence evidence;
    std::set<std::pair<std::uint32_t, std::uint64_t>> exactProcesses;

    for (const auto& session : sessions) {
        if (sessionMatchesOwnedProcess(request.processTree, session)) {
            const auto key = std::make_pair(
                session.processIdentity->pid,
                session.processIdentity->creationIdentity);
            if (!exactProcesses.insert(key).second) {
                evidence.duplicateOwnedProcessSessions = true;
            }
            evidence.ownedSessions.push_back(session);
            continue;
        }

        if (processTreeContainsPid(request.processTree, session.processId)) {
            evidence.pidMatchesWithoutVerifiedIdentity.push_back(session);
        }
    }

    evidence.allOwnedSessionsOnTarget =
        !evidence.ownedSessions.empty() &&
        std::all_of(
            evidence.ownedSessions.begin(), evidence.ownedSessions.end(),
            [&](const windows::AudioSessionObservation& session) {
                return sameEndpoint(
                    session.endpointId, request.targetEndpointId);
            });
    return evidence;
}

RouteCapability ObserveOnlyRouteBackend::capability(
    const RouteRequest&,
    const OwnedSessionEvidence& evidence) const noexcept {
    return evidence.allOwnedSessionsOnTarget
               ? RouteCapability::SatisfiedWithoutMutation
               : RouteCapability::Unsupported;
}

bool ObserveOnlyRouteBackend::captureState(
    const RouteRequest&,
    const OwnedSessionEvidence&,
    BackendState& state,
    std::string& error) noexcept {
    state.opaque.clear();
    error.clear();
    return true;
}

bool ObserveOnlyRouteBackend::apply(
    const RouteRequest&,
    const BackendState&,
    std::string& error) noexcept {
    error =
        "observe-only audio backend cannot change another process endpoint";
    return false;
}

bool ObserveOnlyRouteBackend::rollback(
    const RouteRequest&,
    const BackendState&,
    std::string& error) noexcept {
    error.clear();
    return true;
}

RouteTransaction::RouteTransaction(
    RouteRequest request,
    std::shared_ptr<SessionObservationSource> sessions,
    std::shared_ptr<RouteBackend> backend)
    : request_(std::move(request)),
      sessions_(std::move(sessions)),
      backend_(std::move(backend)) {
    if (!backend_) {
        backend_ = std::make_shared<ObserveOnlyRouteBackend>();
    }
    status_.backendKind = backend_->kind();
}

bool RouteTransaction::validateRequest(
    std::span<const windows::AudioRenderEndpoint> endpoints,
    std::string& error) {
    if ((request_.seatId != 1 && request_.seatId != 2) ||
        request_.processTree.seatId != request_.seatId) {
        status_.error = RouteError::InvalidSeat;
        error = "audio route Seat does not match the owned process tree";
        return false;
    }
    if (!request_.processTree.root.valid() ||
        !request_.processTree.trackingComplete) {
        status_.error = RouteError::InvalidProcessTree;
        error =
            "audio route requires an exact, completely tracked process tree";
        return false;
    }
    if (request_.targetEndpointId.empty()) {
        status_.error = RouteError::TargetEndpointMissing;
        error = "audio target endpoint identity is empty";
        return false;
    }

    bool found = false;
    if (!endpointAvailable(
            endpoints, request_.targetEndpointId, found)) {
        status_.error = found
                            ? RouteError::TargetEndpointUnavailable
                            : RouteError::TargetEndpointMissing;
        error = found
                    ? "audio target endpoint is not currently active"
                    : "audio target endpoint is not present";
        return false;
    }
    return true;
}

bool RouteTransaction::observeEvidence(
    OwnedSessionEvidence& evidence,
    std::string& error) {
    if (!sessions_) {
        status_.error = RouteError::SnapshotFailed;
        error = "audio session observation source is unavailable";
        return false;
    }

    auto observed = sessions_->enumerate();
    if (!observed.isSuccess()) {
        status_.error = RouteError::SnapshotFailed;
        error = "audio session observation failed";
        return false;
    }
    if (!observed.isComplete) {
        status_.error = RouteError::SnapshotFailed;
        error =
            "audio session observation is incomplete and cannot prove ownership";
        return false;
    }

    evidence = collectOwnedSessionEvidence(
        request_, observed.sessions);
    if (!evidence.pidMatchesWithoutVerifiedIdentity.empty()) {
        status_.error = RouteError::OwnershipUnverified;
        error =
            "PID-matching audio session lacks exact process creation identity";
        return false;
    }
    if (evidence.duplicateOwnedProcessSessions) {
        status_.error = RouteError::OwnershipUnverified;
        error =
            "multiple audio sessions for one exact process are not individually identifiable";
        return false;
    }
    return true;
}

bool RouteTransaction::verifyApplied(
    OwnedSessionEvidence& evidence,
    std::string& error) {
    if (!observeEvidence(evidence, error)) return false;
    if (evidence.ownedSessions.empty()) {
        error = "owned audio session disappeared during apply verification";
        return false;
    }
    if (!evidence.allOwnedSessionsOnTarget) {
        error = "owned audio session did not move to the requested endpoint";
        return false;
    }
    return true;
}

bool RouteTransaction::verifyRollback(
    OwnedSessionEvidence& evidence,
    std::string& error) {
    if (!beforeEvidence_) {
        error = "audio rollback has no captured ownership evidence";
        return false;
    }
    if (!observeEvidence(evidence, error)) return false;
    return verifyCapturedEndpoints(*beforeEvidence_, evidence, error);
}

RouteStatus RouteTransaction::attempt(
    std::span<const windows::AudioRenderEndpoint> endpoints,
    std::string* error) {
    if (error) error->clear();
    std::string localError;

    status_.backendKind = backend_->kind();
    status_.rollbackVerified = false;

    if (!validateRequest(endpoints, localError)) {
        status_.phase = RoutePhase::Failed;
        setError(error, std::move(localError));
        return status_;
    }

    OwnedSessionEvidence evidence;
    if (!observeEvidence(evidence, localError)) {
        status_.phase = RoutePhase::Failed;
        status_.evidence = std::move(evidence);
        setError(error, std::move(localError));
        return status_;
    }
    status_.evidence = evidence;

    if (evidence.ownedSessions.empty()) {
        status_.phase = RoutePhase::WaitingForSession;
        status_.error = RouteError::None;
        status_.capability = RouteCapability::Unsupported;
        return status_;
    }

    if (evidence.allOwnedSessionsOnTarget) {
        status_.phase = RoutePhase::Satisfied;
        status_.error = RouteError::None;
        status_.capability = RouteCapability::SatisfiedWithoutMutation;
        status_.mutated = false;
        return status_;
    }

    status_.capability = backend_->capability(request_, evidence);
    if (status_.capability == RouteCapability::Unsupported) {
        status_.phase = RoutePhase::Unsupported;
        status_.error = RouteError::BackendUnsupported;
        localError =
            "no approved audio backend can move the exact owned session";
        setError(error, std::move(localError));
        return status_;
    }
    if (status_.capability == RouteCapability::SatisfiedWithoutMutation) {
        status_.phase = RoutePhase::Satisfied;
        status_.error = RouteError::None;
        status_.mutated = false;
        return status_;
    }

    BackendState before;
    if (!backend_->captureState(request_, evidence, before, localError)) {
        status_.phase = RoutePhase::Failed;
        status_.error = RouteError::SnapshotFailed;
        setError(error, std::move(localError));
        return status_;
    }
    before_ = before;
    beforeEvidence_ = evidence;

    // Once apply is invoked, assume mutation may have occurred until exact
    // rollback is receiver-verified.
    status_.mutated = true;
    if (!backend_->apply(request_, before, localError)) {
        const std::string applyError = localError;
        std::string rollbackError;
        OwnedSessionEvidence rollbackEvidence;
        const bool rolledBack =
            backend_->rollback(request_, before, rollbackError) &&
            verifyRollback(rollbackEvidence, rollbackError);
        status_.evidence = std::move(rollbackEvidence);
        if (rolledBack) {
            status_.phase = RoutePhase::Failed;
            status_.error = RouteError::ApplyFailed;
            status_.mutated = false;
            status_.rollbackVerified = true;
            setError(error, applyError);
        } else {
            status_.phase = RoutePhase::RecoveryRequired;
            status_.error = RouteError::RollbackFailed;
            setError(
                error,
                rollbackError.empty() ? applyError : std::move(rollbackError));
        }
        return status_;
    }

    OwnedSessionEvidence appliedEvidence;
    if (!verifyApplied(appliedEvidence, localError)) {
        const std::string verifyError = localError;
        std::string rollbackError;
        OwnedSessionEvidence rollbackEvidence;
        const bool rolledBack =
            backend_->rollback(request_, before, rollbackError) &&
            verifyRollback(rollbackEvidence, rollbackError);
        status_.evidence = std::move(rollbackEvidence);
        if (rolledBack) {
            status_.phase = RoutePhase::Failed;
            status_.error = RouteError::VerificationFailed;
            status_.mutated = false;
            status_.rollbackVerified = true;
            setError(error, verifyError);
        } else {
            status_.phase = RoutePhase::RecoveryRequired;
            status_.error = RouteError::RollbackFailed;
            setError(
                error,
                rollbackError.empty() ? verifyError : std::move(rollbackError));
        }
        return status_;
    }

    status_.phase = RoutePhase::Applied;
    status_.error = RouteError::None;
    status_.evidence = std::move(appliedEvidence);
    status_.rollbackVerified = false;
    return status_;
}

RouteStatus RouteTransaction::rollback(
    std::span<const windows::AudioRenderEndpoint> endpoints,
    std::string* error) {
    if (error) error->clear();

    std::string localError;
    if (!validateRequest(endpoints, localError)) {
        status_.phase = RoutePhase::RecoveryRequired;
        setError(error, std::move(localError));
        return status_;
    }

    if (!status_.mutated) {
        status_.phase = RoutePhase::Ready;
        status_.error = RouteError::None;
        return status_;
    }
    if (!before_) {
        status_.phase = RoutePhase::RecoveryRequired;
        status_.error = RouteError::RollbackFailed;
        setError(error, "audio rollback state was not captured");
        return status_;
    }

    if (!backend_->rollback(request_, *before_, localError)) {
        status_.phase = RoutePhase::RecoveryRequired;
        status_.error = RouteError::RollbackFailed;
        setError(error, std::move(localError));
        return status_;
    }

    OwnedSessionEvidence evidence;
    if (!verifyRollback(evidence, localError)) {
        status_.phase = RoutePhase::RecoveryRequired;
        status_.error = RouteError::RollbackFailed;
        status_.evidence = std::move(evidence);
        setError(error, std::move(localError));
        return status_;
    }

    status_.phase = RoutePhase::Ready;
    status_.error = RouteError::None;
    status_.evidence = std::move(evidence);
    status_.mutated = false;
    status_.rollbackVerified = true;
    before_.reset();
    beforeEvidence_.reset();
    return status_;
}

std::string_view routePhaseName(RoutePhase value) noexcept {
    switch (value) {
    case RoutePhase::Unprepared: return "unprepared";
    case RoutePhase::WaitingForSession: return "waiting-for-session";
    case RoutePhase::Ready: return "ready";
    case RoutePhase::Satisfied: return "satisfied";
    case RoutePhase::Applied: return "applied";
    case RoutePhase::Unsupported: return "unsupported";
    case RoutePhase::Failed: return "failed";
    case RoutePhase::RecoveryRequired: return "recovery-required";
    }
    return "unknown";
}

std::string_view routeErrorName(RouteError value) noexcept {
    switch (value) {
    case RouteError::None: return "none";
    case RouteError::InvalidSeat: return "invalid-seat";
    case RouteError::InvalidProcessTree: return "invalid-process-tree";
    case RouteError::TargetEndpointMissing: return "target-endpoint-missing";
    case RouteError::TargetEndpointUnavailable:
        return "target-endpoint-unavailable";
    case RouteError::OwnershipUnverified: return "ownership-unverified";
    case RouteError::BackendUnsupported: return "backend-unsupported";
    case RouteError::SnapshotFailed: return "snapshot-failed";
    case RouteError::ApplyFailed: return "apply-failed";
    case RouteError::VerificationFailed: return "verification-failed";
    case RouteError::RollbackFailed: return "rollback-failed";
    }
    return "unknown";
}

std::string_view routeBackendKindName(RouteBackendKind value) noexcept {
    switch (value) {
    case RouteBackendKind::ObserveOnly: return "observe-only";
    case RouteBackendKind::ProviderManaged: return "provider-managed";
    }
    return "unknown";
}

} // namespace hydra::audio
