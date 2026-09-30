#include "hydra/seat_game_lifecycle.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace hydra::runtime {
namespace {

constexpr std::size_t kMaximumDiagnosticBytes = 2048u;
constexpr std::size_t kRememberedCorrelations = 128u;

std::string boundedDiagnostic(std::string value) {
    if (value.size() > kMaximumDiagnosticBytes) value.resize(kMaximumDiagnosticBytes);
    return value;
}

bool boundedIdentifier(std::string_view value) {
    return !value.empty() && value.size() <= kSeatGameIdentifierMaxBytes &&
           value.find('\0') == std::string_view::npos;
}

} // namespace

SeatGameLifecycle::SeatGameLifecycle(
    std::span<const SeatId> activeSeats,
    std::shared_ptr<ISeatGameInstanceFactory> factory)
    : factory_(std::move(factory)) {
    if (activeSeats.empty() || activeSeats.size() > kV1MaximumActiveSeats) {
        configurationValid_ = false;
        configurationErrorCode_ = activeSeats.empty()
            ? SeatGameResultCode::InvalidSeat
            : SeatGameResultCode::V1SeatLimitExceeded;
        configurationError_ = activeSeats.empty()
            ? "at least one active Seat is required"
            : "HydraSeat v1 rejects more than two active Seats";
        return;
    }
    for (const auto seatId : activeSeats) {
        if (seatId == 0 || entries_.contains(seatId)) {
            configurationValid_ = false;
            configurationErrorCode_ = SeatGameResultCode::InvalidSeat;
            configurationError_ = "active Seat identifiers must be nonzero and unique";
            entries_.clear();
            return;
        }
        Entry entry;
        entry.state.seatId = seatId;
        entries_.emplace(seatId, std::move(entry));
    }
    if (!factory_) {
        configurationValid_ = false;
        configurationErrorCode_ = SeatGameResultCode::BackendFailure;
        configurationError_ = "Seat game instance factory is required";
    }
}

SeatGameLifecycle::~SeatGameLifecycle() {
    (void)shutdown();
}

SeatGameCommandResult SeatGameLifecycle::assign(
    SeatId seatId, SeatGameBinding binding, std::uint64_t correlationId) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return {SeatGameResultCode::Busy, {}, false,
                                   "another Seat lifecycle mutation is in progress"};
    if (!configurationValid_) {
        return rejectLocked(configurationErrorCode_, configurationError_);
    }
    if (!rememberCorrelationLocked(correlationId)) {
        return rejectLocked(SeatGameResultCode::DuplicateCorrelation,
                            "duplicate or zero correlation identifier rejected");
    }
    auto* entry = findLocked(seatId);
    if (!entry) return rejectLocked(SeatGameResultCode::InvalidSeat,
                                    "Seat is not configured for this v1 runtime");
    if (entry->state.phase != SeatGamePhase::Idle) {
        return rejectLocked(SeatGameResultCode::InvalidState,
                            "Player/Game binding may change only while the Seat is Idle");
    }
    if (!validBinding(binding)) {
        return rejectLocked(SeatGameResultCode::InvalidBinding,
                            "Player and Game identifiers must be nonempty and bounded");
    }
    entry->state.binding = std::move(binding);
    entry->state.phase = SeatGamePhase::Planning;
    entry->state.diagnostic = "temporary Player/Game binding planned";
    wholeMachineReturnRequested_ = false;
    return finishLocked(SeatGameResultCode::Ok, entry->state.diagnostic);
}

