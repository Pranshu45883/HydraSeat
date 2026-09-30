#include "hydra/local_compatibility_runner.hpp"

#include "hydra/window_tracker.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <limits>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace hydra::local_compatibility {
namespace {

using Clock = std::chrono::steady_clock;

struct CleanupTrace {
    std::uint64_t startedMicros{0u};
    std::uint64_t completedMicros{0u};
};

std::uint64_t elapsedMicros(Clock::time_point begin, Clock::time_point end) noexcept {
    if (end <= begin) return 0u;
    const auto value = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    if (value <= 0) return 0u;
    return static_cast<std::uint64_t>(value);
}

std::wstring lower(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
}

bool containsNul(std::wstring_view value) noexcept {
    return value.find(L'\0') != std::wstring_view::npos;
}

bool isDriveAbsolute(std::wstring_view value) noexcept {
    return value.size() >= 3u && std::iswalpha(value[0]) != 0 && value[1] == L':' &&
           (value[2] == L'\\' || value[2] == L'/');
}

bool isAbsoluteLocalPath(std::wstring_view value) noexcept {
    if (isDriveAbsolute(value)) return true;
    constexpr std::wstring_view extendedPrefix = L"\\\\?\\";
    if (value.size() > extendedPrefix.size() &&
        value.substr(0u, extendedPrefix.size()) == extendedPrefix) {
        return isDriveAbsolute(value.substr(extendedPrefix.size()));
    }
    return false;
}

std::wstring_view fileName(std::wstring_view path) noexcept {
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring_view::npos ? path : path.substr(separator + 1u);
}

bool nativeExePath(std::wstring_view path) {
    if (path.empty() || path.size() > kMaximumLocalCheckPathCodeUnits || containsNul(path) ||
        !isAbsoluteLocalPath(path)) {
        return false;
    }
    const auto name = lower(fileName(path));
    return name.size() > 4u && name.ends_with(L".exe");
}

bool blockedIndirectionTarget(std::wstring_view executablePath) {
    static constexpr std::array<std::wstring_view, 9> blocked{
        L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"wscript.exe", L"cscript.exe",
        L"mshta.exe", L"rundll32.exe", L"regsvr32.exe", L"msiexec.exe"};
    const auto name = lower(fileName(executablePath));
    return std::find(blocked.begin(), blocked.end(), name) != blocked.end();
}

bool validArchitecture(process::ProcessArchitecture value) noexcept {
    switch (value) {
    case process::ProcessArchitecture::Any:
    case process::ProcessArchitecture::X86:
    case process::ProcessArchitecture::X64:
        return true;
    }
    return false;
}

bool addBounded(std::size_t& total, std::size_t amount, std::size_t maximum) noexcept {
    if (amount > maximum || total > maximum - amount) return false;
    total += amount;
    return true;
}

LocalCompatibilityResult validateRequest(const LocalCompatibilityRequest& request,
                                         std::string& message) {
    if (request.targetRisk == LocalCompatibilityTargetRisk::Unknown) {
        message = "local compatibility check requires an explicit reviewed target-risk classification";
        return LocalCompatibilityResult::RiskClassificationRequired;
    }
    if (request.targetRisk == LocalCompatibilityTargetRisk::ProtectedOrExperimental) {
        message = "protected or experimental targets are not allowed in the local compatibility runner";
        return LocalCompatibilityResult::ProtectedTargetBlocked;
    }
    if (request.planFingerprint == 0u) {
        message = "local compatibility check requires a nonzero plan fingerprint";
        return LocalCompatibilityResult::InvalidRequest;
    }
    if (request.launch.seatId != 1u && request.launch.seatId != 2u) {
        message = "local compatibility check requires Seat 1 or Seat 2";
        return LocalCompatibilityResult::InvalidRequest;
    }
    if (!nativeExePath(request.launch.executablePath)) {
        message = "local compatibility check requires one absolute local .exe target";
        return LocalCompatibilityResult::InvalidRequest;
    }
    if (blockedIndirectionTarget(request.launch.executablePath)) {
        message = "shell, script-host, installer, and general indirection executables are not allowed";
        return LocalCompatibilityResult::InvalidRequest;
    }
    if (request.launch.containment != process::ProcessContainmentPolicy::RequireJobObject) {
        message = "local compatibility check requires full Job Object descendant containment";
        return LocalCompatibilityResult::InvalidRequest;
    }
    if (!validArchitecture(request.launch.architecture)) {
        message = "local compatibility check process architecture is invalid";
        return LocalCompatibilityResult::InvalidRequest;
    }
    if (request.launch.arguments.size() > kMaximumLocalCheckArguments) {
        message = "local compatibility check argument count exceeds the hard bound";
        return LocalCompatibilityResult::InvalidRequest;
    }
    std::size_t totalArguments = request.launch.executablePath.size();
    for (const auto& argument : request.launch.arguments) {
        if (argument.size() > kMaximumLocalCheckArgumentCodeUnits || containsNul(argument) ||
            !addBounded(totalArguments, argument.size() + 1u,
                        kMaximumLocalCheckTotalArgumentCodeUnits)) {
            message = "local compatibility check arguments exceed the hard bound or contain NUL";
            return LocalCompatibilityResult::InvalidRequest;
        }
    }
    if (!request.launch.workingDirectory.empty() &&
        (request.launch.workingDirectory.size() > kMaximumLocalCheckPathCodeUnits ||
         containsNul(request.launch.workingDirectory) ||
         !isAbsoluteLocalPath(request.launch.workingDirectory))) {
        message = "local compatibility check working directory must be a bounded absolute local path";
        return LocalCompatibilityResult::InvalidRequest;
    }
    if (request.launch.environmentOverrides.size() > kMaximumLocalCheckEnvironmentOverrides) {
        message = "local compatibility check environment override count exceeds the hard bound";
        return LocalCompatibilityResult::InvalidRequest;
    }
    std::size_t totalEnvironment = 0u;
    std::set<std::wstring> environmentKeys;
    for (const auto& [key, value] : request.launch.environmentOverrides) {
        if (key.empty() || key.size() > kMaximumLocalCheckEnvironmentKeyCodeUnits ||
            value.size() > kMaximumLocalCheckEnvironmentValueCodeUnits || containsNul(key) ||
            containsNul(value) || key.find(L'=') != std::wstring::npos ||
            !addBounded(totalEnvironment, key.size() + value.size() + 2u,
                        kMaximumLocalCheckTotalEnvironmentCodeUnits) ||
            !environmentKeys.insert(lower(key)).second) {
            message = "local compatibility check environment overrides are invalid, duplicate, or unbounded";
            return LocalCompatibilityResult::InvalidRequest;
        }
    }

    const auto& limits = request.limits;
    if (limits.startupWindowTimeoutMs == 0u ||
        limits.startupWindowTimeoutMs > kMaximumStartupWindowTimeoutMs ||
        limits.observationDurationMs == 0u ||
        limits.observationDurationMs > kMaximumObservationDurationMs ||
        limits.gracefulCleanupTimeoutMs > kMaximumCleanupTimeoutMs ||
        limits.forcedCleanupTimeoutMs == 0u ||
        limits.forcedCleanupTimeoutMs > kMaximumCleanupTimeoutMs ||
        limits.pollIntervalMs == 0u || limits.pollIntervalMs > kMaximumPollIntervalMs ||
        limits.pollIntervalMs > limits.startupWindowTimeoutMs) {
        message = "local compatibility check timeout or polling limits are outside hard bounds";
        return LocalCompatibilityResult::InvalidRequest;
    }
    return LocalCompatibilityResult::Success;
}

class NativeOwnedProcess final : public LocalCompatibilityOwnedProcess {
public:
    explicit NativeOwnedProcess(std::unique_ptr<process::SeatProcessGroup> group)
        : group_(std::move(group)) {}

