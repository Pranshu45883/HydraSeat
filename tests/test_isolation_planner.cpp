#include "hydra/input_isolation.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using hydra::BackendAvailability;
using hydra::BackendDescriptor;
using hydra::BackendEnvironment;
using hydra::BackendRisk;
using hydra::GameCompatibilityProfile;
using hydra::InputIsolationCapability;
using hydra::IsolationDiagnosticCode;
using hydra::IsolationPlan;
using hydra::IsolationPlanner;
using hydra::PlanStatus;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool selected(const IsolationPlan& plan, std::string_view backendId) {
    return std::any_of(plan.selectedBackends.begin(), plan.selectedBackends.end(),
                       [&](const auto& step) {
                           return step.backendId == backendId;
                       });
}

bool rejectedWith(const IsolationPlan& plan,
                  std::string_view backendId,
                  IsolationDiagnosticCode code) {
    return std::any_of(plan.rejections.begin(), plan.rejections.end(),
                       [&](const auto& diagnostic) {
                           return diagnostic.backendId == backendId &&
                                  diagnostic.code == code;
                       });
}

std::vector<std::string> selectedIds(const IsolationPlan& plan) {
    std::vector<std::string> result;
    for (const auto& step : plan.selectedBackends) {
        result.push_back(step.backendId);
    }
    return result;
}

BackendDescriptor mockBackend(std::string id,
                              InputIsolationCapability capabilities,
                              BackendRisk risk,
                              int priority = 0) {
    BackendDescriptor result;
    result.id = std::move(id);
    result.displayName = L"Mock backend";
    result.capabilities = capabilities;
    result.availability = BackendAvailability::Available;
    result.risk = risk;
    result.reversible = true;
    result.sessionScoped = true;
    result.priority = priority;
    return result;
}

void testCapabilityHelpers() {
    const auto combined =
        InputIsolationCapability::RawInputObservation |
        InputIsolationCapability::PhysicalInputSuppression;

    check(hydra::hasCapability(combined,
                               InputIsolationCapability::RawInputObservation),
          "combined capability contains Raw Input observation");
    check(hydra::hasCapability(combined,
                               InputIsolationCapability::PhysicalInputSuppression),
          "combined capability contains physical suppression");
    check(!hydra::hasCapability(combined,
                                InputIsolationCapability::CursorClipVirtualization),
          "combined capability does not contain unrelated bits");

    const auto difference = hydra::capabilityDifference(
        combined, InputIsolationCapability::RawInputObservation);
    check(difference == InputIsolationCapability::PhysicalInputSuppression,
          "capability difference removes only requested bits");

    const auto entries = hydra::enumerateCapabilities(combined);
    check(entries.size() == 2, "capability enumeration returns individual bits");
    check(hydra::capabilityName(entries.front()) != "Unknown capability set",
          "individual capabilities have stable names");
    check(hydra::hasNoCapabilities(
              hydra::capabilityDifference(combined, combined)),
          "subtracting a set from itself is empty");
}

void testObservationProfile() {
    BackendEnvironment environment;
    const IsolationPlanner planner(
        hydra::builtInIsolationBackends(environment));
    const auto profile =
        hydra::compatibilityProfileTemplate("observation-harness");
    check(profile.has_value(), "observation profile template exists");

    const auto plan = planner.plan(*profile, environment);
    check(plan.status == PlanStatus::ObservationOnly,
          "observation profile reports observation-only status");
    check(hydra::hasNoCapabilities(plan.missingCapabilities),
          "observation profile has no missing requirements");
    check(selected(plan, "hydra.raw-input-host"),
          "observation profile selects the host backend");
}