SeatGameCommandResult SeatGameLifecycle::start(SeatId seatId,
                                                std::uint64_t correlationId) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return {SeatGameResultCode::Busy, {}, false,
                                   "another Seat lifecycle mutation is in progress"};
    if (!configurationValid_) {
        return rejectLocked(configurationErrorCode_, configurationError_);
    }
    if (!rememberCorrelationLocked(correlationId)) {
        return rejectLocked(SeatGameResultCode::DuplicateCorrelation,
                            "duplicate or zero correlation identifier rejected");
    }
    auto* entry = findLocked(seatId);
    if (!entry) return rejectLocked(SeatGameResultCode::InvalidSeat,
                                    "Seat is not configured for this v1 runtime");
    if (entry->state.phase == SeatGamePhase::Playing && entry->instance &&
        entry->instance->running()) {
        return finishLocked(SeatGameResultCode::AlreadySatisfied,
                            "Seat game is already playing");
    }
    if (entry->state.phase != SeatGamePhase::Planning || !entry->state.binding) {
        return rejectLocked(SeatGameResultCode::InvalidState,
                            "Seat start requires a planned temporary binding");
    }
    std::string error;
    std::unique_ptr<ISeatGameInstance> instance;
    try {
        instance = factory_->createForBinding(
            seatId, *entry->state.binding, entry->state.generation + 1u, error);
    } catch (const std::exception& exception) {
        error = std::string("factory exception: ") + exception.what();
    } catch (...) {
        error = "factory raised an unknown exception";
    }
    if (!instance) {
        entry->state.phase = SeatGamePhase::Idle;
        entry->state.binding.reset();
        entry->state.diagnostic = boundedDiagnostic("Seat instance creation failed: " + error);
        updateReturnPolicyLocked();
        return finishLocked(SeatGameResultCode::BackendFailure, entry->state.diagnostic);
    }
    entry->state.phase = SeatGamePhase::Starting;
    bool started = false;
    try {
        started = instance->start(*entry->state.binding, error);
    } catch (const std::exception& exception) {
        error = std::string("start exception: ") + exception.what();
    } catch (...) {
        error = "start raised an unknown exception";
    }
    if (!started || !instance->running()) {
        std::string cleanupError;
        const bool cleaned = instance->stop(cleanupError) &&
                             instance->verifyStopped(cleanupError);
        // A failed cleanup is still owned state. Preserve the exact Seat-local
        // instance so Stop/EmergencyReset can retry verification instead of
        // dropping the only handle capable of cleaning a partially started
        // process tree or backend mutation.
        if (cleaned) {
            entry->instance.reset();
        } else {
            entry->instance = std::move(instance);
        }
        entry->state.binding.reset();
        entry->state.phase = cleaned ? SeatGamePhase::Idle
                                     : SeatGamePhase::RecoveryRequired;
        entry->state.diagnostic = boundedDiagnostic(
            std::string("Seat game start failed: ") + error +
            (cleanupError.empty() ? "" : "; cleanup: " + cleanupError) +
            (cleaned ? "" : "; exact Seat instance retained for cleanup retry"));
        updateReturnPolicyLocked();
        return finishLocked(cleaned ? SeatGameResultCode::BackendFailure
                                    : SeatGameResultCode::RecoveryRequired,
                            entry->state.diagnostic);
    }
    entry->instance = std::move(instance);
    ++entry->state.generation;
    entry->state.phase = SeatGamePhase::Playing;
    entry->state.diagnostic = "Seat game process ownership verified";
    wholeMachineReturnRequested_ = false;
    return finishLocked(SeatGameResultCode::Ok, entry->state.diagnostic);
}

