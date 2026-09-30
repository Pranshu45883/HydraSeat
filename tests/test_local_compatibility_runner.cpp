#include "hydra/local_compatibility_runner.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::local_compatibility;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

process::ProcessIdentity identity(std::uint32_t pid, std::uint64_t creation,
                                  std::wstring path = L"C:\\Games\\Fixture.exe") {
    return {pid, creation, std::move(path)};
}

windowing::WindowIdentity windowIdentity(const process::ProcessIdentity& processIdentity) {
    windowing::WindowIdentity value;
    value.nativeHandle = 0x1234u;
    value.process = processIdentity;
    value.threadId = 77u;
    value.trackerGeneration = 1u;
    return value;
}

struct FakeProcessState {
    SeatId seatId{1u};
    process::ChildTrackingCapability capability{process::ChildTrackingCapability::FullJobObject};
    process::ProcessIdentity root{identity(101u, 1001u)};
    process::ProcessIdentity child{identity(202u, 2002u, L"C:\\Games\\FixtureChild.exe")};
    bool rootRunning{true};
    bool childRunning{false};
    bool trackingComplete{true};
    bool stopSucceeds{true};
    bool keepChildAfterStop{false};
    int naturalExitAfterSnapshots{-1};
    mutable int snapshotCalls{0};
    int stopCalls{0};
    std::uint32_t lastGracefulTimeoutMs{0u};
    std::uint32_t lastForcedTimeoutMs{0u};
};

class FakeOwnedProcess final : public LocalCompatibilityOwnedProcess {
public:
    explicit FakeOwnedProcess(std::shared_ptr<FakeProcessState> state)
        : state_(std::move(state)) {}

    SeatId seatId() const noexcept override { return state_->seatId; }
    process::ChildTrackingCapability capability() const noexcept override {
        return state_->capability;
    }
    process::ProcessIdentity rootIdentity() const override { return state_->root; }

    process::ProcessTreeSnapshot snapshot() const override {
        ++state_->snapshotCalls;
        if (state_->naturalExitAfterSnapshots >= 0 &&
            state_->snapshotCalls > state_->naturalExitAfterSnapshots) {
            state_->rootRunning = false;
            state_->childRunning = false;
        }
        process::ProcessTreeSnapshot tree;
        tree.seatId = state_->seatId;
        tree.capability = state_->capability;
        tree.root = state_->root;
        tree.sequence = static_cast<std::uint64_t>(state_->snapshotCalls);
        tree.trackingComplete = state_->trackingComplete;

        process::ProcessRecord rootRecord;
        rootRecord.identity = state_->root;
        rootRecord.root = true;
        rootRecord.exited = !state_->rootRunning;
        tree.processes.push_back(rootRecord);
        if (state_->child.valid()) {
            process::ProcessRecord childRecord;
            childRecord.identity = state_->child;
            childRecord.parentProcessId = state_->root.processId;
            childRecord.parentIdentity = state_->root;
            childRecord.parentIdentityVerified = true;
            childRecord.root = false;
            childRecord.exited = !state_->childRunning;
            tree.processes.push_back(childRecord);
        }
        return tree;
    }

    bool ownsExactIdentity(const process::ProcessIdentity& candidate) const override {
        return state_->root.sameInstance(candidate) || state_->child.sameInstance(candidate);
    }

    bool waitForEmpty(std::uint32_t) const override {
        return !state_->rootRunning && !state_->childRunning;
    }

    bool stop(const process::ProcessStopPolicy& policy,
              std::string* error) noexcept override {
        ++state_->stopCalls;
        state_->lastGracefulTimeoutMs = policy.gracefulTimeoutMs;
        state_->lastForcedTimeoutMs = policy.forcedTimeoutMs;
        if (!state_->stopSucceeds) {
            if (error) *error = "fake stop failed";
            return false;
        }
        state_->rootRunning = false;
        if (!state_->keepChildAfterStop) state_->childRunning = false;
        return true;
    }

private:
    std::shared_ptr<FakeProcessState> state_;
};

struct FakeBackendState {
    std::shared_ptr<FakeProcessState> process{std::make_shared<FakeProcessState>()};
    bool launchSucceeds{true};
    std::uint32_t launchDelayMs{0u};
    int launchCalls{0};
    std::function<void()> onLaunch;
};

