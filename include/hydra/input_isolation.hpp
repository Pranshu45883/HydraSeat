#pragma once

#include "hydra/directinput_policy.hpp"
#include "hydra/hidhide_probe.hpp"
#include "hydra/workspace_manager.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hydra {

enum class InputIsolationCapability : std::uint64_t {
    None = 0,

    RawInputObservation = 1ull << 0,
    StablePhysicalDeviceIdentity = 1ull << 1,
    SeatOwnershipResolution = 1ull << 2,
    TargetWindowMessageRouting = 1ull << 3,
    InputDiagnostics = 1ull << 4,

    RawInputRegistrationInterposition = 1ull << 5,
    RawInputDataVirtualization = 1ull << 6,
    WindowMessageFiltering = 1ull << 7,
    KeyboardAsyncStateVirtualization = 1ull << 8,
    KeyboardStateArrayVirtualization = 1ull << 9,
    MouseButtonStateVirtualization = 1ull << 10,
    CursorPositionVirtualization = 1ull << 11,
    CursorClipVirtualization = 1ull << 12,
    CursorVisibilityVirtualization = 1ull << 13,
    CaptureVirtualization = 1ull << 14,

    ForegroundQueryVirtualization = 1ull << 15,
    FocusMutationVirtualization = 1ull << 16,
    FocusMessageSynthesis = 1ull << 17,
    WindowPlacementControl = 1ull << 18,
    WindowStyleControl = 1ull << 19,

    XInputSlotRemapping = 1ull << 20,
    DirectInputVisibility = 1ull << 21,
    DirectInputOrdering = 1ull << 22,
    RawHidControllerRouting = 1ull << 23,

    PhysicalDeviceCloaking = 1ull << 24,
    PhysicalInputSuppression = 1ull << 25,
    VirtualInputInjection = 1ull << 26,
    NamedObjectIsolation = 1ull << 27,
    ProcessLifecycleTracking = 1ull << 28,
    ChildProcessTracking = 1ull << 29,

    // Compatibility aliases retained for earlier Phase 3 callers.
    PhysicalDeviceSuppression = PhysicalInputSuppression,
    PerProcessKeyboardState = (1ull << 8) | (1ull << 9),
    PerProcessMouseState = (1ull << 10) | (1ull << 11),
    PerSeatCursor = CursorPositionVirtualization,
    ForegroundVirtualization = ForegroundQueryVirtualization
};

constexpr std::uint64_t capabilityBits(InputIsolationCapability value) noexcept {
    return static_cast<std::uint64_t>(value);
}

constexpr InputIsolationCapability operator|(InputIsolationCapability left,
                                             InputIsolationCapability right) noexcept {
    return static_cast<InputIsolationCapability>(capabilityBits(left) | capabilityBits(right));
}

constexpr InputIsolationCapability operator&(InputIsolationCapability left,
                                             InputIsolationCapability right) noexcept {
    return static_cast<InputIsolationCapability>(capabilityBits(left) & capabilityBits(right));
}

constexpr InputIsolationCapability operator^(InputIsolationCapability left,
                                             InputIsolationCapability right) noexcept {
    return static_cast<InputIsolationCapability>(capabilityBits(left) ^ capabilityBits(right));
}

constexpr InputIsolationCapability& operator|=(InputIsolationCapability& left,
                                               InputIsolationCapability right) noexcept {
    left = left | right;
    return left;
}

constexpr InputIsolationCapability& operator&=(InputIsolationCapability& left,
                                               InputIsolationCapability right) noexcept {
    left = left & right;
    return left;
}

constexpr InputIsolationCapability allInputIsolationCapabilities() noexcept {
    return static_cast<InputIsolationCapability>((1ull << 30) - 1ull);
}

constexpr InputIsolationCapability operator~(InputIsolationCapability value) noexcept {
    return static_cast<InputIsolationCapability>(
        capabilityBits(allInputIsolationCapabilities()) & ~capabilityBits(value));
}

constexpr InputIsolationCapability capabilityIntersection(
    InputIsolationCapability left, InputIsolationCapability right) noexcept {
    return left & right;
}

constexpr InputIsolationCapability capabilityDifference(
    InputIsolationCapability left, InputIsolationCapability right) noexcept {
    return left & ~right;
}

constexpr bool hasCapability(InputIsolationCapability available,
                             InputIsolationCapability required) noexcept {
    return (capabilityBits(available) & capabilityBits(required)) == capabilityBits(required);
}

constexpr bool hasAnyCapability(InputIsolationCapability available,
                                InputIsolationCapability requested) noexcept {
    return capabilityBits(available & requested) != 0;
}