    SeatId seatId() const noexcept override { return group_->seatId(); }
    process::ChildTrackingCapability capability() const noexcept override {
        return group_->capability();
    }
    process::ProcessIdentity rootIdentity() const override { return group_->rootIdentity(); }
    process::ProcessTreeSnapshot snapshot() const override { return group_->snapshot(); }
    bool ownsExactIdentity(const process::ProcessIdentity& identity) const override {
        return group_->ownsExactIdentity(identity);
    }
    bool waitForEmpty(std::uint32_t timeoutMs) const override {
        return group_->waitForEmpty(timeoutMs);
    }
    bool stop(const process::ProcessStopPolicy& policy,
              std::string* error) noexcept override {
        return group_->stop(policy, error);
    }

private:
    std::unique_ptr<process::SeatProcessGroup> group_;
};

class NativeProcessBackend final : public LocalCompatibilityProcessBackend {
public:
    std::unique_ptr<LocalCompatibilityOwnedProcess> launch(
        const process::ProcessLaunchSpec& spec,
        std::string* error) override {
        auto launched = process::ProcessLauncher::launch(spec, error);
        if (!launched.group || !launched.root.valid() ||
            !launched.group->ownsExactIdentity(launched.root)) {
            if (error && error->empty()) {
                *error = "ProcessLauncher did not return an exact owned root process";
            }
            return nullptr;
        }
        return std::make_unique<NativeOwnedProcess>(std::move(launched.group));
    }
};

class NativeWindowObserver final : public LocalCompatibilityWindowObserver {
public:
    explicit NativeWindowObserver(std::uint32_t reacquisitionTimeoutMs)
        : tracker_(windowing::WindowTrackerOptions{
              256u, 256u, reacquisitionTimeoutMs}) {}