class FakeProcessBackend final : public LocalCompatibilityProcessBackend {
public:
    explicit FakeProcessBackend(std::shared_ptr<FakeBackendState> state)
        : state_(std::move(state)) {}

    std::unique_ptr<LocalCompatibilityOwnedProcess> launch(
        const process::ProcessLaunchSpec&,
        std::string* error) override {
        ++state_->launchCalls;
        if (state_->launchDelayMs != 0u) {
            std::this_thread::sleep_for(std::chrono::milliseconds(state_->launchDelayMs));
        }
        if (state_->onLaunch) state_->onLaunch();
        if (!state_->launchSucceeds) {
            if (error) *error = "fake launch failed";
            return nullptr;
        }
        return std::make_unique<FakeOwnedProcess>(state_->process);
    }

private:
    std::shared_ptr<FakeBackendState> state_;
};

struct FakeWindowState {
    bool startSucceeds{true};
    bool identityValidates{true};
    bool stopped{false};
    int startCalls{0};
    int updateCalls{0};
    int bindAfterUpdates{1};
    std::optional<windowing::WindowIdentity> target;
};

class FakeWindowObserver final : public LocalCompatibilityWindowObserver {
public:
    explicit FakeWindowObserver(std::shared_ptr<FakeWindowState> state)
        : state_(std::move(state)) {}

    bool start(std::string* error) override {
        ++state_->startCalls;
        if (!state_->startSucceeds && error) *error = "fake observer failed";
        return state_->startSucceeds;
    }

    void updateProcessTree(const process::ProcessTreeSnapshot&) override {
        ++state_->updateCalls;
    }

    std::optional<windowing::WindowIdentity> visualTarget(SeatId) const override {
        if (state_->updateCalls < state_->bindAfterUpdates) return std::nullopt;
        return state_->target;
    }

    bool validateIdentity(const windowing::WindowIdentity&) const noexcept override {
        return state_->identityValidates;
    }

    void stop() noexcept override { state_->stopped = true; }

private:
    std::shared_ptr<FakeWindowState> state_;
};

struct Fixture {
    std::shared_ptr<FakeBackendState> backendState{std::make_shared<FakeBackendState>()};
    std::shared_ptr<FakeWindowState> windowState{std::make_shared<FakeWindowState>()};
    FakeProcessBackend backend{backendState};
    FakeWindowObserver observer{windowState};

    Fixture() {
        windowState->target = windowIdentity(backendState->process->root);
    }

    LocalCompatibilityDependencies dependencies() {
        return {&backend, &observer};
    }
};

LocalCompatibilityRequest validRequest() {
    LocalCompatibilityRequest request;
    request.launch.seatId = 1u;
    request.launch.executablePath = L"C:\\Games\\Fixture.exe";
    request.launch.arguments = {L"--local-check"};
    request.launch.workingDirectory = L"C:\\Games";
    request.launch.containment = process::ProcessContainmentPolicy::RequireJobObject;
    request.planFingerprint = 0x12345678u;
    request.targetRisk = LocalCompatibilityTargetRisk::Standard;
    request.limits.startupWindowTimeoutMs = 4u;
    request.limits.observationDurationMs = 1u;
    request.limits.gracefulCleanupTimeoutMs = 0u;
    request.limits.forcedCleanupTimeoutMs = 2u;
    request.limits.pollIntervalMs = 1u;
    return request;
}

const metrics::SeatSessionMetrics* reportSeat(const metrics::SessionMetricsReport& report,
                                               SeatId seatId) {
    for (const auto& seat : report.seats) {
        if (seat.seatId == seatId) return &seat;
    }
    return nullptr;
}