void testRawInputProfileFailsClosed() {
    BackendEnvironment environment;
    const IsolationPlanner planner(
        hydra::builtInIsolationBackends(environment));
    const auto profile =
        hydra::compatibilityProfileTemplate("raw-input-game");
    check(profile.has_value(), "Raw Input profile template exists");

    const auto plan = planner.plan(*profile, environment);
    check(plan.status == PlanStatus::Unsupported,
          "Raw Input zero-bleed profile is unsupported with host-only backends");
    check(hydra::hasCapability(
              plan.missingCapabilities,
              InputIsolationCapability::RawInputRegistrationInterposition),
          "missing plan exposes Raw Input registration interposition");
    check(hydra::hasCapability(
              plan.missingCapabilities,
              InputIsolationCapability::RawInputDataVirtualization),
          "missing plan exposes Raw Input data virtualization");
    check(hydra::hasCapability(
              plan.missingCapabilities,
              InputIsolationCapability::PhysicalInputSuppression),
          "missing plan exposes physical input suppression");
    check(rejectedWith(plan, "external.protoinput",
                       IsolationDiagnosticCode::BackendUnavailable),
          "unavailable ProtoInput is reported rather than assumed");
    check(rejectedWith(plan, "external.hidhide-session",
                       IsolationDiagnosticCode::BackendUnavailable),
          "unavailable HidHide is reported rather than assumed");
}

void testControlledXInputCapabilityBoundary() {
    auto profile = hydra::compatibilityProfileTemplate(
        "xinput-controller-game");
    check(profile.has_value(), "XInput controller profile exists");

    BackendEnvironment detectedOnly;
    const IsolationPlanner detectionPlanner(
        hydra::builtInIsolationBackends(detectedOnly));
    const auto detectionPlan = detectionPlanner.plan(
        *profile, detectedOnly);
    check(detectionPlan.status == PlanStatus::Unsupported &&
              hydra::hasCapability(
                  detectionPlan.missingCapabilities,
                  InputIsolationCapability::XInputSlotRemapping) &&
              !selected(detectionPlan,
                        "hydra.controlled-xinput-adapter"),
          "controller detection never advertises controlled XInput remapping");

    BackendEnvironment controlled;
    controlled.controlledXInputAdapterAvailable = true;
    const IsolationPlanner controlledPlanner(
        hydra::builtInIsolationBackends(controlled));
    const auto controlledPlan = controlledPlanner.plan(
        *profile, controlled);
    check(controlledPlan.status == PlanStatus::Unsupported &&
              selected(controlledPlan,
                       "hydra.controlled-xinput-adapter") &&
              hydra::hasCapability(
                  controlledPlan.coveredCapabilities,
                  InputIsolationCapability::XInputSlotRemapping) &&
              hydra::hasCapability(
                  controlledPlan.missingCapabilities,
                  InputIsolationCapability::PhysicalInputSuppression),
          "explicit controlled adapter availability covers only slot remapping and not physical production routing");
}

void testControlledDirectInputCapabilityBoundary() {
    auto profile = hydra::compatibilityProfileTemplate(
        "directinput-controller-game");
    check(profile.has_value(), "DirectInput controller profile exists");
    check(profile->directInputOrderedInstanceIds.empty(),
          "DirectInput template requires machine-specific instance IDs to be configured later");

    hydra::directinput::DirectInputInstanceId selectedInstance;
    selectedInstance.data1 = 1u;
    profile->directInputOrderedInstanceIds.push_back(selectedInstance);

    BackendEnvironment controlled;
    controlled.directInputAdapterAvailable = true;
    controlled.processInjectionApproved = true;
    controlled.recoveryGuardReady = true;
    const IsolationPlanner planner(
        hydra::builtInIsolationBackends(controlled));
    const auto plan = planner.plan(*profile, controlled);

    check(plan.status == PlanStatus::Unsupported &&
              selected(plan, "hydra.directinput-adapter") &&
              hydra::hasCapability(
                  plan.coveredCapabilities,
                  InputIsolationCapability::DirectInputVisibility) &&
              hydra::hasCapability(
                  plan.coveredCapabilities,
                  InputIsolationCapability::DirectInputOrdering) &&
              hydra::hasCapability(
                  plan.missingCapabilities,
                  InputIsolationCapability::PhysicalInputSuppression),
          "controlled DirectInput availability covers only enumeration visibility/order and not physical production routing");
}

