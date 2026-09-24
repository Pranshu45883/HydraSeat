#include "hydra/runtime_host.hpp"
#include "hydra/session_metrics.hpp"
#include "hydra/two_seat_launch.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace hydra;
using namespace hydra::launch;
using namespace hydra::metrics;
using namespace hydra::runtime;

constexpr auto kProcessReadyTimeout = std::chrono::seconds(5);
constexpr auto kProcessGoneTimeout = std::chrono::seconds(5);
constexpr auto kProcessPollInterval = std::chrono::milliseconds(10);

std::uint64_t durationMicros(std::chrono::steady_clock::time_point start,
                             std::chrono::steady_clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

std::wstring executableDirectory() {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), length))
        .parent_path().wstring();
#else
    return {};
#endif
}

bool exactProcessRunning(const process::ProcessIdentity& identity) noexcept {
#if defined(_WIN32)
    if (!identity.valid()) return false;
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                FALSE, identity.processId);
    if (handle == nullptr) return false;
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    const BOOL times = GetProcessTimes(handle, &creation, &exit, &kernel, &user);
    ULARGE_INTEGER combined{};
    combined.LowPart = creation.dwLowDateTime;
    combined.HighPart = creation.dwHighDateTime;
    const DWORD wait = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return times != FALSE && combined.QuadPart == identity.creationTime100ns &&
           wait == WAIT_TIMEOUT;
#else
    (void)identity;
    return false;
#endif
}

bool waitForIdentityGone(const process::ProcessIdentity& identity,
                         std::chrono::steady_clock::duration timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!exactProcessRunning(identity)) return true;
        std::this_thread::sleep_for(kProcessPollInterval);
    }
    return !exactProcessRunning(identity);
}

struct SharedState;
class ControlledProcessResource;

struct SharedState {
    std::mutex mutex;
    std::map<SeatId, ControlledProcessResource*> processResources;
    std::map<SeatId, std::vector<process::ProcessIdentity>> capturedIdentities;
    std::map<std::string, bool> controlledResourceActive;
    std::vector<std::string> log;
    std::optional<std::pair<SeatId, ResourceKind>> failAfterMutation;
};

std::string stateKey(SeatId seatId, ResourceKind kind) {
    return std::to_string(seatId) + ":" + std::string(resourceKindName(kind));
}

class ControlledMemoryResource final : public ISeatActivationResource {
public:
    ControlledMemoryResource(SeatId seatId, ResourceKind kind,
                             std::shared_ptr<SharedState> state)
        : seatId_(seatId), kind_(kind), state_(std::move(state)) {}

    ResourceKind kind() const noexcept override { return kind_; }

    bool prepare(const SeatActivationPlan& plan,
                 const SeatGameBinding& binding,
                 std::string& error) override {
        if (plan.seatId != seatId_ || binding.gameId != plan.target.gameId) {
            error = "controlled resource plan/binding mismatch";
            return false;
        }
        std::lock_guard lock(state_->mutex);
        state_->log.push_back("prepare:" + stateKey(seatId_, kind_));
        prepared_ = true;
        error.clear();
        return true;
    }

    bool activate(std::string& error) override {
        if (!prepared_) {
            error = "controlled resource activate before prepare";
            return false;
        }
        std::lock_guard lock(state_->mutex);
        state_->controlledResourceActive[stateKey(seatId_, kind_)] = true;
        state_->log.push_back("activate:" + stateKey(seatId_, kind_));
        if (state_->failAfterMutation &&
            state_->failAfterMutation->first == seatId_ &&
            state_->failAfterMutation->second == kind_) {
            error = "injected controlled resource failure after mutation";
            return false;
        }
        error.clear();
        return true;
    }

    bool verifyActive(std::string& error) override {
        if (!active()) {
            error = "controlled resource is not active";
            return false;
        }
        error.clear();
        return true;
    }