void testControlledOwnedWindowProducesTruthfulInsufficientReport() {
    Fixture fixture;
    const auto output = runLocalCompatibilityCheck(validRequest(), fixture.dependencies());
    check(output.diagnostic.succeeded() && output.report.has_value(),
          "exact owned process and authoritative window produce one complete local-check report");
    if (!output.report) return;

    const auto& report = *output.report;
    const auto* checked = reportSeat(report, 1u);
    const auto* untouched = reportSeat(report, 2u);
    check(report.origin == metrics::EvidenceOrigin::ControlledProcess &&
              !report.physicalValidationEligible,
          "runner evidence is always ControlledProcess and never physical-validation eligible");
    check(report.isolationVerdict == metrics::EvidenceVerdict::InsufficientEvidence &&
              report.sessionVerdict == metrics::EvidenceVerdict::InsufficientEvidence &&
              report.input.uniqueInputEvents == 0u,
          "unmeasured input remains insufficient and cannot become a local compatibility pass");
    check(checked != nullptr && checked->processStarted && checked->windowOwnershipVerified &&
              !checked->displayPlacementVerified && !checked->inputRouteReady &&
              checked->controller == metrics::CapabilityOutcome::MissingEvidence &&
              checked->audio == metrics::CapabilityOutcome::MissingEvidence,
          "checked Seat reports only process/window facts and leaves other capability evidence missing");
    check(untouched != nullptr && !untouched->processStarted &&
              !untouched->windowOwnershipVerified && !untouched->displayPlacementVerified &&
              !untouched->inputRouteReady &&
              untouched->controller == metrics::CapabilityOutcome::MissingEvidence &&
              untouched->audio == metrics::CapabilityOutcome::MissingEvidence,
          "the other Seat is not fabricated as measured or ready");
    check(output.diagnostic.facts.cleanupAttempted && output.diagnostic.facts.cleanupVerified &&
              output.diagnostic.facts.returnedToWindowsVerified &&
              output.diagnostic.facts.remainingOwnedProcesses == 0u &&
              fixture.backendState->process->stopCalls == 1,
          "successful result is emitted only after exact owned-process cleanup verifies zero survivors");
}

void testNoWindowTimesOutAndCleansOwnedTree() {
    Fixture fixture;
    fixture.windowState->target.reset();
    fixture.backendState->process->childRunning = true;
    auto request = validRequest();
    request.limits.startupWindowTimeoutMs = 2u;

    const auto output = runLocalCompatibilityCheck(request, fixture.dependencies());
    check(output.diagnostic.result == LocalCompatibilityResult::WindowTimeout && !output.report,
          "window timeout is a failed observation and never fabricates a report");
    check(!output.diagnostic.facts.windowOwnershipVerified &&
              output.diagnostic.facts.cleanupVerified &&
              output.diagnostic.facts.remainingOwnedProcesses == 0u &&
              !fixture.backendState->process->rootRunning &&
              !fixture.backendState->process->childRunning,
          "window timeout stops only the exact owned root+child group and verifies it empty");
}

void testEarlyExitAndNaturalExitAreDistinct() {
    {
        Fixture fixture;
        fixture.backendState->process->naturalExitAfterSnapshots = 1;
        fixture.windowState->bindAfterUpdates = 100;
        const auto output = runLocalCompatibilityCheck(validRequest(), fixture.dependencies());
        check(output.diagnostic.result == LocalCompatibilityResult::EarlyProcessExit &&
                  output.diagnostic.facts.naturalExitObserved && !output.report &&
                  output.diagnostic.facts.cleanupVerified,
              "process exit before authoritative window is an explicit failed observation with cleanup");
    }
    {
        Fixture fixture;
        fixture.backendState->process->naturalExitAfterSnapshots = 2;
        const auto output = runLocalCompatibilityCheck(validRequest(), fixture.dependencies());
        check(output.diagnostic.result == LocalCompatibilityResult::Success && output.report &&
                  output.diagnostic.facts.windowOwnershipVerified &&
                  output.diagnostic.facts.naturalExitObserved &&
                  output.diagnostic.facts.returnedToWindowsVerified,
              "natural exit after an authoritative window is a complete bounded observation");
    }
}

void testExplicitCancellationCleansOnlyLaunchedGroup() {
    Fixture fixture;
    std::stop_source stop;
    fixture.backendState->onLaunch = [&] { stop.request_stop(); };
    const auto output = runLocalCompatibilityCheck(
        validRequest(), fixture.dependencies(), stop.get_token());
    check(output.diagnostic.result == LocalCompatibilityResult::Cancelled && !output.report &&
              fixture.backendState->launchCalls == 1 &&
              fixture.backendState->process->stopCalls == 1 &&
              output.diagnostic.facts.cleanupVerified &&
              output.diagnostic.facts.remainingOwnedProcesses == 0u,
          "explicit cancellation after launch cleans and verifies only the exact owned group");
}