SeatGameCommandResult SeatGameLifecycle::stop(SeatId seatId,
                                               std::uint64_t correlationId) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return {SeatGameResultCode::Busy, {}, false,
                                   "another Seat lifecycle mutation is in progress"};
    if (!configurationValid_) {
        return rejectLocked(configurationErrorCode_, configurationError_);
    }
    if (!rememberCorrelationLocked(correlationId)) {
        return rejectLocked(SeatGameResultCode::DuplicateCorrelation,
                            "duplicate or zero correlation identifier rejected");
    }
    auto* entry = findLocked(seatId);
    if (!entry) return rejectLocked(SeatGameResultCode::InvalidSeat,
                                    "Seat is not configured for this v1 runtime");
    if (entry->state.phase == SeatGamePhase::Idle) {
        updateReturnPolicyLocked();
        return finishLocked(SeatGameResultCode::AlreadySatisfied,
                            "Seat game is already stopped");
    }
    if (entry->state.phase == SeatGamePhase::Planning) {
        entry->state.binding.reset();
        entry->state.phase = SeatGamePhase::Idle;
        entry->state.diagnostic = "planned Seat game cancelled";
        updateReturnPolicyLocked();
        return finishLocked(SeatGameResultCode::Ok, entry->state.diagnostic);
    }
    if (entry->state.phase == SeatGamePhase::Degraded && !entry->instance) {
        entry->state.binding.reset();
        entry->state.phase = SeatGamePhase::Idle;
        entry->state.diagnostic = "verified exited Seat fault acknowledged";
        updateReturnPolicyLocked();
        return finishLocked(SeatGameResultCode::Ok, entry->state.diagnostic);
    }
    if (!entry->instance) {
        entry->state.phase = SeatGamePhase::RecoveryRequired;
        entry->state.diagnostic = "active Seat lost exact game instance ownership";
        return finishLocked(SeatGameResultCode::RecoveryRequired,
                            entry->state.diagnostic);
    }
    entry->state.phase = SeatGamePhase::Stopping;
    std::string error;
    const bool stopped = entry->instance->stop(error) &&
                         entry->instance->verifyStopped(error);
    if (!stopped) {
        entry->state.phase = SeatGamePhase::RecoveryRequired;
        entry->state.diagnostic = boundedDiagnostic("Seat-local cleanup failed: " + error);
        return finishLocked(SeatGameResultCode::RecoveryRequired,
                            entry->state.diagnostic);
    }
    entry->instance.reset();
    entry->state.binding.reset();
    entry->state.phase = SeatGamePhase::Idle;
    entry->state.diagnostic = "Seat-local game ownership cleaned";
    updateReturnPolicyLocked();
    return finishLocked(SeatGameResultCode::Ok, entry->state.diagnostic);
}

SeatGameCommandResult SeatGameLifecycle::observeTargetExit(
    SeatId seatId, bool cleanExit, std::string diagnostic,
    std::uint64_t correlationId) {
    return observeTargetExit(seatId, 0u, cleanExit, std::move(diagnostic),
                             correlationId);
}

SeatGameCommandResult SeatGameLifecycle::observeTargetExit(
    SeatId seatId, std::uint64_t expectedGeneration, bool cleanExit,
    std::string diagnostic, std::uint64_t correlationId) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return {SeatGameResultCode::Busy, {}, false,
                                   "another Seat lifecycle mutation is in progress"};
    if (!configurationValid_) {
        return rejectLocked(configurationErrorCode_, configurationError_);
    }
    if (!rememberCorrelationLocked(correlationId)) {
        return rejectLocked(SeatGameResultCode::DuplicateCorrelation,
                            "duplicate or zero correlation identifier rejected");
    }
    auto* entry = findLocked(seatId);
    if (!entry) return rejectLocked(SeatGameResultCode::InvalidSeat,
                                    "Seat is not configured for this v1 runtime");
    if (expectedGeneration != 0u &&
        entry->state.generation != expectedGeneration) {
        return rejectLocked(SeatGameResultCode::InvalidState,
                            "stale target-exit generation rejected");
    }
    if (entry->state.phase != SeatGamePhase::Playing || !entry->instance) {
        return rejectLocked(SeatGameResultCode::InvalidState,
                            "target exit observation requires a Playing Seat");
    }
    if (entry->instance->running()) {
        return rejectLocked(SeatGameResultCode::InvalidState,
                            "target exit was reported while exact process ownership is still live");
    }
    // Process exit does not prove that Seat-local display/input/controller/audio
    // ownership has already been restored. Run the same idempotent Seat-local
    // stop/rollback path before accepting the exit as clean.
    std::string verifyError;
    if (!entry->instance->stop(verifyError) ||
        !entry->instance->verifyStopped(verifyError)) {
        entry->state.phase = SeatGamePhase::RecoveryRequired;
        entry->state.diagnostic = boundedDiagnostic("target exited but Seat-local cleanup is unverified: " + verifyError);
        return finishLocked(SeatGameResultCode::RecoveryRequired,
                            entry->state.diagnostic);
    }
    entry->instance.reset();
    entry->state.binding.reset();
    entry->state.phase = cleanExit ? SeatGamePhase::Idle : SeatGamePhase::Degraded;
    entry->state.diagnostic = boundedDiagnostic(
        diagnostic.empty() ? (cleanExit ? "target exited normally" : "target exited unexpectedly")
                           : std::move(diagnostic));
    updateReturnPolicyLocked();
    return finishLocked(cleanExit ? SeatGameResultCode::Ok
                                  : SeatGameResultCode::BackendFailure,
                        entry->state.diagnostic);
}

