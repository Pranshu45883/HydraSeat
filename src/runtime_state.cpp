#include "hydra/runtime_state.hpp"

#include <array>

namespace hydra::runtime {

bool RuntimeSessionId::empty() const noexcept {
    for (const auto byte : bytes) {
        if (byte != 0) return false;
    }
    return true;
}

std::string_view hostLifecyclePhaseName(HostLifecyclePhase phase) noexcept {
    switch (phase) {
        case HostLifecyclePhase::Starting: return "starting";
        case HostLifecyclePhase::Running: return "running";
        case HostLifecyclePhase::ExitRequested: return "exit-requested";
        case HostLifecyclePhase::Stopped: return "stopped";
    }
    return "unknown";
}

std::string_view seatSessionPhaseName(SeatSessionPhase phase) noexcept {
    switch (phase) {
        case SeatSessionPhase::Idle: return "idle";
        case SeatSessionPhase::Planning: return "planning";
        case SeatSessionPhase::Prepared: return "prepared";
        case SeatSessionPhase::Starting: return "starting";
        case SeatSessionPhase::Active: return "active";
        case SeatSessionPhase::Degraded: return "degraded";
        case SeatSessionPhase::Stopping: return "stopping";
        case SeatSessionPhase::RollingBack: return "rolling-back";
        case SeatSessionPhase::RecoveryRequired: return "recovery-required";
    }
    return "unknown";
}

std::string_view runtimeCommandName(RuntimeCommand command) noexcept {
    switch (command) {
        case RuntimeCommand::LoadProfile: return "load-profile";
        case RuntimeCommand::Plan: return "plan";
        case RuntimeCommand::Prepare: return "prepare";
        case RuntimeCommand::Start: return "start";
        case RuntimeCommand::StopAndReturnToWindows: return "stop-and-return-to-windows";
        case RuntimeCommand::Reset: return "reset";
        case RuntimeCommand::ExitHostWhenIdle: return "exit-host-when-idle";
        case RuntimeCommand::MarkDegraded: return "mark-degraded";
        case RuntimeCommand::BeginReconfigure: return "begin-reconfigure";
        case RuntimeCommand::AssignSeatGame: return "assign-seat-game";
        case RuntimeCommand::StartSeatGame: return "start-seat-game";
        case RuntimeCommand::StopSeatGame: return "stop-seat-game";
        case RuntimeCommand::ReconcileSeatGames: return "reconcile-seat-games";
        case RuntimeCommand::ObserveSeatGameExit: return "observe-seat-game-exit";
    }
    return "unknown";
}

std::string_view runtimeResultCodeName(RuntimeResultCode code) noexcept {
    switch (code) {
        case RuntimeResultCode::Ok: return "ok";
        case RuntimeResultCode::AlreadySatisfied: return "already-satisfied";
        case RuntimeResultCode::Busy: return "busy";
        case RuntimeResultCode::InvalidState: return "invalid-state";
        case RuntimeResultCode::InvalidProfile: return "invalid-profile";
        case RuntimeResultCode::BackendFailure: return "backend-failure";
        case RuntimeResultCode::RollbackFailure: return "rollback-failure";
        case RuntimeResultCode::RecoveryRequired: return "recovery-required";
    }
    return "unknown";
}

std::string runtimeSessionIdHex(const RuntimeSessionId& id) {
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result;
    result.reserve(id.bytes.size() * 2u);
    for (const auto byte : id.bytes) {
        result.push_back(digits[(byte >> 4u) & 0x0fu]);
        result.push_back(digits[byte & 0x0fu]);
    }
    return result;
}

} // namespace hydra::runtime