void testConfiguredReferencesStillFailWithoutVerifiedSuppression() {
    BackendEnvironment environment;
    environment.protoInputAvailable = true;
    environment.hidHideAvailability =
        hydra::HidHideAvailability::VerifiedSupported;
    environment.processInjectionApproved = true;
    environment.administratorAvailable = true;
    environment.recoveryGuardReady = true;

    const IsolationPlanner planner(
        hydra::builtInIsolationBackends(environment));
    const auto profile = hydra::compatibilityProfileTemplate(
        "polled-keyboard-mouse-game");
    check(profile.has_value(), "polling profile template exists");

    const auto plan = planner.plan(*profile, environment);
    check(plan.status == PlanStatus::Unsupported,
          "configured references remain unsupported without verified physical suppression");
    check(selected(plan, "external.protoinput"),
          "polling profile selects the process compatibility adapter metadata");
    check(selected(plan, "external.hidhide-session"),
          "polling profile selects HidHide only for device cloaking");
    check(selected(plan, "hydra.raw-input-host"),
          "plan retains host observation and identity");
    check(hydra::hasCapability(
              plan.coveredCapabilities,
              InputIsolationCapability::PhysicalDeviceCloaking),
          "HidHide metadata covers physical device cloaking");
    check(hydra::hasCapability(
              plan.missingCapabilities,
              InputIsolationCapability::PhysicalInputSuppression),
          "verified physical input suppression remains explicitly missing");

    const auto protoStep = std::find_if(
        plan.selectedBackends.begin(), plan.selectedBackends.end(),
        [](const auto& step) { return step.backendId == "external.protoinput"; });
    check(protoStep != plan.selectedBackends.end() &&
              hydra::hasCapability(protoStep->assignedCapabilities,
                                   InputIsolationCapability::WindowMessageFiltering),
          "an already-selected backend reports optional capability coverage consistently");
}

void testHidHideAvailabilityStates() {
    GameCompatibilityProfile profile;
    profile.id = "hidhide-availability";
    profile.name = L"HidHide availability test";
    profile.requiredCapabilities =
        InputIsolationCapability::PhysicalDeviceCloaking;
    profile.driverPolicy = hydra::DriverPolicy::InstalledOnly;
    profile.recoveryPolicy = hydra::RecoveryPolicy::Required;
    profile.allowGlobalInputSuppression = true;

    BackendEnvironment environment;
    environment.administratorAvailable = true;
    environment.recoveryGuardReady = true;

    auto plan = IsolationPlanner(
        hydra::builtInIsolationBackends(environment)).plan(
            profile, environment);
    check(plan.status == PlanStatus::Unsupported &&
              rejectedWith(plan, "external.hidhide-session",
                           IsolationDiagnosticCode::BackendUnavailable),
          "unavailable HidHide cannot be planned");

    environment.hidHideAvailability =
        hydra::HidHideAvailability::InstalledUnverified;
    const auto unverifiedCatalog =
        hydra::builtInIsolationBackends(environment);
    const auto unverifiedBackend = std::find_if(
        unverifiedCatalog.begin(), unverifiedCatalog.end(),
        [](const auto& backend) {
            return backend.id == "external.hidhide-session";
        });
    check(unverifiedBackend != unverifiedCatalog.end() &&
              unverifiedBackend->availability ==
                  BackendAvailability::Unavailable &&
              unverifiedBackend->unavailableReason.find(L"not verified") !=
                  std::wstring::npos,
          "installed-unverified HidHide remains unavailable with an explicit reason");
    plan = IsolationPlanner(unverifiedCatalog).plan(profile, environment);
    check(plan.status == PlanStatus::Unsupported &&
              !selected(plan, "external.hidhide-session"),
          "installed-unverified HidHide is never selected");

    environment.hidHideAvailability =
        hydra::HidHideAvailability::VerifiedSupported;
    plan = IsolationPlanner(
        hydra::builtInIsolationBackends(environment)).plan(
            profile, environment);
    check(plan.status == PlanStatus::SupportedWithWarnings &&
              selected(plan, "external.hidhide-session"),
          "verified-supported HidHide is an installed backend environment");
}