SeatGameCommandResult SeatGameLifecycle::reconcile(std::uint64_t correlationId) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return {SeatGameResultCode::Busy, {}, false,
                                   "another Seat lifecycle mutation is in progress"};
    if (!configurationValid_) {
        return rejectLocked(configurationErrorCode_, configurationError_);
    }
    if (!rememberCorrelationLocked(correlationId)) {
        return rejectLocked(SeatGameResultCode::DuplicateCorrelation,
                            "duplicate or zero correlation identifier rejected");
    }
    bool changed = false;
    for (auto& [seatId, entry] : entries_) {
        (void)seatId;
        if (entry.state.phase != SeatGamePhase::Playing || !entry.instance ||
            entry.instance->running()) continue;
        std::string error;
        if (!entry.instance->stop(error) ||
            !entry.instance->verifyStopped(error)) {
            entry.state.phase = SeatGamePhase::RecoveryRequired;
            entry.state.diagnostic = boundedDiagnostic("automatic Seat-local exit cleanup unverified: " + error);
        } else {
            entry.instance.reset();
            entry.state.binding.reset();
            entry.state.phase = SeatGamePhase::Idle;
            entry.state.diagnostic = "normal target exit reconciled with Seat-local rollback";
        }
        changed = true;
    }
    updateReturnPolicyLocked();
    return finishLocked(changed ? SeatGameResultCode::Ok
                                : SeatGameResultCode::AlreadySatisfied,
                        changed ? "Seat target exits reconciled" : "Seat states already current");
}

SeatGameCommandResult SeatGameLifecycle::emergencyStopAll(
    std::uint64_t correlationId) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return {SeatGameResultCode::Busy, {}, false,
                                   "another Seat lifecycle mutation is in progress"};
    if (!configurationValid_) {
        return rejectLocked(configurationErrorCode_, configurationError_);
    }
    if (!rememberCorrelationLocked(correlationId)) {
        return rejectLocked(SeatGameResultCode::DuplicateCorrelation,
                            "duplicate or zero correlation identifier rejected");
    }
    return emergencyStopAllLocked();
}

SeatGameCommandResult SeatGameLifecycle::shutdown() {
    std::lock_guard lock(mutex_);
    return emergencyStopAllLocked();
}

SeatGameCommandResult SeatGameLifecycle::emergencyStopAllLocked() {
    bool safe = true;
    std::string diagnostic;
    for (auto& [seatId, entry] : entries_) {
        (void)seatId;
        bool localSafe = true;
        if (entry.instance) {
            std::string error;
            if (!entry.instance->stop(error) || !entry.instance->verifyStopped(error)) {
                localSafe = false;
                if (!diagnostic.empty()) diagnostic += "; ";
                diagnostic += error;
            } else {
                entry.instance.reset();
            }
        }
        if (localSafe) {
            entry.state.binding.reset();
            entry.state.phase = SeatGamePhase::Idle;
            entry.state.diagnostic = "Seat-local emergency cleanup verified";
        } else {
            safe = false;
            entry.state.phase = SeatGamePhase::RecoveryRequired;
            entry.state.diagnostic = "Seat-local emergency cleanup is unverified";
        }
    }
    wholeMachineReturnRequested_ = safe;
    return finishLocked(safe ? SeatGameResultCode::Ok
                             : SeatGameResultCode::RecoveryRequired,
                        safe ? "all Seat-local ownership cleaned for explicit return"
                             : boundedDiagnostic("emergency Seat cleanup failed: " + diagnostic));
}