    bool start(std::string* error) override { return tracker_.start(error); }

    void updateProcessTree(const process::ProcessTreeSnapshot& tree) override {
        tracker_.setProcessTrees({tree});
    }

    std::optional<windowing::WindowIdentity> visualTarget(SeatId seatId) const override {
        const auto target = tracker_.target(seatId, windowing::WindowTargetKind::Visual);
        if (!target || target->status != windowing::WindowTargetStatus::Bound ||
            !target->window || !target->window->visible || !target->window->identity.valid()) {
            return std::nullopt;
        }
        return target->window->identity;
    }

    bool validateIdentity(const windowing::WindowIdentity& identity) const noexcept override {
        return tracker_.validateIdentity(identity);
    }

    void stop() noexcept override { tracker_.stop(); }

private:
    windowing::WindowTracker tracker_;
};

class ObserverStopGuard final {
public:
    explicit ObserverStopGuard(LocalCompatibilityWindowObserver* observer) noexcept
        : observer_(observer) {}
    ~ObserverStopGuard() {
        if (observer_ != nullptr) observer_->stop();
    }
    ObserverStopGuard(const ObserverStopGuard&) = delete;
    ObserverStopGuard& operator=(const ObserverStopGuard&) = delete;

private:
    LocalCompatibilityWindowObserver* observer_{nullptr};
};

bool treeContainsLiveIdentity(const process::ProcessTreeSnapshot& tree,
                              const process::ProcessIdentity& identity) {
    if (!identity.valid()) return false;
    for (const auto& record : tree.processes) {
        if (!record.exited && record.identity.sameInstance(identity)) return true;
    }
    return tree.root.sameInstance(identity) && tree.runningCount() != 0u;
}

bool exactOwnershipReady(const LocalCompatibilityOwnedProcess& group,
                         const process::ProcessTreeSnapshot& tree) {
    return group.seatId() != 0u &&
           group.capability() == process::ChildTrackingCapability::FullJobObject &&
           tree.seatId == group.seatId() && tree.root.valid() &&
           group.rootIdentity().sameInstance(tree.root) && tree.trackingComplete &&
           group.ownsExactIdentity(tree.root);
}

bool cleanupOwnedGroup(LocalCompatibilityOwnedProcess& group,
                       const LocalCompatibilityLimits& limits,
                       LocalCompatibilityFacts& facts,
                       CleanupTrace& trace,
                       std::string& error) {
    facts.cleanupAttempted = true;
    const auto begin = Clock::now();
    trace.startedMicros = monotonicInputMetricTimestampMicros();
    if (trace.startedMicros == 0u) trace.startedMicros = 1u;

    process::ProcessStopPolicy policy;
    policy.gracefulTimeoutMs = limits.gracefulCleanupTimeoutMs;
    policy.forcedTimeoutMs = limits.forcedCleanupTimeoutMs;
    policy.forceTerminate = true;
    const bool stopped = group.stop(policy, &error);
    const bool empty = group.waitForEmpty(limits.forcedCleanupTimeoutMs);
    const auto after = group.snapshot();

    trace.completedMicros = monotonicInputMetricTimestampMicros();
    if (trace.completedMicros < trace.startedMicros) {
        trace.completedMicros = trace.startedMicros;
    }
    facts.cleanupDurationMicros = elapsedMicros(begin, Clock::now());
    facts.remainingOwnedProcesses = after.runningCount();
    facts.cleanupVerified = stopped && empty && after.trackingComplete &&
                            facts.remainingOwnedProcesses == 0u;
    facts.returnedToWindowsVerified = facts.cleanupVerified;
    if (!facts.cleanupVerified && error.empty()) {
        error = "exact owned process group could not be verified empty after cleanup";
    }
    return facts.cleanupVerified;
}

metrics::SessionMetricsBuildInput makeMetricsInput(
    const LocalCompatibilityRequest& request,
    const LocalCompatibilityFacts& facts,
    const CleanupTrace& cleanup) {
    metrics::SessionMetricsBuildInput input;
    input.planFingerprint = request.planFingerprint;
    input.origin = metrics::EvidenceOrigin::ControlledProcess;

    // buildSessionMetricsReport intentionally rejects an empty trace. The local
    // runner did not observe input routing, so it records only the cleanup rollback
    // interval and no input event. This keeps uniqueInputEvents at zero and the
    // isolation verdict at InsufficientEvidence instead of fabricating a pass.
    InputMetricSample rollbackStart;
    rollbackStart.correlationId = 1u;
    rollbackStart.timestampMicros = cleanup.startedMicros;
    rollbackStart.stage = InputMetricStage::RollbackStarted;
    rollbackStart.eventClass = InputMetricEventClass::None;
    InputMetricSample rollbackComplete = rollbackStart;
    rollbackComplete.timestampMicros = cleanup.completedMicros;
    rollbackComplete.stage = InputMetricStage::RollbackCompleted;
    input.input.capacity = 2u;
    input.input.acceptedSamples = 2u;
    input.input.samples = {rollbackStart, rollbackComplete};

    input.seats.reserve(metrics::kMaximumSessionMetricSeats);
    for (SeatId seatId : {SeatId{1u}, SeatId{2u}}) {
        metrics::SeatSessionMetrics seat;
        seat.seatId = seatId;
        seat.controller = metrics::CapabilityOutcome::MissingEvidence;
        seat.audio = metrics::CapabilityOutcome::MissingEvidence;
        if (seatId == request.launch.seatId) {
            seat.launchDurationMicros = facts.launchDurationMicros;
            seat.stopDurationMicros = facts.cleanupDurationMicros;
            seat.rollbackDurationMicros = facts.cleanupDurationMicros;
            seat.processStarted = facts.processStarted;
            seat.windowOwnershipVerified = facts.windowOwnershipVerified;
            // This runner does not activate or measure these authorities.
            seat.displayPlacementVerified = false;
            seat.inputRouteReady = false;
        }
        input.seats.push_back(seat);
    }
    input.finalState = metrics::SessionFinalState::ReturnedToWindows;
    input.rollbackAttempted = facts.cleanupAttempted;
    input.rollbackVerified = facts.cleanupVerified;
    return input;
}

LocalCompatibilityRunOutput failure(LocalCompatibilityRunOutput output,
                                    LocalCompatibilityResult result,
                                    std::string message) {
    output.diagnostic.result = result;
    output.diagnostic.message = std::move(message);
    output.report.reset();
    return output;
}

} // namespace