void testVerifiedSuppressionBackendCanCompletePlan() {
    BackendEnvironment environment;
    environment.protoInputAvailable = true;
    environment.hidHideAvailability =
        hydra::HidHideAvailability::VerifiedSupported;
    environment.processInjectionApproved = true;
    environment.administratorAvailable = true;
    environment.recoveryGuardReady = true;

    auto backends = hydra::builtInIsolationBackends(environment);
    auto verifiedSuppression = mockBackend(
        "test.verified-physical-suppression",
        InputIsolationCapability::PhysicalInputSuppression,
        BackendRisk::High,
        90);
    verifiedSuppression.requiresRecoveryGuard = true;
    backends.push_back(verifiedSuppression);

    const auto profile = hydra::compatibilityProfileTemplate(
        "polled-keyboard-mouse-game");
    check(profile.has_value(), "polling profile template exists");

    const IsolationPlanner planner(std::move(backends));
    const auto plan = planner.plan(*profile, environment);
    check(plan.status == PlanStatus::SupportedWithWarnings,
          "a verified suppression capability can complete the theoretical plan");
    check(hydra::hasNoCapabilities(plan.missingCapabilities),
          "complete capability coverage has no missing requirements");
    check(selected(plan, "test.verified-physical-suppression"),
          "planner selects the explicit verified-suppression test backend");
}

void testAntiCheatRejectsInvasiveBackends() {
    BackendEnvironment environment;
    environment.protoInputAvailable = true;
    environment.hidHideAvailability =
        hydra::HidHideAvailability::VerifiedSupported;
    environment.processInjectionApproved = true;
    environment.administratorAvailable = true;
    environment.recoveryGuardReady = true;

    auto profile = hydra::compatibilityProfileTemplate(
        "polled-keyboard-mouse-game");
    check(profile.has_value(), "polling profile template exists");
    profile->antiCheatDetected = true;
    profile->antiCheatPolicy =
        hydra::AntiCheatPolicy::DenyInvasiveBackends;

    const IsolationPlanner planner(
        hydra::builtInIsolationBackends(environment));
    const auto plan = planner.plan(*profile, environment);

    check(plan.status == PlanStatus::Unsupported,
          "anti-cheat policy fails closed for invasive requirements");
    check(rejectedWith(plan, "external.protoinput",
                       IsolationDiagnosticCode::AntiCheatConflict),
          "anti-cheat policy rejects process injection");
    check(rejectedWith(plan, "external.hidhide-session",
                       IsolationDiagnosticCode::AntiCheatConflict),
          "anti-cheat policy rejects the driver backend");
}

void testRecoveryGuardIsRequired() {
    BackendEnvironment environment;
    environment.protoInputAvailable = true;
    environment.hidHideAvailability =
        hydra::HidHideAvailability::VerifiedSupported;
    environment.processInjectionApproved = true;
    environment.administratorAvailable = true;
    environment.recoveryGuardReady = false;

    const IsolationPlanner planner(
        hydra::builtInIsolationBackends(environment));
    const auto profile =
        hydra::compatibilityProfileTemplate("raw-input-game");
    check(profile.has_value(), "Raw Input profile template exists");

    const auto plan = planner.plan(*profile, environment);
    check(plan.status == PlanStatus::Unsupported,
          "physical suppression cannot plan without recovery");
    check(rejectedWith(plan, "external.hidhide-session",
                       IsolationDiagnosticCode::RecoveryGuardMissing),
          "HidHide rejection explains missing recovery guard");
    check(hydra::hasCapability(
              plan.missingCapabilities,
              InputIsolationCapability::PhysicalInputSuppression),
          "suppression remains explicitly missing");
}