    bool rollback(std::string& error) noexcept override {
        std::lock_guard lock(state_->mutex);
        state_->controlledResourceActive[stateKey(seatId_, kind_)] = false;
        state_->log.push_back("rollback:" + stateKey(seatId_, kind_));
        error.clear();
        return true;
    }

    bool verifySafe(std::string& error) noexcept override {
        if (active()) {
            error = "controlled resource remains active";
            return false;
        }
        error.clear();
        return true;
    }

    bool active() const noexcept override {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->controlledResourceActive.find(
            stateKey(seatId_, kind_));
        return found != state_->controlledResourceActive.end() && found->second;
    }

private:
    SeatId seatId_{0};
    ResourceKind kind_{ResourceKind::Recovery};
    std::shared_ptr<SharedState> state_;
    bool prepared_{false};
};

class ControlledProcessResource final : public ISeatActivationResource {
public:
    ControlledProcessResource(SeatActivationPlan plan,
                              std::shared_ptr<SharedState> state)
        : plan_(std::move(plan)), state_(std::move(state)) {}

    ~ControlledProcessResource() override {
        std::string ignored;
        (void)rollback(ignored);
        std::lock_guard lock(state_->mutex);
        const auto found = state_->processResources.find(plan_.seatId);
        if (found != state_->processResources.end() && found->second == this) {
            state_->processResources.erase(found);
        }
    }

    ResourceKind kind() const noexcept override { return ResourceKind::Process; }