void testLaunchPastStartupBoundFailsAndCleansOwnedGroup() {
    Fixture fixture;
    fixture.backendState->launchDelayMs = 5u;
    auto request = validRequest();
    request.limits.startupWindowTimeoutMs = 1u;

    const auto output = runLocalCompatibilityCheck(request, fixture.dependencies());
    check(output.diagnostic.result == LocalCompatibilityResult::StartupTimeout && !output.report &&
              fixture.backendState->process->stopCalls == 1 &&
              output.diagnostic.facts.cleanupVerified &&
              output.diagnostic.facts.remainingOwnedProcesses == 0u,
          "launch work that already exceeded the startup bound is cleaned and cannot continue to window success");
}

void testForeignAndPidReuseWindowsNeverBecomeOwned() {
    {
        Fixture fixture;
        fixture.windowState->target = windowIdentity(
            identity(999u, 9009u, fixture.backendState->process->root.executablePath));
        auto request = validRequest();
        request.limits.startupWindowTimeoutMs = 2u;
        const auto output = runLocalCompatibilityCheck(request, fixture.dependencies());
        check(output.diagnostic.result == LocalCompatibilityResult::WindowTimeout &&
                  !output.diagnostic.facts.windowOwnershipVerified &&
                  output.diagnostic.facts.cleanupVerified,
              "same-name foreign process/window is never selected as ownership authority");
    }
    {
        Fixture fixture;
        auto reused = fixture.backendState->process->root;
        ++reused.creationTime100ns;
        fixture.windowState->target = windowIdentity(reused);
        auto request = validRequest();
        request.limits.startupWindowTimeoutMs = 2u;
        const auto output = runLocalCompatibilityCheck(request, fixture.dependencies());
        check(output.diagnostic.result == LocalCompatibilityResult::WindowTimeout &&
                  !output.diagnostic.facts.windowOwnershipVerified &&
                  output.diagnostic.facts.cleanupVerified,
              "same PID with a different creation time fails closed as PID reuse");
    }
}

void testCleanupMustProveAllDescendantsGone() {
    Fixture fixture;
    fixture.backendState->process->childRunning = true;
    fixture.backendState->process->keepChildAfterStop = true;
    const auto output = runLocalCompatibilityCheck(validRequest(), fixture.dependencies());
    check(output.diagnostic.result == LocalCompatibilityResult::CleanupFailed && !output.report &&
              output.diagnostic.facts.cleanupAttempted &&
              !output.diagnostic.facts.cleanupVerified &&
              !output.diagnostic.facts.returnedToWindowsVerified &&
              output.diagnostic.facts.remainingOwnedProcesses == 1u,
          "returned-to-Windows success is impossible while one exact owned descendant remains");
}

void testOwnershipAndObserverFailuresFailClosed() {
    {
        Fixture fixture;
        fixture.backendState->process->capability = process::ChildTrackingCapability::RootOnly;
        const auto output = runLocalCompatibilityCheck(validRequest(), fixture.dependencies());
        check(output.diagnostic.result == LocalCompatibilityResult::OwnershipUnavailable &&
                  !output.report && output.diagnostic.facts.cleanupVerified,
              "weaker-than-Job ownership is rejected and cleaned instead of being accepted");
    }
    {
        Fixture fixture;
        fixture.windowState->startSucceeds = false;
        const auto output = runLocalCompatibilityCheck(validRequest(), fixture.dependencies());
        check(output.diagnostic.result == LocalCompatibilityResult::WindowObserverUnavailable &&
                  fixture.backendState->launchCalls == 0 && !output.report,
              "window observer startup failure occurs before any process launch");
    }
    {
        Fixture fixture;
        fixture.backendState->launchSucceeds = false;
        const auto output = runLocalCompatibilityCheck(validRequest(), fixture.dependencies());
        check(output.diagnostic.result == LocalCompatibilityResult::LaunchFailed &&
                  fixture.windowState->stopped && !output.report,
              "process launch failure leaves no compatibility report and stops the observer");
    }
}

