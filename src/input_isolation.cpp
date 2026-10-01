#include "hydra/input_isolation.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cwctype>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace hydra {
namespace {

using Capability = InputIsolationCapability;

constexpr std::array<Capability, 30> kCapabilities{
    Capability::RawInputObservation,
    Capability::StablePhysicalDeviceIdentity,
    Capability::SeatOwnershipResolution,
    Capability::TargetWindowMessageRouting,
    Capability::InputDiagnostics,
    Capability::RawInputRegistrationInterposition,
    Capability::RawInputDataVirtualization,
    Capability::WindowMessageFiltering,
    Capability::KeyboardAsyncStateVirtualization,
    Capability::KeyboardStateArrayVirtualization,
    Capability::MouseButtonStateVirtualization,
    Capability::CursorPositionVirtualization,
    Capability::CursorClipVirtualization,
    Capability::CursorVisibilityVirtualization,
    Capability::CaptureVirtualization,
    Capability::ForegroundQueryVirtualization,
    Capability::FocusMutationVirtualization,
    Capability::FocusMessageSynthesis,
    Capability::WindowPlacementControl,
    Capability::WindowStyleControl,
    Capability::XInputSlotRemapping,
    Capability::DirectInputVisibility,
    Capability::DirectInputOrdering,
    Capability::RawHidControllerRouting,
    Capability::PhysicalDeviceCloaking,
    Capability::PhysicalInputSuppression,
    Capability::VirtualInputInjection,
    Capability::NamedObjectIsolation,
    Capability::ProcessLifecycleTracking,
    Capability::ChildProcessTracking,
};

constexpr Capability kObservationCapabilities =
    Capability::RawInputObservation |
    Capability::StablePhysicalDeviceIdentity |
    Capability::SeatOwnershipResolution |
    Capability::InputDiagnostics;

constexpr Capability kPhysicalControlCapabilities =
    Capability::PhysicalDeviceCloaking |
    Capability::PhysicalInputSuppression;

unsigned capabilityCount(Capability value) noexcept {
    return static_cast<unsigned>(std::popcount(capabilityBits(value)));
}

std::size_t preferenceIndex(const GameCompatibilityProfile& profile,
                            std::string_view backendId) {
    const auto it = std::find(profile.preferredBackends.begin(),
                              profile.preferredBackends.end(), backendId);
    if (it == profile.preferredBackends.end()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(
        std::distance(profile.preferredBackends.begin(), it));
}

IsolationDiagnostic makeDiagnostic(
    const GameCompatibilityProfile& profile,
    IsolationDiagnosticCode code,
    IsolationDiagnosticSeverity severity,
    std::string backendId,
    Capability capability,
    std::string message,
    std::string remediation) {
    IsolationDiagnostic result;
    result.code = code;
    result.severity = severity;
    result.profileId = profile.id;
    result.backendId = std::move(backendId);
    result.capability = capability;
    result.message = std::move(message);
    result.remediation = std::move(remediation);
    return result;
}

std::optional<IsolationDiagnostic> rejectBackend(
    const GameCompatibilityProfile& profile,
    const BackendDescriptor& backend,
    const BackendEnvironment& environment) {
    if (backend.availability != BackendAvailability::Available) {
        return makeDiagnostic(
            profile,
            IsolationDiagnosticCode::BackendUnavailable,
            IsolationDiagnosticSeverity::Error,
            backend.id,
            backend.capabilities,
            "Backend is not available in the current environment.",
            "Install or configure the optional component separately, then probe availability again.");
    }

    if (backend.requiresProcessInjection) {
        if (profile.injectionPolicy == InjectionPolicy::Forbidden) {
            return makeDiagnostic(
                profile,
                IsolationDiagnosticCode::InjectionForbidden,
                IsolationDiagnosticSeverity::Error,
                backend.id,
                backend.capabilities,
                "Profile policy forbids process injection.",
                "Use a non-injecting backend or a profile that explicitly permits injection.");
        }
        if (!environment.processInjectionApproved) {
            return makeDiagnostic(
                profile,
                IsolationDiagnosticCode::InjectionConsentRequired,
                IsolationDiagnosticSeverity::Error,
                backend.id,
                backend.capabilities,
                "Process injection was not explicitly approved for this planning session.",
                "Review the target and backend, then provide explicit approval.");
        }
    }

    if (backend.usesKernelDriver && profile.driverPolicy == DriverPolicy::Forbidden) {
        return makeDiagnostic(
            profile,
            IsolationDiagnosticCode::DriverForbidden,
            IsolationDiagnosticSeverity::Error,
            backend.id,
            backend.capabilities,
            "Profile policy forbids driver-backed operation.",
            "Use a user-mode backend or select a reviewed installed-driver policy.");
    }

    if (backend.usesKernelDriver &&
        profile.driverPolicy == DriverPolicy::UserApprovedInstallation &&
        !environment.driverInstallationApproved) {
        return makeDiagnostic(
            profile,
            IsolationDiagnosticCode::DriverConsentRequired,
            IsolationDiagnosticSeverity::Error,
            backend.id,
            backend.capabilities,
            "Driver-backed operation was not explicitly approved for this planning session.",
            "Review the installed driver and recovery path, then provide explicit approval.");
    }

    if (backend.requiresAdministrator && !environment.administratorAvailable) {
        return makeDiagnostic(
            profile,
            IsolationDiagnosticCode::AdministratorRequired,
            IsolationDiagnosticSeverity::Error,
            backend.id,
            backend.capabilities,
            "Backend requires administrator access that is unavailable.",
            "Provide the required access only after reviewing recovery and deployment risks.");
    }

    const bool invasive = backend.antiCheatSensitive ||
                          backend.requiresProcessInjection ||
                          backend.usesKernelDriver;
    if (profile.antiCheatDetected &&
        profile.antiCheatPolicy != AntiCheatPolicy::ExplicitExperimentalOverride &&
        invasive) {
        return makeDiagnostic(
            profile,
            IsolationDiagnosticCode::AntiCheatConflict,
            IsolationDiagnosticSeverity::Error,
            backend.id,
            backend.capabilities,
            "Protected-process policy rejects this invasive backend.",
            "Use observation-only diagnostics; HydraSeat does not bypass anti-cheat or process protection.");
    }

    if (profile.antiCheatPolicy == AntiCheatPolicy::ObservationOnly &&
        !hasNoCapabilities(capabilityDifference(backend.capabilities,
                                                kObservationCapabilities))) {
        return makeDiagnostic(
            profile,
            IsolationDiagnosticCode::AntiCheatConflict,
            IsolationDiagnosticSeverity::Error,
            backend.id,
            backend.capabilities,
            "Observation-only policy rejects behavior-changing capabilities.",
            "Use only the host observation backend.");
    }

    if (backend.modifiesPersistentSystemState &&
        !environment.persistentSystemStateChangesAllowed) {
        return makeDiagnostic(
            profile,
            IsolationDiagnosticCode::PersistentStateForbidden,
            IsolationDiagnosticSeverity::Error,
            backend.id,
            backend.capabilities,
            "Persistent system mutation was not approved.",
            "Use a session-scoped backend or explicitly approve the persistent change.");
    }

    if ((backend.requiresRecoveryGuard ||
         hasAnyCapability(backend.capabilities, kPhysicalControlCapabilities)) &&
        !environment.recoveryGuardReady) {
        return makeDiagnostic(
            profile,
            IsolationDiagnosticCode::RecoveryGuardMissing,
            IsolationDiagnosticSeverity::Error,
            backend.id,
            backend.capabilities & kPhysicalControlCapabilities,
            "Physical cloaking or suppression requires a ready recovery guard.",
            "Configure the watchdog, rollback path and independent emergency input first.");
    }

    if (hasAnyCapability(backend.capabilities, kPhysicalControlCapabilities) &&
        !profile.allowGlobalInputSuppression) {
        return makeDiagnostic(
            profile,
            IsolationDiagnosticCode::GlobalSuppressionForbidden,
            IsolationDiagnosticSeverity::Error,
            backend.id,
            backend.capabilities & kPhysicalControlCapabilities,
            "Profile does not permit physical input cloaking or suppression.",
            "Use suppression only in a reviewed zero-bleed profile with recovery enabled.");
    }

    return std::nullopt;
}

BackendDescriptor makeDescriptor(
    std::string id,
    std::wstring displayName,
    Capability capabilities,
    BackendAvailability availability,
    BackendRisk risk,
    int priority) {
    BackendDescriptor result;
    result.id = std::move(id);
    result.displayName = std::move(displayName);
    result.capabilities = capabilities;
    result.availability = availability;
    result.risk = risk;
    result.priority = priority;
    return result;
}

bool isObservationOnlyRequirement(Capability required) noexcept {
    return hasNoCapabilities(capabilityDifference(required,
                                                  kObservationCapabilities));
}

} // namespace

std::vector<Capability> enumerateCapabilities(Capability capabilities) {
    std::vector<Capability> result;
    for (const auto capability : kCapabilities) {
        if (hasCapability(capabilities, capability)) {
            result.push_back(capability);
        }
    }
    return result;
}

std::string_view capabilityName(Capability capability) noexcept {
    switch (capability) {
    case Capability::None: return "None";
    case Capability::RawInputObservation: return "Raw Input observation";
    case Capability::StablePhysicalDeviceIdentity: return "Stable physical device identity";
    case Capability::SeatOwnershipResolution: return "Seat ownership resolution";
    case Capability::TargetWindowMessageRouting: return "Target-window message routing";
    case Capability::InputDiagnostics: return "Input diagnostics";
    case Capability::RawInputRegistrationInterposition: return "Raw Input registration interposition";
    case Capability::RawInputDataVirtualization: return "Raw Input data virtualization";
    case Capability::WindowMessageFiltering: return "Window-message filtering";
    case Capability::KeyboardAsyncStateVirtualization: return "Keyboard async-state virtualization";
    case Capability::KeyboardStateArrayVirtualization: return "Keyboard state-array virtualization";
    case Capability::MouseButtonStateVirtualization: return "Mouse button-state virtualization";
    case Capability::CursorPositionVirtualization: return "Cursor position virtualization";
    case Capability::CursorClipVirtualization: return "Cursor clip virtualization";
    case Capability::CursorVisibilityVirtualization: return "Cursor visibility virtualization";
    case Capability::CaptureVirtualization: return "Capture virtualization";
    case Capability::ForegroundQueryVirtualization: return "Foreground-query virtualization";
    case Capability::FocusMutationVirtualization: return "Focus-mutation virtualization";
    case Capability::FocusMessageSynthesis: return "Focus-message synthesis";
    case Capability::WindowPlacementControl: return "Window placement control";
    case Capability::WindowStyleControl: return "Window style control";
    case Capability::XInputSlotRemapping: return "XInput slot remapping";
    case Capability::DirectInputVisibility: return "DirectInput visibility";
    case Capability::DirectInputOrdering: return "DirectInput ordering";
    case Capability::RawHidControllerRouting: return "Raw HID controller routing";
    case Capability::PhysicalDeviceCloaking: return "Physical device cloaking";
    case Capability::PhysicalInputSuppression: return "Physical input suppression";
    case Capability::VirtualInputInjection: return "Virtual input injection";
    case Capability::NamedObjectIsolation: return "Named-object isolation";
    case Capability::ProcessLifecycleTracking: return "Process lifecycle tracking";
    case Capability::ChildProcessTracking: return "Child-process tracking";
    default: return "Unknown capability set";
    }
}

IsolationPlanner::IsolationPlanner(std::vector<BackendDescriptor> backends)
    : m_backends(std::move(backends)) {
    std::sort(m_backends.begin(), m_backends.end(),
              [](const BackendDescriptor& left,
                 const BackendDescriptor& right) {
                  return left.id < right.id;
              });
    const auto duplicate = std::adjacent_find(
        m_backends.begin(), m_backends.end(),
        [](const BackendDescriptor& left,
           const BackendDescriptor& right) {
            return left.id == right.id;
        });
    if (duplicate != m_backends.end()) {
        throw std::invalid_argument(
            "duplicate isolation backend id: " + duplicate->id);
    }
}

IsolationPlan IsolationPlanner::plan(
    const GameCompatibilityProfile& profile,
    const BackendEnvironment& environment) const {
    IsolationPlan result;
    result.profileId = profile.id;

    Capability effectiveRequired = profile.requiredCapabilities;
    if (profile.requireZeroBleed) {
        effectiveRequired |= Capability::PhysicalInputSuppression;
    }
    const Capability requested = effectiveRequired |
                                 profile.optionalCapabilities;

    std::vector<const BackendDescriptor*> candidates;
    candidates.reserve(m_backends.size());

    for (const auto& backend : m_backends) {
        if (!hasAnyCapability(backend.capabilities, requested)) {
            continue;
        }
        if (const auto rejection = rejectBackend(profile, backend, environment)) {
            result.rejections.push_back(*rejection);
        } else {
            candidates.push_back(&backend);
        }
    }

    std::sort(result.rejections.begin(), result.rejections.end(),
              [](const IsolationDiagnostic& left,
                 const IsolationDiagnostic& right) {
                  return std::tie(left.backendId, left.code,
                                  left.capability, left.message) <
                         std::tie(right.backendId, right.code,
                                  right.capability, right.message);
              });

    std::set<std::string> selectedIds;
    bool selectedInjectionBackend = false;
    bool policyFailure = false;

    const auto selectFor = [&](Capability wanted, bool optional) {
        while (hasAnyCapability(
            capabilityDifference(wanted, result.coveredCapabilities),
            allInputIsolationCapabilities())) {
            const Capability uncovered =
                capabilityDifference(wanted, result.coveredCapabilities);
            const BackendDescriptor* best = nullptr;

            for (const auto* candidate : candidates) {
                if (selectedIds.contains(candidate->id)) {
                    continue;
                }
                const Capability contribution =
                    candidate->capabilities & uncovered;
                if (hasNoCapabilities(contribution)) {
                    continue;
                }
                if (optional && candidate->risk != BackendRisk::Low) {
                    continue;
                }

                if (best == nullptr) {
                    best = candidate;
                    continue;
                }

                const auto rank = [&](const BackendDescriptor& backend) {
                    const std::size_t preferred =
                        preferenceIndex(profile, backend.id);
                    return std::tuple{
                        preferred == std::numeric_limits<std::size_t>::max(),
                        preferred,
                        -static_cast<int>(capabilityCount(
                            backend.capabilities & uncovered)),
                        static_cast<int>(backend.risk),
                        !backend.reversible,
                        -backend.priority,
                        backend.id};
                };

                if (rank(*candidate) < rank(*best)) {
                    best = candidate;
                }
            }

            if (best == nullptr) {
                break;
            }

            const Capability assigned =
                best->capabilities &
                capabilityDifference(requested, result.coveredCapabilities);
            PlannedBackendStep step;
            step.backendId = best->id;
            step.displayName = best->displayName;
            step.assignedCapabilities = assigned;
            step.risk = best->risk;
            step.reversible = best->reversible;
            step.requiresProcessInjection = best->requiresProcessInjection;
            step.requiresAdministrator = best->requiresAdministrator;
            step.usesKernelDriver = best->usesKernelDriver;
            result.selectedBackends.push_back(std::move(step));

            selectedIds.insert(best->id);
            selectedInjectionBackend = selectedInjectionBackend ||
                                       best->requiresProcessInjection;
            result.coveredCapabilities |=
                best->capabilities & requested;
        }
    };

    selectFor(effectiveRequired, false);
    selectFor(profile.optionalCapabilities, true);

    result.missingCapabilities = capabilityDifference(
        effectiveRequired, result.coveredCapabilities);

    if (profile.injectionPolicy == InjectionPolicy::Required &&
        !selectedInjectionBackend) {
        result.rejections.push_back(makeDiagnostic(
            profile,
            IsolationDiagnosticCode::InjectionConsentRequired,
            IsolationDiagnosticSeverity::Error,
            {},
            Capability::None,
            "Profile requires a process-injection backend, but none was selected.",
            "Make an approved compatible backend available or change the profile policy."));
        policyFailure = true;
    }

    if (profile.recoveryPolicy == RecoveryPolicy::Required &&
        !environment.recoveryGuardReady &&
        !isObservationOnlyRequirement(effectiveRequired)) {
        result.rejections.push_back(makeDiagnostic(
            profile,
            IsolationDiagnosticCode::RecoveryGuardMissing,
            IsolationDiagnosticSeverity::Error,
            {},
            kPhysicalControlCapabilities,
            "Profile requires a recovery guard before behavior-changing isolation can be activated.",
            "Configure the watchdog, rollback path and independent emergency input first."));
        policyFailure = true;
    } else if (profile.recoveryPolicy == RecoveryPolicy::Recommended &&
               !environment.recoveryGuardReady &&
               !isObservationOnlyRequirement(effectiveRequired)) {
        result.warnings.push_back(makeDiagnostic(
            profile,
            IsolationDiagnosticCode::RecoveryGuardMissing,
            IsolationDiagnosticSeverity::Warning,
            {},
            Capability::None,
            "A recovery guard is recommended for this behavior-changing profile.",
            "Configure a watchdog and rollback path before production use."));
    }

    for (const auto capability :
         enumerateCapabilities(result.missingCapabilities)) {
        result.rejections.push_back(makeDiagnostic(
            profile,
            IsolationDiagnosticCode::CapabilityMissing,
            IsolationDiagnosticSeverity::Error,
            {},
            capability,
            std::string("Required capability is missing: ") +
                std::string(capabilityName(capability)),
            "Make a suitable backend available without weakening the profile's safety policy."));
    }

    for (const auto& step : result.selectedBackends) {
        if (step.risk != BackendRisk::Low ||
            step.requiresProcessInjection ||
            step.usesKernelDriver ||
            step.requiresAdministrator) {
            result.warnings.push_back(makeDiagnostic(
                profile,
                IsolationDiagnosticCode::ElevatedRiskBackend,
                IsolationDiagnosticSeverity::Warning,
                step.backendId,
                step.assignedCapabilities,
                "Selected backend requires elevated-risk review or explicit consent.",
                "Review version, architecture, recovery and target-process policy before activation."));
        }
    }

    if (profile.antiCheatDetected &&
        profile.antiCheatPolicy ==
            AntiCheatPolicy::ExplicitExperimentalOverride) {
        result.warnings.push_back(makeDiagnostic(
            profile,
            IsolationDiagnosticCode::ExperimentalOverride,
            IsolationDiagnosticSeverity::Warning,
            {},
            Capability::None,
            "An explicit experimental anti-cheat override is active.",
            "HydraSeat does not claim anti-cheat compatibility or provide bypass support."));
    }

    std::sort(result.warnings.begin(), result.warnings.end(),
              [](const IsolationDiagnostic& left,
                 const IsolationDiagnostic& right) {
                  return std::tie(left.backendId, left.code,
                                  left.capability, left.message) <
                         std::tie(right.backendId, right.code,
                                  right.capability, right.message);
              });

    if (policyFailure ||
        hasAnyCapability(result.missingCapabilities,
                         allInputIsolationCapabilities())) {
        result.status = PlanStatus::Unsupported;
    } else if (profile.antiCheatPolicy ==
                   AntiCheatPolicy::ObservationOnly ||
               isObservationOnlyRequirement(effectiveRequired)) {
        result.status = PlanStatus::ObservationOnly;
    } else if (!result.warnings.empty()) {
        result.status = PlanStatus::SupportedWithWarnings;
    } else {
        result.status = PlanStatus::Supported;
    }

    return result;
}

BackendDescriptor rawInputHostBackend() {
    auto result = makeDescriptor(
        "hydra.raw-input-host",
        L"HydraSeat Raw Input host",
        kObservationCapabilities,
        BackendAvailability::Available,
        BackendRisk::Low,
        100);
    result.reversible = true;
    result.sessionScoped = true;
    return result;
}

BackendDescriptor protoInputBackend(bool available) {
    const Capability capabilities =
        Capability::RawInputRegistrationInterposition |
        Capability::RawInputDataVirtualization |
        Capability::WindowMessageFiltering |
        Capability::KeyboardAsyncStateVirtualization |
        Capability::KeyboardStateArrayVirtualization |
        Capability::MouseButtonStateVirtualization |
        Capability::CursorPositionVirtualization |
        Capability::CursorClipVirtualization |
        Capability::CursorVisibilityVirtualization |
        Capability::CaptureVirtualization |
        Capability::ForegroundQueryVirtualization |
        Capability::FocusMutationVirtualization |
        Capability::FocusMessageSynthesis |
        Capability::WindowPlacementControl |
        Capability::WindowStyleControl |
        Capability::XInputSlotRemapping |
        Capability::VirtualInputInjection |
        Capability::NamedObjectIsolation;

    auto result = makeDescriptor(
        "external.protoinput",
        L"External ProtoInput adapter",
        capabilities,
        available ? BackendAvailability::Available
                  : BackendAvailability::Unavailable,
        BackendRisk::High,
        80);
    result.requiresProcessInjection = true;
    result.antiCheatSensitive = true;
    result.reversible = true;
    result.sessionScoped = true;
    if (!available) {
        result.unavailableReason =
            L"Version-pinned external binaries and expected hashes are not configured.";
    }
    return result;
}

BackendDescriptor hidHideSessionBackend(HidHideAvailability availability) {
    const bool available =
        availability == HidHideAvailability::VerifiedSupported;
    auto result = makeDescriptor(
        "external.hidhide-session",
        L"External HidHide session control",
        Capability::PhysicalDeviceCloaking,
        available ? BackendAvailability::Available
                  : BackendAvailability::Unavailable,
        BackendRisk::High,
        70);
    result.requiresAdministrator = true;
    result.usesKernelDriver = true;
    result.antiCheatSensitive = true;
    result.reversible = true;
    result.sessionScoped = true;
    result.requiresRecoveryGuard = true;
    if (availability == HidHideAvailability::InstalledUnverified) {
        result.unavailableReason =
            L"HidHide evidence was detected, but the exact installed control "
             "interface/version contract was not verified.";
    } else if (!available) {
        result.unavailableReason =
            L"A verified HidHide installation and control interface were not detected.";
    }
    return result;
}

BackendDescriptor directInputAdapterBackend(bool available) {
    auto result = makeDescriptor(
        "hydra.directinput-adapter",
        L"HydraSeat DirectInput adapter",
        Capability::DirectInputVisibility |
            Capability::DirectInputOrdering,
        available ? BackendAvailability::Available
                  : BackendAvailability::Unavailable,
        BackendRisk::Medium,
        50);
    result.requiresProcessInjection = true;
    result.antiCheatSensitive = true;
    result.reversible = true;
    result.sessionScoped = true;
    if (!available) {
        result.unavailableReason =
            L"The controlled DirectInput visibility/order adapter is not enabled "
             "for this target; code availability alone is not production support.";
    }
    return result;
}

BackendDescriptor controlledXInputAdapterBackend(bool available) {
    auto result = makeDescriptor(
        "hydra.controlled-xinput-adapter",
        L"HydraSeat controlled XInput adapter",
        Capability::XInputSlotRemapping,
        available ? BackendAvailability::Available
                  : BackendAvailability::Unavailable,
        BackendRisk::Low,
        90);
    result.reversible = true;
    result.sessionScoped = true;
    if (!available) {
        result.unavailableReason =
            L"The controlled adapter ABI v4 is not active for this target; "
             "controller detection alone is not XInput isolation.";
    }
    return result;
}

BackendDescriptor controlledExternalShimBackend(bool available) {
    const Capability capabilities =
        Capability::RawInputRegistrationInterposition |
        Capability::RawInputDataVirtualization |
        Capability::KeyboardAsyncStateVirtualization |
        Capability::CursorPositionVirtualization |
        Capability::CursorClipVirtualization |
        Capability::CaptureVirtualization |
        Capability::ForegroundQueryVirtualization |
        Capability::FocusMessageSynthesis |
        Capability::ProcessLifecycleTracking;
    auto result = makeDescriptor(
        "hydra.controlled-external-shim",
        L"HydraSeat controlled external-process shim",
        capabilities,
        available ? BackendAvailability::Available
                  : BackendAvailability::Unavailable,
        BackendRisk::High,
        95);
    result.requiresProcessInjection = true;
    result.antiCheatSensitive = true;
    result.reversible = true;
    result.sessionScoped = true;
    result.requiresRecoveryGuard = true;
    if (!available) {
        result.unavailableReason =
            L"P3-E external bridge/harness evidence is not enabled for this target.";
    }
    return result;
}

std::vector<BackendDescriptor> builtInIsolationBackends(
    const BackendEnvironment& environment) {
    return {
        rawInputHostBackend(),
        protoInputBackend(environment.protoInputAvailable),
        hidHideSessionBackend(environment.hidHideAvailability),
        directInputAdapterBackend(
            environment.directInputAdapterAvailable),
        controlledXInputAdapterBackend(
            environment.controlledXInputAdapterAvailable),
        controlledExternalShimBackend(
            environment.controlledExternalShimAvailable),
    };
}

std::vector<GameCompatibilityProfile> compatibilityProfileTemplates() {
    GameCompatibilityProfile observation;
    observation.id = "observation-harness";
    observation.name = L"Observation harness";
    observation.requiredCapabilities = kObservationCapabilities;
    observation.injectionPolicy = InjectionPolicy::Forbidden;
    observation.driverPolicy = DriverPolicy::Forbidden;
    observation.recoveryPolicy = RecoveryPolicy::NotApplicable;

    GameCompatibilityProfile raw;
    raw.id = "raw-input-game";
    raw.name = L"Raw Input game";
    raw.requiredCapabilities =
        kObservationCapabilities |
        Capability::RawInputRegistrationInterposition |
        Capability::RawInputDataVirtualization |
        Capability::PhysicalDeviceCloaking |
        Capability::PhysicalInputSuppression;
    raw.optionalCapabilities = Capability::WindowMessageFiltering;
    raw.injectionPolicy = InjectionPolicy::UserApproved;
    raw.driverPolicy = DriverPolicy::InstalledOnly;
    raw.recoveryPolicy = RecoveryPolicy::Required;
    raw.allowGlobalInputSuppression = true;
    raw.requireZeroBleed = true;
    raw.preferredBackends = {
        "external.protoinput",
        "external.hidhide-session",
        "hydra.raw-input-host",
    };

    GameCompatibilityProfile polled = raw;
    polled.id = "polled-keyboard-mouse-game";
    polled.name = L"Polled keyboard/mouse game";
    polled.requiredCapabilities |=
        Capability::KeyboardAsyncStateVirtualization |
        Capability::KeyboardStateArrayVirtualization |
        Capability::MouseButtonStateVirtualization |
        Capability::CursorPositionVirtualization |
        Capability::ForegroundQueryVirtualization;

    GameCompatibilityProfile focus = raw;
    focus.id = "focus-cursor-game";
    focus.name = L"Focus/cursor game";
    focus.requiredCapabilities |=
        Capability::CursorPositionVirtualization |
        Capability::CursorClipVirtualization |
        Capability::CursorVisibilityVirtualization |
        Capability::CaptureVirtualization |
        Capability::ForegroundQueryVirtualization |
        Capability::FocusMutationVirtualization;
    focus.optionalCapabilities |= Capability::FocusMessageSynthesis;

    GameCompatibilityProfile xinput = raw;
    xinput.id = "xinput-controller-game";
    xinput.name = L"XInput controller game";
    xinput.requiredCapabilities =
        kObservationCapabilities |
        Capability::XInputSlotRemapping |
        Capability::PhysicalInputSuppression;
    xinput.preferredBackends = {
        "hydra.controlled-xinput-adapter",
        "external.protoinput",
        "hydra.raw-input-host",
    };

    GameCompatibilityProfile direct = raw;
    direct.id = "directinput-controller-game";
    direct.name = L"DirectInput controller game";
    direct.requiredCapabilities =
        kObservationCapabilities |
        Capability::DirectInputVisibility |
        Capability::DirectInputOrdering |
        Capability::PhysicalInputSuppression;
    direct.preferredBackends = {
        "hydra.directinput-adapter",
        "external.hidhide-session",
        "hydra.raw-input-host",
    };

    GameCompatibilityProfile glfwCursor;
    glfwCursor.id = "glfw-3.5.1-cursor-test";
    glfwCursor.name = L"GLFW 3.5.1 Cursor Test";
    glfwCursor.requiredCapabilities =
        Capability::RawInputRegistrationInterposition |
        Capability::RawInputDataVirtualization |
        Capability::KeyboardAsyncStateVirtualization |
        Capability::CursorPositionVirtualization |
        Capability::CursorClipVirtualization |
        Capability::CaptureVirtualization |
        Capability::ForegroundQueryVirtualization |
        Capability::FocusMessageSynthesis |
        Capability::ProcessLifecycleTracking;
    glfwCursor.injectionPolicy = InjectionPolicy::Required;
    glfwCursor.driverPolicy = DriverPolicy::Forbidden;
    glfwCursor.antiCheatPolicy = AntiCheatPolicy::DenyInvasiveBackends;
    glfwCursor.recoveryPolicy = RecoveryPolicy::Required;
    glfwCursor.preferredBackends = {"hydra.controlled-external-shim"};

    GameCompatibilityProfile protectedObservation = observation;
    protectedObservation.id = "protected-game-observation-only";
    protectedObservation.name = L"Protected game observation only";
    protectedObservation.antiCheatDetected = true;
    protectedObservation.antiCheatPolicy =
        AntiCheatPolicy::ObservationOnly;

    return {
        observation,
        raw,
        polled,
        focus,
        xinput,
        direct,
        glfwCursor,
        protectedObservation,
    };
}

std::vector<std::string_view> isolationProfileTemplateNames() {
    return {
        "observation-harness",
        "raw-input-game",
        "polled-keyboard-mouse-game",
        "focus-cursor-game",
        "xinput-controller-game",
        "directinput-controller-game",
        "glfw-3.5.1-cursor-test",
        "protected-game-observation-only",
    };
}

std::optional<GameCompatibilityProfile> compatibilityProfileTemplate(
    std::string_view name) {
    const auto profiles = compatibilityProfileTemplates();
    const auto it = std::find_if(
        profiles.begin(), profiles.end(),
        [&](const GameCompatibilityProfile& profile) {
            return profile.id == name;
        });
    if (it == profiles.end()) {
        return std::nullopt;
    }
    return *it;
}

std::string_view planStatusName(PlanStatus status) noexcept {
    switch (status) {
    case PlanStatus::Supported: return "Supported";
    case PlanStatus::SupportedWithWarnings: return "SupportedWithWarnings";
    case PlanStatus::ObservationOnly: return "ObservationOnly";
    case PlanStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

std::string_view diagnosticCodeName(
    IsolationDiagnosticCode code) noexcept {
    switch (code) {
    case IsolationDiagnosticCode::BackendUnavailable: return "BackendUnavailable";
    case IsolationDiagnosticCode::CapabilityMissing: return "CapabilityMissing";
    case IsolationDiagnosticCode::InjectionForbidden: return "InjectionForbidden";
    case IsolationDiagnosticCode::InjectionConsentRequired: return "InjectionConsentRequired";
    case IsolationDiagnosticCode::AntiCheatConflict: return "AntiCheatConflict";
    case IsolationDiagnosticCode::DriverForbidden: return "DriverForbidden";
    case IsolationDiagnosticCode::DriverConsentRequired: return "DriverConsentRequired";
    case IsolationDiagnosticCode::AdministratorRequired: return "AdministratorRequired";
    case IsolationDiagnosticCode::PersistentStateForbidden: return "PersistentStateForbidden";
    case IsolationDiagnosticCode::GlobalSuppressionForbidden: return "GlobalSuppressionForbidden";
    case IsolationDiagnosticCode::RecoveryGuardMissing: return "RecoveryGuardMissing";
    case IsolationDiagnosticCode::ElevatedRiskBackend: return "ElevatedRiskBackend";
    case IsolationDiagnosticCode::ExperimentalOverride: return "ExperimentalOverride";
    case IsolationDiagnosticCode::HandshakeFailed: return "HandshakeFailed";
    case IsolationDiagnosticCode::RouteRejected: return "RouteRejected";
    case IsolationDiagnosticCode::PhysicalCloakFailed: return "PhysicalCloakFailed";
    case IsolationDiagnosticCode::CrossSeatBleedDetected: return "CrossSeatBleedDetected";
    case IsolationDiagnosticCode::RollbackIncomplete: return "RollbackIncomplete";
    }
    return "UnknownDiagnostic";
}

std::string_view diagnosticSeverityName(
    IsolationDiagnosticSeverity severity) noexcept {
    switch (severity) {
    case IsolationDiagnosticSeverity::Information: return "Information";
    case IsolationDiagnosticSeverity::Warning: return "Warning";
    case IsolationDiagnosticSeverity::Error: return "Error";
    }
    return "Error";
}

std::string_view backendRiskName(BackendRisk risk) noexcept {
    switch (risk) {
    case BackendRisk::Low: return "Low";
    case BackendRisk::Medium: return "Medium";
    case BackendRisk::High: return "High";
    }
    return "High";
}

std::wstring SeatRoutingPolicy::normalize(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(
                           std::towlower(static_cast<wint_t>(character)));
                   });
    return result;
}

bool SeatRoutingPolicy::bindDevice(std::wstring deviceId,
                                   SeatId seatId) {
    if (deviceId.empty() || seatId == 0) {
        return false;
    }
    m_deviceOwners[normalize(deviceId)] = seatId;
    return true;
}

bool SeatRoutingPolicy::unbindDevice(std::wstring_view deviceId) {
    if (deviceId.empty()) {
        return false;
    }
    return m_deviceOwners.erase(normalize(deviceId)) != 0;
}

bool SeatRoutingPolicy::bindTargetWindow(SeatId seatId,
                                         std::uint64_t targetHwnd) {
    if (seatId == 0 || targetHwnd == 0) return false;
    m_targetWindows[seatId] = targetHwnd;
    return true;
}

void SeatRoutingPolicy::unbindTargetWindow(SeatId seatId) noexcept {
    m_targetWindows.erase(seatId);
}

void SeatRoutingPolicy::clearSeat(SeatId seatId) {
    for (auto it = m_deviceOwners.begin();
         it != m_deviceOwners.end();) {
        if (it->second == seatId) {
            it = m_deviceOwners.erase(it);
        } else {
            ++it;
        }
    }
    m_targetWindows.erase(seatId);
}

std::optional<SeatId> SeatRoutingPolicy::ownerOf(
    std::wstring_view deviceId) const {
    if (deviceId.empty()) {
        return std::nullopt;
    }
    const auto it = m_deviceOwners.find(normalize(deviceId));
    if (it == m_deviceOwners.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::uint64_t SeatRoutingPolicy::targetWindow(SeatId seatId) const noexcept {
    const auto it = m_targetWindows.find(seatId);
    return it == m_targetWindows.end() ? 0u : it->second;
}

InputRouteDecision SeatRoutingPolicy::route(
    std::wstring_view deviceId,
    const WorkspaceManager& seats,
    bool isolationRequested) const {
    InputRouteDecision decision;
    const auto owner = ownerOf(deviceId);
    if (!owner) {
        return decision;
    }

    const auto* seat = seats.getWorkspace(*owner);
    if (seat == nullptr || !seat->active) {
        return decision;
    }

    decision.seatId = seat->workspaceId;
    decision.targetHwnd = targetWindow(seat->workspaceId);
    decision.consumePhysicalInput = isolationRequested;
    return decision;
}

} // namespace hydra