std::vector<SeatGameState> SeatGameLifecycle::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshotLocked();
}

bool SeatGameLifecycle::wholeMachineReturnRequested() const {
    std::lock_guard lock(mutex_);
    return wholeMachineReturnRequested_;
}

SeatGameCommandResult SeatGameLifecycle::rejectLocked(
    SeatGameResultCode code, std::string diagnostic) const {
    return {code, snapshotLocked(), wholeMachineReturnRequested_,
            boundedDiagnostic(std::move(diagnostic))};
}

SeatGameCommandResult SeatGameLifecycle::finishLocked(
    SeatGameResultCode code, std::string diagnostic) {
    return {code, snapshotLocked(), wholeMachineReturnRequested_,
            boundedDiagnostic(std::move(diagnostic))};
}

SeatGameLifecycle::Entry* SeatGameLifecycle::findLocked(SeatId seatId) {
    const auto found = entries_.find(seatId);
    return found == entries_.end() ? nullptr : &found->second;
}

bool SeatGameLifecycle::rememberCorrelationLocked(std::uint64_t correlationId) {
    if (correlationId == 0 || std::find(correlations_.begin(), correlations_.end(),
                                        correlationId) != correlations_.end()) {
        return false;
    }
    correlations_.push_back(correlationId);
    if (correlations_.size() > kRememberedCorrelations) correlations_.pop_front();
    return true;
}

void SeatGameLifecycle::updateReturnPolicyLocked() {
    wholeMachineReturnRequested_ = std::all_of(
        entries_.begin(), entries_.end(), [](const auto& pair) {
            return pair.second.state.phase == SeatGamePhase::Idle;
        });
}

std::vector<SeatGameState> SeatGameLifecycle::snapshotLocked() const {
    std::vector<SeatGameState> result;
    result.reserve(entries_.size());
    for (const auto& [seatId, entry] : entries_) {
        (void)seatId;
        result.push_back(entry.state);
    }
    return result;
}

bool SeatGameLifecycle::validBinding(const SeatGameBinding& binding) {
    return boundedIdentifier(binding.playerId) && boundedIdentifier(binding.gameId);
}

std::string_view seatGamePhaseName(SeatGamePhase phase) noexcept {
    switch (phase) {
        case SeatGamePhase::Idle: return "idle";
        case SeatGamePhase::Planning: return "planning";
        case SeatGamePhase::Starting: return "starting";
        case SeatGamePhase::Playing: return "playing";
        case SeatGamePhase::Stopping: return "stopping";
        case SeatGamePhase::Degraded: return "degraded";
        case SeatGamePhase::RecoveryRequired: return "recovery-required";
    }
    return "unknown";
}

std::string_view seatGameResultCodeName(SeatGameResultCode code) noexcept {
    switch (code) {
        case SeatGameResultCode::Ok: return "ok";
        case SeatGameResultCode::AlreadySatisfied: return "already-satisfied";
        case SeatGameResultCode::Busy: return "busy";
        case SeatGameResultCode::InvalidSeat: return "invalid-seat";
        case SeatGameResultCode::InvalidState: return "invalid-state";
        case SeatGameResultCode::InvalidBinding: return "invalid-binding";
        case SeatGameResultCode::DuplicateCorrelation: return "duplicate-correlation";
        case SeatGameResultCode::BackendFailure: return "backend-failure";
        case SeatGameResultCode::RecoveryRequired: return "recovery-required";
        case SeatGameResultCode::V1SeatLimitExceeded: return "v1-seat-limit-exceeded";
    }
    return "unknown";
}

} // namespace hydra::runtime