void testRiskShellSeatAndBoundsAreRejectedBeforeLaunch() {
    const auto runInvalid = [](LocalCompatibilityRequest request,
                               LocalCompatibilityResult expected,
                               std::string_view message) {
        Fixture fixture;
        const auto output = runLocalCompatibilityCheck(request, fixture.dependencies());
        check(output.diagnostic.result == expected && fixture.backendState->launchCalls == 0 &&
                  fixture.windowState->startCalls == 0 && !output.report,
              message);
    };

    {
        auto request = validRequest();
        request.targetRisk = LocalCompatibilityTargetRisk::Unknown;
        runInvalid(request, LocalCompatibilityResult::RiskClassificationRequired,
                   "unknown protected-risk classification fails closed before launch");
    }
    {
        auto request = validRequest();
        request.targetRisk = LocalCompatibilityTargetRisk::ProtectedOrExperimental;
        runInvalid(request, LocalCompatibilityResult::ProtectedTargetBlocked,
                   "protected/experimental target has no approval bypass in this runner");
    }
    {
        auto request = validRequest();
        request.launch.executablePath = L"C:\\Windows\\System32\\cmd.exe";
        runInvalid(request, LocalCompatibilityResult::InvalidRequest,
                   "cmd.exe shell indirection is rejected before launch");
    }
    {
        auto request = validRequest();
        request.launch.seatId = 3u;
        runInvalid(request, LocalCompatibilityResult::InvalidRequest,
                   "Seat outside the V1 two-Seat boundary is rejected");
    }
    {
        auto request = validRequest();
        request.launch.containment = process::ProcessContainmentPolicy::RootOnly;
        runInvalid(request, LocalCompatibilityResult::InvalidRequest,
                   "root-only containment cannot satisfy descendant cleanup authority");
    }
    {
        auto request = validRequest();
        request.launch.arguments.assign(kMaximumLocalCheckArguments + 1u, L"x");
        runInvalid(request, LocalCompatibilityResult::InvalidRequest,
                   "argument count hard bound is enforced");
    }
    {
        auto request = validRequest();
        request.launch.environmentOverrides = {
            {L"HYDRA_TEST", std::wstring(kMaximumLocalCheckEnvironmentValueCodeUnits + 1u, L'x')}};
        runInvalid(request, LocalCompatibilityResult::InvalidRequest,
                   "environment value hard bound is enforced");
    }
    {
        auto request = validRequest();
        request.limits.startupWindowTimeoutMs = kMaximumStartupWindowTimeoutMs + 1u;
        runInvalid(request, LocalCompatibilityResult::InvalidRequest,
                   "startup/window timeout hard maximum is enforced");
    }
}

void testResultNamesStayStable() {
    check(localCompatibilityResultName(LocalCompatibilityResult::Success) == "Success" &&
              localCompatibilityResultName(LocalCompatibilityResult::StartupTimeout) ==
                  "StartupTimeout" &&
              localCompatibilityResultName(LocalCompatibilityResult::WindowTimeout) ==
                  "WindowTimeout" &&
              localCompatibilityResultName(LocalCompatibilityResult::CleanupFailed) ==
                  "CleanupFailed",
          "typed local compatibility result names remain stable for integration diagnostics");
}

} // namespace

int main() {
    testControlledOwnedWindowProducesTruthfulInsufficientReport();
    testNoWindowTimesOutAndCleansOwnedTree();
    testEarlyExitAndNaturalExitAreDistinct();
    testExplicitCancellationCleansOnlyLaunchedGroup();
    testLaunchPastStartupBoundFailsAndCleansOwnedGroup();
    testForeignAndPidReuseWindowsNeverBecomeOwned();
    testCleanupMustProveAllDescendantsGone();
    testOwnershipAndObserverFailuresFailClosed();
    testRiskShellSeatAndBoundsAreRejectedBeforeLaunch();
    testResultNamesStayStable();

    if (failures != 0) {
        std::cerr << failures << " local compatibility runner test(s) failed\n";
        return 1;
    }
    std::cout << "local compatibility runner tests passed\n";
    return 0;
}
