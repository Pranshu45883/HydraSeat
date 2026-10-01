#pragma once

#include "hydra/audio_endpoint_inventory.hpp"
#include "hydra/audio_session_observer.hpp"
#include "hydra/process_group.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::audio {

enum class RouteBackendKind : std::uint8_t {
    ObserveOnly = 0,
    ProviderManaged = 1,
};

enum class RouteCapability : std::uint8_t {
    Unsupported = 0,
    SatisfiedWithoutMutation = 1,
    Mutable = 2,
};

struct RouteRequest {
    SeatId seatId{0};
    process::ProcessTreeSnapshot processTree;
    std::wstring targetEndpointId;
};

struct OwnedSessionEvidence {
    std::vector<windows::AudioSessionObservation> ownedSessions;
    std::vector<windows::AudioSessionObservation> pidMatchesWithoutVerifiedIdentity;
    bool duplicateOwnedProcessSessions{false};
    bool allOwnedSessionsOnTarget{false};
};

enum class RoutePhase : std::uint8_t {
    Unprepared = 0,
    WaitingForSession = 1,
    Ready = 2,
    Satisfied = 3,
    Applied = 4,
    Unsupported = 5,
    Failed = 6,
    RecoveryRequired = 7,
};

enum class RouteError : std::uint8_t {
    None = 0,
    InvalidSeat = 1,
    InvalidProcessTree = 2,
    TargetEndpointMissing = 3,
    TargetEndpointUnavailable = 4,
    OwnershipUnverified = 5,
    BackendUnsupported = 6,
    SnapshotFailed = 7,
    ApplyFailed = 8,
    VerificationFailed = 9,
    RollbackFailed = 10,
};

struct BackendState {
    std::vector<std::uint8_t> opaque;

    bool operator==(const BackendState&) const = default;
};

struct RouteStatus {
    RoutePhase phase{RoutePhase::Unprepared};
    RouteError error{RouteError::None};
    RouteBackendKind backendKind{RouteBackendKind::ObserveOnly};
    RouteCapability capability{RouteCapability::Unsupported};
    OwnedSessionEvidence evidence;
    bool mutated{false};
    bool rollbackVerified{false};
};

class SessionObservationSource {
public:
    virtual ~SessionObservationSource() = default;
    virtual windows::AudioSessionInventoryResult enumerate() noexcept = 0;
};

class NativeSessionObservationSource final : public SessionObservationSource {
public:
    windows::AudioSessionInventoryResult enumerate() noexcept override;
};

// Mutable implementations must capture enough exact state to restore only the
// sessions owned by RouteRequest::processTree. Undocumented/global policy
// mutation is deliberately not provided by the default backend.
class RouteBackend {
public:
    virtual ~RouteBackend() = default;

    virtual RouteBackendKind kind() const noexcept = 0;
    virtual RouteCapability capability(
        const RouteRequest& request,
        const OwnedSessionEvidence& evidence) const noexcept = 0;
    virtual bool captureState(
        const RouteRequest& request,
        const OwnedSessionEvidence& evidence,
        BackendState& state,
        std::string& error) noexcept = 0;
    virtual bool apply(
        const RouteRequest& request,
        const BackendState& before,
        std::string& error) noexcept = 0;
    virtual bool rollback(
        const RouteRequest& request,
        const BackendState& before,
        std::string& error) noexcept = 0;
};

class ObserveOnlyRouteBackend final : public RouteBackend {
public:
    RouteBackendKind kind() const noexcept override {
        return RouteBackendKind::ObserveOnly;
    }
    RouteCapability capability(
        const RouteRequest& request,
        const OwnedSessionEvidence& evidence) const noexcept override;
    bool captureState(
        const RouteRequest& request,
        const OwnedSessionEvidence& evidence,
        BackendState& state,
        std::string& error) noexcept override;
    bool apply(
        const RouteRequest& request,
        const BackendState& before,
        std::string& error) noexcept override;
    bool rollback(
        const RouteRequest& request,
        const BackendState& before,
        std::string& error) noexcept override;
};

class RouteTransaction final {
public:
    RouteTransaction(
        RouteRequest request,
        std::shared_ptr<SessionObservationSource> sessions,
        std::shared_ptr<RouteBackend> backend = {});

    RouteStatus attempt(
        std::span<const windows::AudioRenderEndpoint> endpoints,
        std::string* error = nullptr);
    RouteStatus rollback(
        std::span<const windows::AudioRenderEndpoint> endpoints,
        std::string* error = nullptr);

    const RouteStatus& status() const noexcept { return status_; }

private:
    bool validateRequest(
        std::span<const windows::AudioRenderEndpoint> endpoints,
        std::string& error);
    bool observeEvidence(OwnedSessionEvidence& evidence, std::string& error);
    bool verifyApplied(OwnedSessionEvidence& evidence, std::string& error);
    bool verifyRollback(OwnedSessionEvidence& evidence, std::string& error);

    RouteRequest request_;
    std::shared_ptr<SessionObservationSource> sessions_;
    std::shared_ptr<RouteBackend> backend_;
    std::optional<BackendState> before_;
    std::optional<OwnedSessionEvidence> beforeEvidence_;
    RouteStatus status_;
};

OwnedSessionEvidence collectOwnedSessionEvidence(
    const RouteRequest& request,
    std::span<const windows::AudioSessionObservation> sessions);

bool sessionMatchesOwnedProcess(
    const process::ProcessTreeSnapshot& processTree,
    const windows::AudioSessionObservation& session) noexcept;

std::string_view routePhaseName(RoutePhase value) noexcept;
std::string_view routeErrorName(RouteError value) noexcept;
std::string_view routeBackendKindName(RouteBackendKind value) noexcept;

} // namespace hydra::audio
