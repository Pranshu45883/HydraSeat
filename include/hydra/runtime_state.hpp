#pragma once

#include "hydra/workspace_manager.hpp"
#include "hydra/seat_game_lifecycle.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::runtime {

enum class HostLifecyclePhase : std::uint8_t {
    Starting = 0,
    Running = 1,
    ExitRequested = 2,
    Stopped = 3
};

enum class SeatSessionPhase : std::uint8_t {
    Idle = 0,
    Planning = 1,
    Prepared = 2,
    Starting = 3,
    Active = 4,
    Degraded = 5,
    Stopping = 6,
    RollingBack = 7,
    RecoveryRequired = 8
};

enum class RuntimeCommand : std::uint8_t {
    LoadProfile = 0,
    Plan = 1,
    Prepare = 2,
    Start = 3,
    StopAndReturnToWindows = 4,
    Reset = 5,
    ExitHostWhenIdle = 6,
    MarkDegraded = 7,
    BeginReconfigure = 8,
    AssignSeatGame = 9,
    StartSeatGame = 10,
    StopSeatGame = 11,
    ReconcileSeatGames = 12,
    ObserveSeatGameExit = 13
};

enum class RuntimeResultCode : std::uint8_t {
    Ok = 0,
    AlreadySatisfied = 1,
    Busy = 2,
    InvalidState = 3,
    InvalidProfile = 4,
    BackendFailure = 5,
    RollbackFailure = 6,
    RecoveryRequired = 7
};

struct RuntimeSessionId {
    std::array<std::uint8_t, 16> bytes{};

    bool empty() const noexcept;
    bool operator==(const RuntimeSessionId&) const = default;
};

struct SeatRuntimeState {
    SeatId seatId{0};
    SeatSessionPhase phase{SeatSessionPhase::Idle};
    std::string diagnostic;

    bool operator==(const SeatRuntimeState&) const = default;
};

struct RuntimeTransition {
    std::uint64_t sequence{0};
    std::uint64_t correlationId{0};
    RuntimeCommand command{RuntimeCommand::Plan};
    SeatSessionPhase from{SeatSessionPhase::Idle};
    SeatSessionPhase to{SeatSessionPhase::Idle};
    RuntimeResultCode result{RuntimeResultCode::Ok};
    SeatId seatId{0}; // Zero for whole-machine transitions.
    std::string diagnostic;

    bool operator==(const RuntimeTransition&) const = default;
};

struct HostRuntimeSnapshot {
    std::uint32_t schemaVersion{3};
    HostLifecyclePhase hostPhase{HostLifecyclePhase::Starting};
    SeatSessionPhase sessionPhase{SeatSessionPhase::Idle};
    RuntimeSessionId sessionId{};
    std::uint64_t generation{0};
    std::uint64_t transitionSequence{0};
    std::uint32_t connectedControlClients{0};
    SeatId managementSeatId{1};
    bool profileLoaded{false};
    bool mutationInProgress{false};
    std::vector<SeatRuntimeState> seats;
    std::vector<SeatGameState> seatGames;
    bool wholeMachineReturnRequested{false};
    std::vector<SeatConfig> configuredSeats;
    std::optional<RuntimeTransition> lastTransition;
    std::string diagnostic;
};

struct RuntimeCommandResult {
    RuntimeResultCode code{RuntimeResultCode::Ok};
    HostRuntimeSnapshot snapshot;
    std::string diagnostic;

    bool succeeded() const noexcept {
        return code == RuntimeResultCode::Ok ||
               code == RuntimeResultCode::AlreadySatisfied;
    }
};

std::string_view hostLifecyclePhaseName(HostLifecyclePhase phase) noexcept;
std::string_view seatSessionPhaseName(SeatSessionPhase phase) noexcept;
std::string_view runtimeCommandName(RuntimeCommand command) noexcept;
std::string_view runtimeResultCodeName(RuntimeResultCode code) noexcept;
std::string runtimeSessionIdHex(const RuntimeSessionId& id);

} // namespace hydra::runtime