void testRegistrationOrderDoesNotChangePlan() {
    BackendEnvironment environment;
    environment.protoInputAvailable = true;
    environment.hidHideAvailability =
        hydra::HidHideAvailability::VerifiedSupported;
    environment.processInjectionApproved = true;
    environment.administratorAvailable = true;
    environment.recoveryGuardReady = true;

    auto catalog = hydra::builtInIsolationBackends(environment);
    const IsolationPlanner forward(catalog);
    std::reverse(catalog.begin(), catalog.end());
    const IsolationPlanner reversed(catalog);

    const auto profile = hydra::compatibilityProfileTemplate(
        "focus-cursor-game");
    check(profile.has_value(), "focus/cursor profile template exists");

    const auto firstPlan = forward.plan(*profile, environment);
    const auto secondPlan = reversed.plan(*profile, environment);
    check(firstPlan.status == secondPlan.status,
          "backend registration order does not change status");
    check(selectedIds(firstPlan) == selectedIds(secondPlan),
          "backend registration order does not change selected order");
    check(firstPlan.coveredCapabilities == secondPlan.coveredCapabilities,
          "backend registration order does not change coverage");
}

void testPreferenceAndRiskRanking() {
    auto medium = mockBackend(
        "backend.medium",
        InputIsolationCapability::TargetWindowMessageRouting,
        BackendRisk::Medium,
        100);
    auto low = mockBackend(
        "backend.low",
        InputIsolationCapability::TargetWindowMessageRouting,
        BackendRisk::Low,
        1);

    GameCompatibilityProfile profile;
    profile.id = "ranking";
    profile.name = L"Ranking test";
    profile.requiredCapabilities =
        InputIsolationCapability::TargetWindowMessageRouting;

    const IsolationPlanner planner({medium, low});
    auto plan = planner.plan(profile);
    check(!plan.selectedBackends.empty() &&
              plan.selectedBackends.front().backendId == "backend.low",
          "lower-risk backend wins an unpreferred tie");

    profile.preferredBackends = {"backend.medium"};
    plan = planner.plan(profile);
    check(!plan.selectedBackends.empty() &&
              plan.selectedBackends.front().backendId == "backend.medium",
          "explicit profile preference is deterministic and visible");
    check(plan.status == PlanStatus::SupportedWithWarnings,
          "preferred medium-risk backend produces a warning");
}

void testOptionalCoverageCannotHideMissingRequirements() {
    GameCompatibilityProfile profile;
    profile.id = "optional-does-not-mask-required";
    profile.name = L"Optional coverage test";
    profile.requiredCapabilities =
        InputIsolationCapability::RawInputDataVirtualization;
    profile.optionalCapabilities =
        InputIsolationCapability::TargetWindowMessageRouting;

    const IsolationPlanner planner({mockBackend(
        "backend.optional",
        InputIsolationCapability::TargetWindowMessageRouting,
        BackendRisk::Low)});
    const auto plan = planner.plan(profile);

    check(plan.status == PlanStatus::Unsupported,
          "optional coverage never changes a missing-required plan to supported");
    check(hydra::hasCapability(
              plan.coveredCapabilities,
              InputIsolationCapability::TargetWindowMessageRouting),
          "optional low-risk capability may still be reported as covered");
    check(hydra::hasCapability(
              plan.missingCapabilities,
              InputIsolationCapability::RawInputDataVirtualization),
          "required capability remains missing");
}

void testDuplicateBackendIdsAreRejected() {
    auto first = mockBackend(
        "duplicate.backend",
        InputIsolationCapability::RawInputObservation,
        BackendRisk::Low);
    auto second = first;
    second.requiresProcessInjection = true;

    bool threw = false;
    try {
        const IsolationPlanner planner({first, second});
        (void)planner;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "duplicate backend IDs are rejected instead of order-dependent deduplication");
}

