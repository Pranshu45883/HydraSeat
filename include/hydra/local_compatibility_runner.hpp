#pragma once

#include "hydra/process_launcher.hpp"
#include "hydra/session_metrics.hpp"
#include "hydra/window_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace hydra::local_compatibility {

inline constexpr std::size_t kMaximumLocalCheckArguments = 64u;
inline constexpr std::size_t kMaximumLocalCheckEnvironmentOverrides = 64u;
inline constexpr std::size_t kMaximumLocalCheckArgumentCodeUnits = 4096u;
inline constexpr std::size_t kMaximumLocalCheckEnvironmentKeyCodeUnits = 256u;
inline constexpr std::size_t kMaximumLocalCheckEnvironmentValueCodeUnits = 8192u;
inline constexpr std::size_t kMaximumLocalCheckTotalArgumentCodeUnits = 30000u;
inline constexpr std::size_t kMaximumLocalCheckTotalEnvironmentCodeUnits = 32768u;
inline constexpr std::size_t kMaximumLocalCheckPathCodeUnits = 32767u;

inline constexpr std::uint32_t kMaximumStartupWindowTimeoutMs = 30000u;
inline constexpr std::uint32_t kMaximumObservationDurationMs = 30000u;
inline constexpr std::uint32_t kMaximumCleanupTimeoutMs = 10000u;
inline constexpr std::uint32_t kMaximumPollIntervalMs = 250u;

// This classification must be propagated from an existing reviewed provider/game
// signal. Unknown deliberately fails closed, and this runner has no approval or
// bypass input for protected/experimental targets.
enum class LocalCompatibilityTargetRisk : std::uint8_t {
    Unknown = 0,
    Standard = 1,
    ProtectedOrExperimental = 2,
};

struct LocalCompatibilityLimits {
    std::uint32_t startupWindowTimeoutMs{5000u};
    std::uint32_t observationDurationMs{1000u};
    std::uint32_t gracefulCleanupTimeoutMs{1000u};
    std::uint32_t forcedCleanupTimeoutMs{3000u};
    std::uint32_t pollIntervalMs{10u};
};

struct LocalCompatibilityRequest {
    process::ProcessLaunchSpec launch;
    std::uint64_t planFingerprint{0};
    LocalCompatibilityTargetRisk targetRisk{LocalCompatibilityTargetRisk::Unknown};
    LocalCompatibilityLimits limits;
};

enum class LocalCompatibilityResult : std::uint8_t {
    Success = 0,
    InvalidRequest = 1,
    RiskClassificationRequired = 2,
    ProtectedTargetBlocked = 3,
    WindowObserverUnavailable = 4,
    LaunchFailed = 5,
    StartupTimeout = 6,
    OwnershipUnavailable = 7,
    EarlyProcessExit = 8,
    WindowTimeout = 9,
    Cancelled = 10,
    CleanupFailed = 11,
    MetricsBuildFailed = 12,
};

struct LocalCompatibilityFacts {
    process::ProcessIdentity rootProcess;
    bool processStarted{false};
    bool windowOwnershipVerified{false};
    bool naturalExitObserved{false};
    bool cleanupAttempted{false};
    bool cleanupVerified{false};
    bool returnedToWindowsVerified{false};
    std::size_t remainingOwnedProcesses{0u};
    std::uint64_t launchDurationMicros{0u};
    std::uint64_t windowObservationDurationMicros{0u};
    std::uint64_t cleanupDurationMicros{0u};
};

struct LocalCompatibilityDiagnostic {
    LocalCompatibilityResult result{LocalCompatibilityResult::InvalidRequest};
    std::string message;
    LocalCompatibilityFacts facts;

    bool succeeded() const noexcept {
        return result == LocalCompatibilityResult::Success;
    }
};

struct LocalCompatibilityRunOutput {
    LocalCompatibilityDiagnostic diagnostic;
    // Present only for a semantically complete run: process started under exact
    // ownership, an authoritative owned top-level window was observed, and the
    // exact owned process group was verified empty on return to Windows.
    std::optional<metrics::SessionMetricsReport> report;
};

// Narrow test/integration seam. Production adapters delegate to ProcessLauncher /
// SeatProcessGroup and WindowTracker; this interface does not define any alternate
// ownership mechanism.
class LocalCompatibilityOwnedProcess {
public:
    virtual ~LocalCompatibilityOwnedProcess() = default;
    virtual SeatId seatId() const noexcept = 0;
    virtual process::ChildTrackingCapability capability() const noexcept = 0;
    virtual process::ProcessIdentity rootIdentity() const = 0;
    virtual process::ProcessTreeSnapshot snapshot() const = 0;
    virtual bool ownsExactIdentity(const process::ProcessIdentity& identity) const = 0;
    virtual bool waitForEmpty(std::uint32_t timeoutMs) const = 0;
    virtual bool stop(const process::ProcessStopPolicy& policy,
                      std::string* error = nullptr) noexcept = 0;
};

class LocalCompatibilityProcessBackend {
public:
    virtual ~LocalCompatibilityProcessBackend() = default;
    virtual std::unique_ptr<LocalCompatibilityOwnedProcess> launch(
        const process::ProcessLaunchSpec& spec,
        std::string* error = nullptr) = 0;
};

class LocalCompatibilityWindowObserver {
public:
    virtual ~LocalCompatibilityWindowObserver() = default;
    virtual bool start(std::string* error = nullptr) = 0;
    virtual void updateProcessTree(const process::ProcessTreeSnapshot& tree) = 0;
    virtual std::optional<windowing::WindowIdentity> visualTarget(SeatId seatId) const = 0;
    virtual bool validateIdentity(const windowing::WindowIdentity& identity) const noexcept = 0;
    virtual void stop() noexcept = 0;
};

struct LocalCompatibilityDependencies {
    LocalCompatibilityProcessBackend* processBackend{nullptr};
    LocalCompatibilityWindowObserver* windowObserver{nullptr};
};

LocalCompatibilityRunOutput runLocalCompatibilityCheck(
    const LocalCompatibilityRequest& request,
    LocalCompatibilityDependencies dependencies,
    std::stop_token cancellation = {});

// Production convenience overload. It still uses only the existing exact process
// and window ownership components; no shell, provider parsing, persistence, input
// isolation, controller, audio, display placement, or compatibility mutation occurs.
LocalCompatibilityRunOutput runLocalCompatibilityCheck(
    const LocalCompatibilityRequest& request,
    std::stop_token cancellation = {});

std::string_view localCompatibilityResultName(LocalCompatibilityResult result) noexcept;

} // namespace hydra::local_compatibility
