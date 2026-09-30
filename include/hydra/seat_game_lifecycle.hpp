#pragma once

#include "hydra/workspace_manager.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::runtime {

constexpr std::size_t kV1MaximumActiveSeats = 2u;
constexpr std::size_t kSeatGameIdentifierMaxBytes = 256u;

enum class SeatGamePhase : std::uint8_t {
    Idle = 0,
    Planning = 1,
    Starting = 2,
    Playing = 3,
    Stopping = 4,
    Degraded = 5,
    RecoveryRequired = 6,
};

enum class SeatGameResultCode : std::uint8_t {
    Ok = 0,
    AlreadySatisfied = 1,
    Busy = 2,
    InvalidSeat = 3,
    InvalidState = 4,
    InvalidBinding = 5,
    DuplicateCorrelation = 6,
    BackendFailure = 7,
    RecoveryRequired = 8,
    V1SeatLimitExceeded = 9,
};

struct SeatGameBinding {
    std::string playerId;
    std::string gameId;

    bool operator==(const SeatGameBinding&) const = default;
};

struct SeatGameState {
    SeatId seatId{0};
    SeatGamePhase phase{SeatGamePhase::Idle};
    std::optional<SeatGameBinding> binding;
    std::uint64_t generation{0};
    std::string diagnostic;

    bool operator==(const SeatGameState&) const = default;
};

struct SeatGameCommandResult {
    SeatGameResultCode code{SeatGameResultCode::Ok};
    std::vector<SeatGameState> seats;
    bool wholeMachineReturnRequested{false};
    std::string diagnostic;

    bool succeeded() const noexcept {
        return code == SeatGameResultCode::Ok ||
               code == SeatGameResultCode::AlreadySatisfied;
    }
};

// One instance owns only the temporary process/window/input/audio/controller
// state for a single Seat. stop() must be idempotent and must never clean a
// different Seat. The coordinator invokes these methods only on its serialized
// control path, never from an input callback.
class ISeatGameInstance {
public:
    virtual ~ISeatGameInstance() = default;
    virtual bool start(const SeatGameBinding& binding, std::string& error) = 0;
    virtual bool stop(std::string& error) noexcept = 0;
    virtual bool verifyStopped(std::string& error) noexcept = 0;
    virtual bool running() const noexcept = 0;
};

class ISeatGameInstanceFactory {
public:
    virtual ~ISeatGameInstanceFactory() = default;
    virtual std::unique_ptr<ISeatGameInstance> create(SeatId seatId,
                                                       std::string& error) = 0;
    // Binding-aware creation lets a host-owned immutable plan registry resolve
    // the exact Seat/Player/Game activation epoch without weakening existing
    // factories. Implementations that do not need this context retain the old
    // create(SeatId) behavior by default.
    virtual std::unique_ptr<ISeatGameInstance> createForBinding(
        SeatId seatId, const SeatGameBinding& binding,
        std::uint64_t expectedGeneration, std::string& error) {
        (void)binding;
        (void)expectedGeneration;
        return create(seatId, error);
    }
};

// Authoritative in-memory per-Seat lifecycle. Producers are host control/target
// observation threads; a try-lock rejects concurrent writers. State snapshots
// are coherent, bounded to two v1 Seats, and preserve Seat ordering. Player and
// Game identities are temporary bindings and are never written to SeatConfig.
class SeatGameLifecycle {
public:
    SeatGameLifecycle(std::span<const SeatId> activeSeats,
                      std::shared_ptr<ISeatGameInstanceFactory> factory);
    ~SeatGameLifecycle();

    SeatGameLifecycle(const SeatGameLifecycle&) = delete;
    SeatGameLifecycle& operator=(const SeatGameLifecycle&) = delete;

    SeatGameCommandResult assign(SeatId seatId, SeatGameBinding binding,
                                 std::uint64_t correlationId);
    SeatGameCommandResult start(SeatId seatId, std::uint64_t correlationId);
    SeatGameCommandResult stop(SeatId seatId, std::uint64_t correlationId);
    SeatGameCommandResult observeTargetExit(SeatId seatId, bool cleanExit,
                                            std::string diagnostic,
                                            std::uint64_t correlationId);
    SeatGameCommandResult observeTargetExit(SeatId seatId,
                                             std::uint64_t expectedGeneration,
                                             bool cleanExit,
                                             std::string diagnostic,
                                             std::uint64_t correlationId);
    SeatGameCommandResult reconcile(std::uint64_t correlationId);
    SeatGameCommandResult emergencyStopAll(std::uint64_t correlationId);
    // Internal host teardown path. It intentionally bypasses external command
    // correlation accounting while preserving the same fail-closed cleanup
    // result as emergencyStopAll().
    SeatGameCommandResult shutdown();

    std::vector<SeatGameState> snapshot() const;
    bool wholeMachineReturnRequested() const;

private:
    struct Entry {
        SeatGameState state;
        std::unique_ptr<ISeatGameInstance> instance;
    };

    SeatGameCommandResult rejectLocked(SeatGameResultCode code,
                                       std::string diagnostic) const;
    SeatGameCommandResult finishLocked(SeatGameResultCode code,
                                       std::string diagnostic);
    Entry* findLocked(SeatId seatId);
    bool rememberCorrelationLocked(std::uint64_t correlationId);
    void updateReturnPolicyLocked();
    SeatGameCommandResult emergencyStopAllLocked();
    std::vector<SeatGameState> snapshotLocked() const;
    static bool validBinding(const SeatGameBinding& binding);

    mutable std::mutex mutex_;
    std::map<SeatId, Entry> entries_;
    std::shared_ptr<ISeatGameInstanceFactory> factory_;
    std::deque<std::uint64_t> correlations_;
    bool configurationValid_{true};
    SeatGameResultCode configurationErrorCode_{SeatGameResultCode::Ok};
    std::string configurationError_;
    bool wholeMachineReturnRequested_{false};
};

std::string_view seatGamePhaseName(SeatGamePhase phase) noexcept;
std::string_view seatGameResultCodeName(SeatGameResultCode code) noexcept;

} // namespace hydra::runtime