constexpr bool hasNoCapabilities(InputIsolationCapability value) noexcept {
    return capabilityBits(value) == 0;
}

std::vector<InputIsolationCapability> enumerateCapabilities(
    InputIsolationCapability capabilities);
std::string_view capabilityName(InputIsolationCapability capability) noexcept;

enum class BackendAvailability {
    Unavailable,
    Available
};

enum class BackendRisk {
    Low,
    Medium,
    High
};

enum class InjectionPolicy {
    Forbidden,
    UserApproved,
    Required
};

enum class DriverPolicy {
    Forbidden,
    InstalledOnly,
    UserApprovedInstallation
};

enum class AntiCheatPolicy {
    DenyInvasiveBackends,
    ObservationOnly,
    ExplicitExperimentalOverride
};

enum class RecoveryPolicy {
    Required,
    Recommended,
    NotApplicable
};

enum class PlanStatus {
    Supported,
    SupportedWithWarnings,
    ObservationOnly,
    Unsupported
};

enum class IsolationDiagnosticSeverity {
    Information,
    Warning,
    Error
};

enum class IsolationDiagnosticCode {
    BackendUnavailable,
    CapabilityMissing,
    InjectionForbidden,
    InjectionConsentRequired,
    AntiCheatConflict,
    DriverForbidden,
    DriverConsentRequired,
    AdministratorRequired,
    PersistentStateForbidden,
    GlobalSuppressionForbidden,
    RecoveryGuardMissing,
    ElevatedRiskBackend,
    ExperimentalOverride,
    HandshakeFailed,
    RouteRejected,
    PhysicalCloakFailed,
    CrossSeatBleedDetected,
    RollbackIncomplete
};

struct IsolationDiagnostic {
    IsolationDiagnosticCode code{IsolationDiagnosticCode::CapabilityMissing};
    IsolationDiagnosticSeverity severity{IsolationDiagnosticSeverity::Error};
    std::chrono::system_clock::time_point timestamp{};
    std::optional<SeatId> seatId;
    std::optional<std::uint32_t> processId;
    std::optional<std::uint32_t> systemError;
    std::string profileId;
    std::string backendId;
    InputIsolationCapability capability{InputIsolationCapability::None};
    std::string message;
    std::string remediation;
};

struct BackendDescriptor {
    std::string id;
    std::wstring displayName;
    InputIsolationCapability capabilities{InputIsolationCapability::None};
    BackendAvailability availability{BackendAvailability::Unavailable};
    BackendRisk risk{BackendRisk::High};

    bool requiresProcessInjection{false};
    bool requiresAdministrator{false};
    bool usesKernelDriver{false};
    bool modifiesPersistentSystemState{false};
    bool antiCheatSensitive{false};
    bool reversible{false};
    bool sessionScoped{false};
    bool requiresRecoveryGuard{false};

    int priority{0};
    std::wstring unavailableReason;
};

struct BackendEnvironment {
    bool processInjectionApproved{false};
    bool driverInstallationApproved{false};
    bool administratorAvailable{false};
    bool persistentSystemStateChangesAllowed{false};
    bool recoveryGuardReady{false};

    bool protoInputAvailable{false};
    HidHideAvailability hidHideAvailability{
        HidHideAvailability::Unavailable};
    bool directInputAdapterAvailable{false};
    bool controlledXInputAdapterAvailable{false};
    bool controlledExternalShimAvailable{false};
};

struct GameCompatibilityProfile {
    std::string id;
    std::wstring name;
    InputIsolationCapability requiredCapabilities{InputIsolationCapability::None};
    InputIsolationCapability optionalCapabilities{InputIsolationCapability::None};

    InjectionPolicy injectionPolicy{InjectionPolicy::Forbidden};
    DriverPolicy driverPolicy{DriverPolicy::Forbidden};
    AntiCheatPolicy antiCheatPolicy{AntiCheatPolicy::DenyInvasiveBackends};
    RecoveryPolicy recoveryPolicy{RecoveryPolicy::NotApplicable};

    bool antiCheatDetected{false};
    bool allowGlobalInputSuppression{false};
    bool requireZeroBleed{false};
    std::vector<std::string> preferredBackends;

    // DirectInput instance GUIDs are machine-specific stable identifiers returned
    // by DirectInput 8 enumeration. Their profile order defines the controlled
    // process-local enumeration order; unlisted IDs are not visible.
    std::vector<directinput::DirectInputInstanceId>
        directInputOrderedInstanceIds;
};