    bool prepare(const SeatActivationPlan& plan,
                 const SeatGameBinding& binding,
                 std::string& error) override {
        if (plan.seatId != plan_.seatId || plan.fingerprint != plan_.fingerprint ||
            binding.gameId != plan.target.gameId) {
            error = "controlled process resource plan/binding mismatch";
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }

    bool activate(std::string& error) override {
        if (!prepared_ || group_) {
            error = "controlled process activation state is invalid";
            return false;
        }
#if !defined(_WIN32)
        error = "controlled Windows process resource is unavailable on this platform";
        return false;
#else
        auto result = process::ProcessLauncher::launch(plan_.target.process, &error);
        if (!result.group) {
            if (error.empty()) error = "controlled process launch failed";
            return false;
        }
        group_ = std::move(result.group);
        {
            std::lock_guard lock(state_->mutex);
            state_->processResources[plan_.seatId] = this;
            state_->capturedIdentities[plan_.seatId] = {result.root};
            state_->log.push_back("activate:" + stateKey(plan_.seatId,
                                                          ResourceKind::Process));
            if (state_->failAfterMutation &&
                state_->failAfterMutation->first == plan_.seatId &&
                state_->failAfterMutation->second == ResourceKind::Process) {
                error = "injected controlled process failure after launch";
                return false;
            }
        }
        error.clear();
        return true;
#endif
    }

    bool verifyActive(std::string& error) override {
        if (!group_) {
            error = "controlled process group is absent";
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + kProcessReadyTimeout;
        process::ProcessTreeSnapshot snapshot;
        while (std::chrono::steady_clock::now() < deadline) {
            snapshot = group_->snapshot();
            if (snapshot.runningCount() >= 2u) break;
            if (snapshot.runningCount() == 0u) break;
            std::this_thread::sleep_for(kProcessPollInterval);
        }
        if (snapshot.runningCount() < 2u) {
            error = "controlled process tree did not expose root plus child before timeout";
            return false;
        }
        std::vector<process::ProcessIdentity> identities;
        identities.reserve(snapshot.processes.size());
        for (const auto& record : snapshot.processes) {
            if (!record.exited && record.identity.valid()) {
                identities.push_back(record.identity);
            }
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->capturedIdentities[plan_.seatId] = std::move(identities);
        }
        error.clear();
        return true;
    }

    bool rollback(std::string& error) noexcept override {
        if (!group_) {
            error.clear();
            return true;
        }
        const auto snapshot = group_->snapshot();
        {
            std::lock_guard lock(state_->mutex);
            auto& identities = state_->capturedIdentities[plan_.seatId];
            for (const auto& record : snapshot.processes) {
                if (!record.identity.valid()) continue;
                const auto duplicate = std::find_if(
                    identities.begin(), identities.end(),
                    [&](const process::ProcessIdentity& existing) {
                        return existing.processId == record.identity.processId &&
                               existing.creationTime100ns ==
                                   record.identity.creationTime100ns;
                    });
                if (duplicate == identities.end()) identities.push_back(record.identity);
            }
            state_->log.push_back("rollback:" + stateKey(plan_.seatId,
                                                          ResourceKind::Process));
        }
        process::ProcessStopPolicy policy;
        policy.gracefulTimeoutMs = 500u;
        policy.forcedTimeoutMs = 2000u;
        if (!group_->stop(policy, &error) || !group_->waitForEmpty(0u)) {
            if (error.empty()) {
                error = "controlled process group did not reach empty after stop";
            }
            return false;
        }
        group_.reset();
        error.clear();
        return true;
    }

    bool verifySafe(std::string& error) noexcept override {
        std::vector<process::ProcessIdentity> identities;
        {
            std::lock_guard lock(state_->mutex);
            identities = state_->capturedIdentities[plan_.seatId];
        }
        if (group_ && group_->snapshot().runningCount() != 0u) {
            error = "controlled process group still owns running processes";
            return false;
        }
        for (const auto& identity : identities) {
            if (!waitForIdentityGone(identity, kProcessGoneTimeout)) {
                error = "captured controlled process identity remains live after rollback";
                return false;
            }
        }
        error.clear();
        return true;
    }

    bool active() const noexcept override {
        return group_ && group_->snapshot().runningCount() != 0u;
    }

    bool forceControlledExit(std::string& error) noexcept {
        return rollback(error);
    }

    std::optional<process::ProcessIdentity> rootIdentity() const {
        if (!group_) return std::nullopt;
        const auto identity = group_->rootIdentity();
        if (!identity.valid()) return std::nullopt;
        return identity;
    }

private:
    SeatActivationPlan plan_;
    std::shared_ptr<SharedState> state_;
    std::unique_ptr<process::SeatProcessGroup> group_;
    bool prepared_{false};
};

class ControlledResourceFactory final : public ISeatActivationResourceFactory {
public:
    explicit ControlledResourceFactory(std::shared_ptr<SharedState> state)
        : state_(std::move(state)) {}

    std::unique_ptr<ISeatActivationResource> create(
        ResourceKind kind,
        const SeatActivationPlan& plan,
        std::string& error) override {
        error.clear();
        if (kind == ResourceKind::Process) {
            return std::make_unique<ControlledProcessResource>(plan, state_);
        }
        return std::make_unique<ControlledMemoryResource>(plan.seatId, kind, state_);
    }

private:
    std::shared_ptr<SharedState> state_;
};

SeatLaunchInput makeSeatInput(SeatId seatId,
                              std::wstring childExecutable,
                              std::string gameId) {
    SeatLaunchInput input;
    input.seat.seatId = seatId;
    input.seat.name = L"Controlled Seat " + std::to_wstring(seatId);
    input.seat.displayIds = {L"CONTROLLED-DISPLAY-" + std::to_wstring(seatId)};
    input.seat.primaryDisplayId = input.seat.displayIds.front();
    input.seat.keyboardIds = {L"CONTROLLED-KEYBOARD-" + std::to_wstring(seatId)};
    input.seat.mouseIds = {L"CONTROLLED-MOUSE-" + std::to_wstring(seatId)};
    input.seat.active = true;

    input.target.gameId = std::move(gameId);
    input.target.process.seatId = seatId;
    input.target.process.executablePath = std::move(childExecutable);
    input.target.process.arguments = {
        L"--depth", L"1", L"--sleep-ms", L"30000",
        L"--descendant-sleep-ms", L"30000"};
    input.target.process.workingDirectory = executableDirectory();
#if defined(_WIN64)
    input.target.process.architecture = process::ProcessArchitecture::X64;
#else
    input.target.process.architecture = process::ProcessArchitecture::X86;
#endif
    input.target.requirements.display = true;
    input.target.requirements.keyboard = true;
    input.target.requirements.mouse = true;
    input.target.requirements.controller = false;
    input.target.requirements.audioOutput = false;
    input.target.requirements.windowOwnership = true;
    input.target.requirements.recovery = true;
    return input;
}

InputMetricSample metricSample(std::uint64_t correlation,
                               InputMetricStage stage,
                               std::uint64_t timestamp,
                               std::uint32_t seat,
                               std::uint32_t processId) {
    InputMetricSample sample;
    sample.correlationId = correlation;
    sample.stage = stage;
    sample.timestampMicros = timestamp;
    sample.expectedSeatId = seat;
    sample.targetProcessId = processId;
    sample.eventClass = InputMetricEventClass::Key;
    if (stage == InputMetricStage::TargetApplied ||
        stage == InputMetricStage::TargetQueried) {
        sample.receivingSeatId = seat;
        sample.receivingProcessId = processId;
    }
    return sample;
}

void appendSyntheticCompleteInput(InputMetricsSnapshot& snapshot,
                                  std::uint64_t correlation,
                                  std::uint64_t base,
                                  std::uint32_t seat,
                                  std::uint32_t processId) {
    snapshot.samples.push_back(metricSample(
        correlation, InputMetricStage::PhysicalObserved, base, seat, processId));
    snapshot.samples.push_back(metricSample(
        correlation, InputMetricStage::RouteEnqueued, base + 10u, seat, processId));
    snapshot.samples.push_back(metricSample(
        correlation, InputMetricStage::RouteDequeued, base + 20u, seat, processId));
    snapshot.samples.push_back(metricSample(
        correlation, InputMetricStage::RouteWritten, base + 30u, seat, processId));
    snapshot.samples.push_back(metricSample(
        correlation, InputMetricStage::TargetApplied, base + 40u, seat, processId));
    snapshot.samples.push_back(metricSample(
        correlation, InputMetricStage::TargetQueried, base + 60u, seat, processId));
}

const SeatActivationPlan* findSeatPlan(const TwoSeatLaunchPlan& plan, SeatId seatId) {
    const auto found = std::find_if(
        plan.seats.begin(), plan.seats.end(),
        [&](const SeatActivationPlan& seat) { return seat.seatId == seatId; });
    return found == plan.seats.end() ? nullptr : &*found;
}

SeatGamePhase seatPhase(const HostRuntimeSnapshot& snapshot, SeatId seatId) {
    const auto found = std::find_if(
        snapshot.seatGames.begin(), snapshot.seatGames.end(),
        [&](const SeatGameState& seat) { return seat.seatId == seatId; });
    return found == snapshot.seatGames.end()
        ? SeatGamePhase::RecoveryRequired : found->phase;
}

std::optional<process::ProcessIdentity> currentRoot(
    const std::shared_ptr<SharedState>& state, SeatId seatId) {
    std::lock_guard lock(state->mutex);
    const auto found = state->processResources.find(seatId);
    if (found == state->processResources.end() || found->second == nullptr) {
        return std::nullopt;
    }
    return found->second->rootIdentity();
}

bool forceSeatProcessExit(const std::shared_ptr<SharedState>& state,
                          SeatId seatId,
                          std::string& error) {
    ControlledProcessResource* resource = nullptr;
    {
        std::lock_guard lock(state->mutex);
        const auto found = state->processResources.find(seatId);
        if (found != state->processResources.end()) resource = found->second;
    }
    if (resource == nullptr) {
        error = "controlled Seat process resource is unavailable";
        return false;
    }
    return resource->forceControlledExit(error);
}

bool capturedIdentitiesGone(const std::shared_ptr<SharedState>& state,
                            SeatId seatId) {
    std::vector<process::ProcessIdentity> identities;
    {
        std::lock_guard lock(state->mutex);
        identities = state->capturedIdentities[seatId];
    }
    return std::all_of(
        identities.begin(), identities.end(),
        [](const process::ProcessIdentity& identity) {
            return waitForIdentityGone(identity, kProcessGoneTimeout);
        });
}

struct HappyRun {
    bool success{false};
    std::string diagnostic;
    SessionMetricsReport report;
    std::string json;
    bool seat1SurvivedSeat2Stop{false};
    bool seat1SurvivedSeat2NaturalExit{false};
    bool seat2RestartedAsNewIdentity{false};
    bool noOwnedProcessOrphans{false};
};

HappyRun runHappySession(const std::wstring& childExecutable) {
    HappyRun result;
    const std::vector<SeatLaunchInput> inputs{
        makeSeatInput(1, childExecutable, "controlled-game-a"),
        makeSeatInput(2, childExecutable, "controlled-game-b")};
    const auto compiled = compileTwoSeatLaunchPlan(inputs);
    if (!compiled.plan) {
        result.diagnostic = "controlled MVP launch plan failed to compile";
        return result;
    }
    const auto* seat1Plan = findSeatPlan(*compiled.plan, 1);
    const auto* seat2Plan = findSeatPlan(*compiled.plan, 2);
    if (seat1Plan == nullptr || seat2Plan == nullptr) {
        result.diagnostic = "controlled MVP launch plan lost a Seat";
        return result;
    }

    auto state = std::make_shared<SharedState>();
    auto resources = std::make_shared<ControlledResourceFactory>(state);
    auto gameFactory = std::make_shared<PlannedSeatGameInstanceFactory>(
        *compiled.plan, resources);
    RuntimeHost host({}, gameFactory);
    std::vector<SeatConfig> profile{seat1Plan->seat, seat2Plan->seat};

    std::uint64_t correlation = 1000u;
    if (!host.loadProfile(profile, 1, correlation++).succeeded() ||
        !host.plan(correlation++).succeeded() ||
        !host.prepare(correlation++).succeeded() ||
        !host.start(correlation++).succeeded()) {
        result.diagnostic = "RuntimeHost failed to enter active controlled MVP state";
        return result;
    }

    SeatSessionMetrics seat1Metrics;
    seat1Metrics.seatId = 1;
    seat1Metrics.controller = CapabilityOutcome::NotRequired;
    seat1Metrics.audio = CapabilityOutcome::NotRequired;
    SeatSessionMetrics seat2Metrics = seat1Metrics;
    seat2Metrics.seatId = 2;

    const auto start1 = std::chrono::steady_clock::now();
    if (!host.assignSeatGame(1, {"mario", seat1Plan->target.gameId},
                             correlation++).succeeded() ||
        !host.startSeatGame(1, correlation++).succeeded()) {
        result.diagnostic = "Seat 1 failed to start controlled MVP target";
        return result;
    }
    seat1Metrics.launchDurationMicros = durationMicros(
        start1, std::chrono::steady_clock::now());

    const auto start2 = std::chrono::steady_clock::now();
    if (!host.assignSeatGame(2, {"luigi", seat2Plan->target.gameId},
                             correlation++).succeeded() ||
        !host.startSeatGame(2, correlation++).succeeded()) {
        result.diagnostic = "Seat 2 failed to start controlled MVP target";
        return result;
    }
    seat2Metrics.launchDurationMicros = durationMicros(
        start2, std::chrono::steady_clock::now());

    auto seat1Root = currentRoot(state, 1);
    auto seat2Root = currentRoot(state, 2);
    if (!seat1Root || !seat2Root || !exactProcessRunning(*seat1Root) ||
        !exactProcessRunning(*seat2Root)) {
        result.diagnostic = "controlled MVP root process identity was not live";
        return result;
    }

    seat1Metrics.processStarted = true;
    seat2Metrics.processStarted = true;
    seat1Metrics.windowOwnershipVerified = true;
    seat2Metrics.windowOwnershipVerified = true;
    seat1Metrics.displayPlacementVerified = true;
    seat2Metrics.displayPlacementVerified = true;
    seat1Metrics.inputRouteReady = true;
    seat2Metrics.inputRouteReady = true;

    const auto stop2Start = std::chrono::steady_clock::now();
    if (!host.stopSeatGame(2, correlation++).succeeded()) {
        result.diagnostic = "Seat 2 controlled stop failed";
        return result;
    }
    seat2Metrics.stopDurationMicros = durationMicros(
        stop2Start, std::chrono::steady_clock::now());
    result.seat1SurvivedSeat2Stop =
        exactProcessRunning(*seat1Root) && !exactProcessRunning(*seat2Root) &&
        seatPhase(host.snapshot(), 1) == SeatGamePhase::Playing &&
        seatPhase(host.snapshot(), 2) == SeatGamePhase::Idle;
    if (!result.seat1SurvivedSeat2Stop) {
        result.diagnostic = "Seat 2 stop changed healthy Seat 1 or left Seat 2 process live";
        return result;
    }

    if (!host.assignSeatGame(2, {"luigi", seat2Plan->target.gameId},
                             correlation++).succeeded() ||
        !host.startSeatGame(2, correlation++).succeeded()) {
        result.diagnostic = "Seat 2 controlled restart failed";
        return result;
    }
    const auto seat2RestartRoot = currentRoot(state, 2);
    result.seat2RestartedAsNewIdentity =
        seat2RestartRoot && exactProcessRunning(*seat2RestartRoot) &&
        (seat2RestartRoot->processId != seat2Root->processId ||
         seat2RestartRoot->creationTime100ns != seat2Root->creationTime100ns);
    if (!result.seat2RestartedAsNewIdentity) {
        result.diagnostic = "Seat 2 restart did not produce a new exact process identity";
        return result;
    }

    std::string exitError;
    if (!forceSeatProcessExit(state, 2, exitError) ||
        !host.observeSeatGameExit(2, true, "controlled normal exit",
                                  correlation++).succeeded()) {
        result.diagnostic = "Seat 2 controlled natural-exit cleanup failed: " + exitError;
        return result;
    }
    result.seat1SurvivedSeat2NaturalExit =
        exactProcessRunning(*seat1Root) &&
        !exactProcessRunning(*seat2RestartRoot) &&
        seatPhase(host.snapshot(), 1) == SeatGamePhase::Playing &&
        seatPhase(host.snapshot(), 2) == SeatGamePhase::Idle;
    if (!result.seat1SurvivedSeat2NaturalExit) {
        result.diagnostic = "Seat 2 natural exit changed healthy Seat 1 or failed Seat-local cleanup";
        return result;
    }

    const auto stop1Start = std::chrono::steady_clock::now();
    if (!host.stopSeatGame(1, correlation++).succeeded()) {
        result.diagnostic = "Seat 1 final controlled stop failed";
        return result;
    }
    seat1Metrics.stopDurationMicros = durationMicros(
        stop1Start, std::chrono::steady_clock::now());
    seat1Metrics.rollbackDurationMicros = seat1Metrics.stopDurationMicros;
    seat2Metrics.rollbackDurationMicros = seat2Metrics.stopDurationMicros;

    const auto beforeReturn = host.snapshot();
    if (!beforeReturn.wholeMachineReturnRequested ||
        seatPhase(beforeReturn, 1) != SeatGamePhase::Idle ||
        seatPhase(beforeReturn, 2) != SeatGamePhase::Idle) {
        result.diagnostic = "both controlled Seats Idle did not request whole-machine return";
        return result;
    }

    const auto returnStart = std::chrono::steady_clock::now();
    const auto returned = host.stopAndReturnToWindows(correlation++);
    const auto returnDuration = durationMicros(
        returnStart, std::chrono::steady_clock::now());
    if (!returned.succeeded() || host.snapshot().sessionPhase != SeatSessionPhase::Idle) {
        result.diagnostic = "RuntimeHost failed verified return-to-Windows transition";
        return result;
    }
    seat1Metrics.rollbackDurationMicros += returnDuration;
    seat2Metrics.rollbackDurationMicros += returnDuration;

    result.noOwnedProcessOrphans =
        capturedIdentitiesGone(state, 1) && capturedIdentitiesGone(state, 2);
    if (!result.noOwnedProcessOrphans) {
        result.diagnostic = "captured controlled process identity survived final rollback";
        return result;
    }

    SessionMetricsBuildInput evidence;
    evidence.planFingerprint = compiled.plan->fingerprint;
    // The lifecycle uses real owned processes, but input/display/controller/audio
    // evidence below is synthetic. The report deliberately takes the most
    // conservative origin so this run cannot become physical validation.
    evidence.origin = EvidenceOrigin::Synthetic;
    appendSyntheticCompleteInput(evidence.input, 1u, 1000u, 1u,
                                 seat1Root->processId);
    appendSyntheticCompleteInput(evidence.input, 2u, 2000u, 2u,
                                 seat2RestartRoot->processId);
    InputMetricSample resource;
    resource.stage = InputMetricStage::HostResourceSample;
    resource.timestampMicros = 3000u;
    resource.hostCpuTimeMicros = 1u;
    resource.hostWorkingSetBytes = 1u;
    evidence.input.samples.push_back(resource);
    evidence.seats = {seat1Metrics, seat2Metrics};
    evidence.finalState = SessionFinalState::ReturnedToWindows;
    evidence.rollbackAttempted = true;
    evidence.rollbackVerified = true;
    if (buildSessionMetricsReport(evidence, result.report) !=
        SessionMetricsResult::Success) {
        result.diagnostic = "controlled MVP integrated evidence report failed to build";
        return result;
    }
    if (result.report.sessionVerdict != EvidenceVerdict::Pass ||
        result.report.physicalValidationEligible ||
        result.report.origin != EvidenceOrigin::Synthetic) {
        result.diagnostic = "controlled MVP report crossed the physical evidence boundary";
        return result;
    }
    result.json = encodeSessionMetricsReportJson(result.report);
    result.success = true;
    return result;
}

bool runSeat2Fault(const std::wstring& childExecutable, std::string& diagnostic) {
    const std::vector<SeatLaunchInput> inputs{
        makeSeatInput(1, childExecutable, "controlled-game-a"),
        makeSeatInput(2, childExecutable, "controlled-game-b")};
    const auto compiled = compileTwoSeatLaunchPlan(inputs);
    if (!compiled.plan) {
        diagnostic = "fault matrix plan failed to compile";
        return false;
    }
    const auto* seat1Plan = findSeatPlan(*compiled.plan, 1);
    const auto* seat2Plan = findSeatPlan(*compiled.plan, 2);
    if (seat1Plan == nullptr || seat2Plan == nullptr) {
        diagnostic = "fault matrix plan lost a Seat";
        return false;
    }
    auto state = std::make_shared<SharedState>();
    auto resources = std::make_shared<ControlledResourceFactory>(state);
    auto gameFactory = std::make_shared<PlannedSeatGameInstanceFactory>(
        *compiled.plan, resources);
    RuntimeHost host({}, gameFactory);
    std::vector<SeatConfig> profile{seat1Plan->seat, seat2Plan->seat};
    std::uint64_t correlation = 4000u;
    if (!host.loadProfile(profile, 1, correlation++).succeeded() ||
        !host.plan(correlation++).succeeded() ||
        !host.prepare(correlation++).succeeded() ||
        !host.start(correlation++).succeeded() ||
        !host.assignSeatGame(1, {"mario", seat1Plan->target.gameId},
                             correlation++).succeeded() ||
        !host.startSeatGame(1, correlation++).succeeded()) {
        diagnostic = "fault matrix failed before Seat 2 injection";
        return false;
    }
    const auto seat1Root = currentRoot(state, 1);
    if (!seat1Root || !exactProcessRunning(*seat1Root)) {
        diagnostic = "fault matrix Seat 1 exact process is not live";
        return false;
    }
    {
        std::lock_guard lock(state->mutex);
        state->failAfterMutation = std::make_pair(SeatId{2}, ResourceKind::Input);
    }
    if (!host.assignSeatGame(2, {"luigi", seat2Plan->target.gameId},
                             correlation++).succeeded()) {
        diagnostic = "fault matrix Seat 2 binding failed";
        return false;
    }
    const auto failed = host.startSeatGame(2, correlation++);
    if (failed.succeeded() ||
        failed.code != SeatGameResultCode::BackendFailure) {
        diagnostic = "fault matrix Seat 2 injected failure was not contained as backend failure";
        return false;
    }
    if (!exactProcessRunning(*seat1Root) ||
        seatPhase(host.snapshot(), 1) != SeatGamePhase::Playing ||
        seatPhase(host.snapshot(), 2) != SeatGamePhase::Idle ||
        !capturedIdentitiesGone(state, 2)) {
        diagnostic = "Seat 2 fault changed Seat 1 or left a controlled Seat 2 process orphan";
        return false;
    }
    if (!host.stopSeatGame(1, correlation++).succeeded() ||
        !host.stopAndReturnToWindows(correlation++).succeeded() ||
        !capturedIdentitiesGone(state, 1)) {
        diagnostic = "fault matrix final cleanup failed";
        return false;
    }
    diagnostic.clear();
    return true;
}

int runSelfTest(const std::wstring& childExecutable, bool printJson) {
#if !defined(_WIN32)
    std::cerr << "P5-MVP-01 controlled process harness requires Windows.\n";
    return EXIT_FAILURE;
#else
    const auto happy = runHappySession(childExecutable);
    if (!happy.success) {
        std::cerr << "controlled MVP happy-path failure: " << happy.diagnostic << '\n';
        return EXIT_FAILURE;
    }
    std::string faultDiagnostic;
    if (!runSeat2Fault(childExecutable, faultDiagnostic)) {
        std::cerr << "controlled MVP fault-path failure: " << faultDiagnostic << '\n';
        return EXIT_FAILURE;
    }
    if (printJson) {
        std::cout << happy.json << '\n';
    } else {
        std::cout
            << "Controlled two-Seat MVP self-test passed: "
            << "real Job Object process lifecycle, independent Seat stop/restart/exit, "
            << "fault isolation, no captured process orphans, verified host return, "
            << "and synthetic-origin integrated evidence boundary.\n";
    }
    return EXIT_SUCCESS;
#endif
}

void usage() {
    std::cout
        << "HydraSeat P5-MVP-01 controlled two-Seat harness\n"
        << "  --self-test   run the complete controlled happy/fault matrix\n"
        << "  --run         run the same matrix and print aggregate evidence JSON\n"
        << "  --help        show this help\n"
        << "\n"
        << "This harness uses real HydraSeat-owned process trees but synthetic "
           "display/input/controller/audio resources. It is not physical validation.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        usage();
        return EXIT_SUCCESS;
    }
    if (argc != 2) {
        usage();
        return EXIT_FAILURE;
    }
    const auto directory = executableDirectory();
    const auto child = std::filesystem::path(directory) /
#if defined(_WIN32)
        L"hydra_process_tree_child.exe";
#else
        L"hydra_process_tree_child";
#endif
    const std::string argument = argv[1];
    if (argument == "--self-test") return runSelfTest(child.wstring(), false);
    if (argument == "--run") return runSelfTest(child.wstring(), true);
    if (argument == "--help" || argument == "-h") {
        usage();
        return EXIT_SUCCESS;
    }
    usage();
    return EXIT_FAILURE;
}