void testInjectionPolicyCannotBeOverriddenByEnvironment() {
    BackendEnvironment environment;
    environment.protoInputAvailable = true;
    environment.processInjectionApproved = true;

    GameCompatibilityProfile profile;
    profile.id = "injection-forbidden";
    profile.name = L"Injection forbidden test";
    profile.requiredCapabilities =
        InputIsolationCapability::RawInputDataVirtualization;
    profile.injectionPolicy = hydra::InjectionPolicy::Forbidden;

    const IsolationPlanner planner(
        hydra::builtInIsolationBackends(environment));
    const auto plan = planner.plan(profile, environment);
    check(plan.status == PlanStatus::Unsupported,
          "environment approval cannot override a profile injection prohibition");
    check(rejectedWith(plan, "external.protoinput",
                       IsolationDiagnosticCode::InjectionForbidden),
          "profile-level injection prohibition is reported explicitly");
}

void testRequiredInjectionPolicyNeedsInjectionBackend() {
    BackendEnvironment environment;
    auto nonInjecting = mockBackend(
        "test.non-injecting-raw-data",
        InputIsolationCapability::RawInputDataVirtualization,
        BackendRisk::Low);

    GameCompatibilityProfile profile;
    profile.id = "injection-required";
    profile.name = L"Injection required test";
    profile.requiredCapabilities =
        InputIsolationCapability::RawInputDataVirtualization;
    profile.injectionPolicy = hydra::InjectionPolicy::Required;

    const IsolationPlanner planner({nonInjecting});
    const auto plan = planner.plan(profile, environment);
    check(plan.status == PlanStatus::Unsupported,
          "required injection policy fails even when a non-injecting mock covers the bit");
    check(rejectedWith(plan, {},
                       IsolationDiagnosticCode::InjectionConsentRequired),
          "missing required injection backend is reported as a policy failure");
}

void testDriverInstallationConsentPolicy() {
    BackendEnvironment environment;
    environment.hidHideAvailability =
        hydra::HidHideAvailability::VerifiedSupported;
    environment.administratorAvailable = true;
    environment.recoveryGuardReady = true;
    environment.driverInstallationApproved = false;

    GameCompatibilityProfile profile;
    profile.id = "driver-consent";
    profile.name = L"Driver consent test";
    profile.requiredCapabilities =
        InputIsolationCapability::PhysicalDeviceCloaking;
    profile.driverPolicy = hydra::DriverPolicy::UserApprovedInstallation;
    profile.allowGlobalInputSuppression = true;
    profile.recoveryPolicy = hydra::RecoveryPolicy::Required;

    const IsolationPlanner planner(
        hydra::builtInIsolationBackends(environment));
    auto plan = planner.plan(profile, environment);
    check(plan.status == PlanStatus::Unsupported,
          "driver-backed plan is unsupported without explicit driver consent");
    check(rejectedWith(plan, "external.hidhide-session",
                       IsolationDiagnosticCode::DriverConsentRequired),
          "missing driver consent is reported explicitly");

    environment.driverInstallationApproved = true;
    plan = planner.plan(profile, environment);
    check(plan.status == PlanStatus::SupportedWithWarnings,
          "driver-backed metadata plan proceeds only after explicit consent");
}