LocalCompatibilityRunOutput runLocalCompatibilityCheck(
    const LocalCompatibilityRequest& request,
    LocalCompatibilityDependencies dependencies,
    std::stop_token cancellation) {
    LocalCompatibilityRunOutput output;
    std::string validationMessage;
    const auto validation = validateRequest(request, validationMessage);
    if (validation != LocalCompatibilityResult::Success) {
        return failure(std::move(output), validation, std::move(validationMessage));
    }
    if (dependencies.processBackend == nullptr || dependencies.windowObserver == nullptr) {
        return failure(std::move(output), LocalCompatibilityResult::InvalidRequest,
                       "local compatibility check dependencies are incomplete");
    }
    if (cancellation.stop_requested()) {
        return failure(std::move(output), LocalCompatibilityResult::Cancelled,
                       "local compatibility check was cancelled before launch");
    }

    std::string observerError;
    if (!dependencies.windowObserver->start(&observerError)) {
        return failure(std::move(output), LocalCompatibilityResult::WindowObserverUnavailable,
                       observerError.empty() ? "authoritative window observer could not start"
                                             : std::move(observerError));
    }
    ObserverStopGuard observerGuard(dependencies.windowObserver);

    std::string launchError;
    const auto launchBegin = Clock::now();
    auto group = dependencies.processBackend->launch(request.launch, &launchError);
    output.diagnostic.facts.launchDurationMicros = elapsedMicros(launchBegin, Clock::now());
    if (!group) {
        return failure(std::move(output), LocalCompatibilityResult::LaunchFailed,
                       launchError.empty() ? "exact local process launch failed"
                                           : std::move(launchError));
    }

    auto& facts = output.diagnostic.facts;
    facts.processStarted = true;
    facts.rootProcess = group->rootIdentity();
    CleanupTrace cleanupTrace;
    const auto cleanupFailure = [&](LocalCompatibilityResult original,
                                    std::string message) -> LocalCompatibilityRunOutput {
        std::string cleanupError;
        if (!cleanupOwnedGroup(*group, request.limits, facts, cleanupTrace, cleanupError)) {
            if (!message.empty()) message += "; ";
            message += cleanupError.empty() ? "exact owned process cleanup failed" : cleanupError;
            return failure(std::move(output), LocalCompatibilityResult::CleanupFailed,
                           std::move(message));
        }
        return failure(std::move(output), original, std::move(message));
    };

    const auto startupBudgetMicros =
        static_cast<std::uint64_t>(request.limits.startupWindowTimeoutMs) * 1000u;
    if (facts.launchDurationMicros > startupBudgetMicros) {
        return cleanupFailure(LocalCompatibilityResult::StartupTimeout,
                              "exact process launch exceeded the bounded startup timeout");
    }

    auto tree = group->snapshot();
    if (!facts.rootProcess.valid() || group->seatId() != request.launch.seatId ||
        !group->ownsExactIdentity(facts.rootProcess) || !exactOwnershipReady(*group, tree)) {
        return cleanupFailure(LocalCompatibilityResult::OwnershipUnavailable,
                              "launched process group did not provide complete exact Job ownership");
    }

    if (cancellation.stop_requested()) {
        return cleanupFailure(LocalCompatibilityResult::Cancelled,
                              "local compatibility check was cancelled after launch");
    }

    const auto windowBegin = Clock::now();
    const auto windowDeadline =
        launchBegin + std::chrono::milliseconds(request.limits.startupWindowTimeoutMs);
    while (true) {
        if (cancellation.stop_requested()) {
            facts.windowObservationDurationMicros = elapsedMicros(windowBegin, Clock::now());
            return cleanupFailure(LocalCompatibilityResult::Cancelled,
                                  "local compatibility check was cancelled while waiting for a window");
        }

        tree = group->snapshot();
        if (!exactOwnershipReady(*group, tree)) {
            facts.windowObservationDurationMicros = elapsedMicros(windowBegin, Clock::now());
            return cleanupFailure(LocalCompatibilityResult::OwnershipUnavailable,
                                  "exact process-tree ownership became unverifiable");
        }
        dependencies.windowObserver->updateProcessTree(tree);

        if (tree.runningCount() == 0u) {
            facts.naturalExitObserved = true;
            facts.windowObservationDurationMicros = elapsedMicros(windowBegin, Clock::now());
            return cleanupFailure(LocalCompatibilityResult::EarlyProcessExit,
                                  "owned process tree exited before an authoritative window appeared");
        }

        const auto target = dependencies.windowObserver->visualTarget(request.launch.seatId);
        if (target && target->valid() &&
            dependencies.windowObserver->validateIdentity(*target) &&
            group->ownsExactIdentity(target->process) &&
            treeContainsLiveIdentity(tree, target->process)) {
            facts.windowOwnershipVerified = true;
            facts.windowObservationDurationMicros = elapsedMicros(windowBegin, Clock::now());
            break;
        }

        if (Clock::now() >= windowDeadline) {
            facts.windowObservationDurationMicros = elapsedMicros(windowBegin, Clock::now());
            return cleanupFailure(LocalCompatibilityResult::WindowTimeout,
                                  "no authoritative owned top-level window appeared before timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(request.limits.pollIntervalMs));
    }

    const auto observationDeadline =
        Clock::now() + std::chrono::milliseconds(request.limits.observationDurationMs);
    while (Clock::now() < observationDeadline) {
        if (cancellation.stop_requested()) {
            return cleanupFailure(LocalCompatibilityResult::Cancelled,
                                  "local compatibility check was cancelled during observation");
        }
        tree = group->snapshot();
        if (!exactOwnershipReady(*group, tree)) {
            return cleanupFailure(LocalCompatibilityResult::OwnershipUnavailable,
                                  "exact process-tree ownership became unverifiable during observation");
        }
        dependencies.windowObserver->updateProcessTree(tree);
        if (tree.runningCount() == 0u) {
            facts.naturalExitObserved = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(request.limits.pollIntervalMs));
    }

    std::string cleanupError;
    if (!cleanupOwnedGroup(*group, request.limits, facts, cleanupTrace, cleanupError)) {
        return failure(std::move(output), LocalCompatibilityResult::CleanupFailed,
                       cleanupError.empty() ? "exact owned process cleanup failed"
                                            : std::move(cleanupError));
    }

    metrics::SessionMetricsReport report;
    const auto metricsResult = metrics::buildSessionMetricsReport(
        makeMetricsInput(request, facts, cleanupTrace), report);
    if (metricsResult != metrics::SessionMetricsResult::Success ||
        report.origin != metrics::EvidenceOrigin::ControlledProcess ||
        report.physicalValidationEligible ||
        report.sessionVerdict == metrics::EvidenceVerdict::Pass ||
        report.input.uniqueInputEvents != 0u) {
        return failure(std::move(output), LocalCompatibilityResult::MetricsBuildFailed,
                       metricsResult == metrics::SessionMetricsResult::Success
                           ? "local compatibility metrics unexpectedly promoted unmeasured evidence"
                           : std::string("session metrics build failed: ") +
                                 std::string(metrics::sessionMetricsResultName(metricsResult)));
    }

    output.report = std::move(report);
    output.diagnostic.result = LocalCompatibilityResult::Success;
    output.diagnostic.message = facts.naturalExitObserved
        ? "controlled local compatibility observation completed after natural process exit"
        : "controlled local compatibility observation completed and exact owned processes were stopped";
    return output;
}

LocalCompatibilityRunOutput runLocalCompatibilityCheck(
    const LocalCompatibilityRequest& request,
    std::stop_token cancellation) {
    NativeProcessBackend processBackend;
    NativeWindowObserver windowObserver(request.limits.startupWindowTimeoutMs);
    return runLocalCompatibilityCheck(
        request, LocalCompatibilityDependencies{&processBackend, &windowObserver}, cancellation);
}

std::string_view localCompatibilityResultName(LocalCompatibilityResult result) noexcept {
    switch (result) {
    case LocalCompatibilityResult::Success: return "Success";
    case LocalCompatibilityResult::InvalidRequest: return "InvalidRequest";
    case LocalCompatibilityResult::RiskClassificationRequired: return "RiskClassificationRequired";
    case LocalCompatibilityResult::ProtectedTargetBlocked: return "ProtectedTargetBlocked";
    case LocalCompatibilityResult::WindowObserverUnavailable: return "WindowObserverUnavailable";
    case LocalCompatibilityResult::LaunchFailed: return "LaunchFailed";
    case LocalCompatibilityResult::StartupTimeout: return "StartupTimeout";
    case LocalCompatibilityResult::OwnershipUnavailable: return "OwnershipUnavailable";
    case LocalCompatibilityResult::EarlyProcessExit: return "EarlyProcessExit";
    case LocalCompatibilityResult::WindowTimeout: return "WindowTimeout";
    case LocalCompatibilityResult::Cancelled: return "Cancelled";
    case LocalCompatibilityResult::CleanupFailed: return "CleanupFailed";
    case LocalCompatibilityResult::MetricsBuildFailed: return "MetricsBuildFailed";
    }
    return "Unknown";
}

} // namespace hydra::local_compatibility