struct PlannedBackendStep {
    std::string backendId;
    std::wstring displayName;
    InputIsolationCapability assignedCapabilities{InputIsolationCapability::None};
    BackendRisk risk{BackendRisk::High};
    bool reversible{false};
    bool requiresProcessInjection{false};
    bool requiresAdministrator{false};
    bool usesKernelDriver{false};
};

struct IsolationPlan {
    PlanStatus status{PlanStatus::Unsupported};
    std::string profileId;
    std::vector<PlannedBackendStep> selectedBackends;
    InputIsolationCapability coveredCapabilities{InputIsolationCapability::None};
    InputIsolationCapability missingCapabilities{InputIsolationCapability::None};
    std::vector<IsolationDiagnostic> rejections;
    std::vector<IsolationDiagnostic> warnings;
};

class IsolationPlanner {
public:
    explicit IsolationPlanner(std::vector<BackendDescriptor> backends);

    IsolationPlan plan(const GameCompatibilityProfile& profile,
                       const BackendEnvironment& environment = {}) const;

    const std::vector<BackendDescriptor>& backends() const noexcept {
        return m_backends;
    }

private:
    std::vector<BackendDescriptor> m_backends;
};

BackendDescriptor rawInputHostBackend();
BackendDescriptor protoInputBackend(bool available = false);
BackendDescriptor hidHideSessionBackend(
    HidHideAvailability availability = HidHideAvailability::Unavailable);
BackendDescriptor directInputAdapterBackend(bool available = false);
BackendDescriptor controlledXInputAdapterBackend(bool available = false);
BackendDescriptor controlledExternalShimBackend(bool available = false);
std::vector<BackendDescriptor> builtInIsolationBackends(
    const BackendEnvironment& environment = {});

std::vector<GameCompatibilityProfile> compatibilityProfileTemplates();
std::vector<std::string_view> isolationProfileTemplateNames();
std::optional<GameCompatibilityProfile> compatibilityProfileTemplate(
    std::string_view name);

std::string_view planStatusName(PlanStatus status) noexcept;
std::string_view diagnosticCodeName(IsolationDiagnosticCode code) noexcept;
std::string_view diagnosticSeverityName(IsolationDiagnosticSeverity severity) noexcept;
std::string_view backendRiskName(BackendRisk risk) noexcept;

struct InputRouteDecision {
    std::optional<SeatId> seatId;
    std::uint64_t targetHwnd{0};
    bool consumePhysicalInput{false};

    bool operator==(const InputRouteDecision&) const = default;
};

class SeatRoutingPolicy {
public:
    bool bindDevice(std::wstring deviceId, SeatId seatId);
    bool unbindDevice(std::wstring_view deviceId);
    bool bindTargetWindow(SeatId seatId, std::uint64_t targetHwnd);
    void unbindTargetWindow(SeatId seatId) noexcept;
    void clearSeat(SeatId seatId);
    void clearDeviceBindings() noexcept { m_deviceOwners.clear(); }
    void clear() noexcept {
        m_deviceOwners.clear();
        m_targetWindows.clear();
    }

    std::optional<SeatId> ownerOf(std::wstring_view deviceId) const;
    std::uint64_t targetWindow(SeatId seatId) const noexcept;
    InputRouteDecision route(std::wstring_view deviceId,
                             const WorkspaceManager& seats,
                             bool isolationRequested) const;

private:
    static std::wstring normalize(std::wstring_view value);
    std::unordered_map<std::wstring, SeatId> m_deviceOwners;
    std::unordered_map<SeatId, std::uint64_t> m_targetWindows;
};

class InputIsolationBackend {
public:
    virtual ~InputIsolationBackend() = default;

    virtual std::wstring_view name() const noexcept = 0;
    virtual InputIsolationCapability capabilities() const noexcept = 0;
    virtual bool start() = 0;
    virtual void stop() noexcept = 0;

    // Runtime activation remains a later Phase 3 step. Implementations must
    // return false when they cannot enforce the route safely.
    virtual bool applyRoute(std::wstring_view physicalDeviceId,
                            const InputRouteDecision& decision) = 0;
};

class UnsupportedIsolationBackend final : public InputIsolationBackend {
public:
    std::wstring_view name() const noexcept override { return L"unsupported"; }
    InputIsolationCapability capabilities() const noexcept override {
        return InputIsolationCapability::None;
    }
    bool start() override { return true; }
    void stop() noexcept override {}
    bool applyRoute(std::wstring_view, const InputRouteDecision&) override {
        return false;
    }
};

} // namespace hydra