void testGlfwExternalProfileBoundary() {
    const auto profile = hydra::compatibilityProfileTemplate(
        "glfw-3.5.1-cursor-test");
    check(profile.has_value(), "GLFW P3-E profile template exists");
    check(!profile->requireZeroBleed &&
              !hydra::hasCapability(
                  profile->requiredCapabilities,
                  InputIsolationCapability::PhysicalInputSuppression),
          "P3-E profile does not overclaim physical suppression");

    BackendEnvironment environment;
    IsolationPlanner planner(hydra::builtInIsolationBackends(environment));
    auto plan = planner.plan(*profile, environment);
    check(plan.status == PlanStatus::Unsupported &&
              rejectedWith(plan, "hydra.controlled-external-shim",
                           IsolationDiagnosticCode::BackendUnavailable),
          "P3-E profile fails closed when the controlled bridge is unavailable");

    environment.controlledExternalShimAvailable = true;
    planner = IsolationPlanner(hydra::builtInIsolationBackends(environment));
    plan = planner.plan(*profile, environment);
    check(plan.status == PlanStatus::Unsupported &&
              rejectedWith(plan, "hydra.controlled-external-shim",
                           IsolationDiagnosticCode::InjectionConsentRequired),
          "P3-E profile requires explicit injection approval");

    environment.processInjectionApproved = true;
    planner = IsolationPlanner(hydra::builtInIsolationBackends(environment));
    plan = planner.plan(*profile, environment);
    check(plan.status == PlanStatus::Unsupported &&
              rejectedWith(plan, "hydra.controlled-external-shim",
                           IsolationDiagnosticCode::RecoveryGuardMissing),
          "P3-E profile requires the validated recovery guard");

    environment.recoveryGuardReady = true;
    planner = IsolationPlanner(hydra::builtInIsolationBackends(environment));
    plan = planner.plan(*profile, environment);
    check(plan.status == PlanStatus::SupportedWithWarnings &&
              hydra::hasNoCapabilities(plan.missingCapabilities) &&
              selected(plan, "hydra.controlled-external-shim"),
          "reviewed P3-E environment selects only the controlled external shim");
    check(!hydra::hasAnyCapability(
              plan.coveredCapabilities,
              InputIsolationCapability::PhysicalInputSuppression |
                  InputIsolationCapability::PhysicalDeviceCloaking),
          "P3-E plan does not advertise physical hiding or suppression");

    auto protectedProfile = *profile;
    protectedProfile.antiCheatDetected = true;
    plan = planner.plan(protectedProfile, environment);
    check(plan.status == PlanStatus::Unsupported &&
              rejectedWith(plan, "hydra.controlled-external-shim",
                           IsolationDiagnosticCode::AntiCheatConflict),
          "protected target rejects the P3-E invasive bridge");
}

void testProtectedObservationTemplate() {
    BackendEnvironment environment;
    environment.protoInputAvailable = true;
    environment.hidHideAvailability =
        hydra::HidHideAvailability::VerifiedSupported;
    environment.processInjectionApproved = true;
    environment.administratorAvailable = true;
    environment.recoveryGuardReady = true;

    const auto profile = hydra::compatibilityProfileTemplate(
        "protected-game-observation-only");
    check(profile.has_value(), "protected observation template exists");
    const IsolationPlanner planner(
        hydra::builtInIsolationBackends(environment));
    const auto plan = planner.plan(*profile, environment);

    check(plan.status == PlanStatus::ObservationOnly,
          "protected template remains observation-only");
    check(selectedIds(plan) ==
              std::vector<std::string>{"hydra.raw-input-host"},
          "protected template selects no invasive backend");
}

} // namespace

int main() {
    testCapabilityHelpers();
    testObservationProfile();
    testRawInputProfileFailsClosed();
    testControlledXInputCapabilityBoundary();
    testControlledDirectInputCapabilityBoundary();
    testConfiguredReferencesStillFailWithoutVerifiedSuppression();
    testHidHideAvailabilityStates();
    testVerifiedSuppressionBackendCanCompletePlan();
    testAntiCheatRejectsInvasiveBackends();
    testRecoveryGuardIsRequired();
    testRegistrationOrderDoesNotChangePlan();
    testPreferenceAndRiskRanking();
    testOptionalCoverageCannotHideMissingRequirements();
    testDuplicateBackendIdsAreRejected();
    testInjectionPolicyCannotBeOverriddenByEnvironment();
    testRequiredInjectionPolicyNeedsInjectionBackend();
    testDriverInstallationConsentPolicy();
    testGlfwExternalProfileBoundary();
    testProtectedObservationTemplate();

    std::cout << "Isolation planner tests passed.\n";
    return EXIT_SUCCESS;
}
