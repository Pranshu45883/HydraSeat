#include "hydra/input_isolation.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using hydra::InputIsolationCapability;

void printUsage(std::ostream& out) {
    out << "Usage:\n"
        << "  hydra_plan --list\n"
        << "  hydra_plan <profile-template> [options]\n\n"
        << "Options only describe an environment; this tool never activates a backend.\n"
        << "  --protoinput          Mark a configured external ProtoInput adapter available\n"
        << "  --hidhide-unverified  Record HidHide evidence without a verified contract\n"
        << "  --hidhide-verified    Record a verified-supported read-only probe result\n"
        << "  --directinput         Mark the controlled DirectInput visibility/order adapter available\n"
        << "  --allow-injection     Record approval; does not override a profile prohibition\n"
        << "  --admin               Record administrator access as available\n"
        << "  --allow-driver-install Record approval; does not override a profile prohibition\n"
        << "  --allow-persistent    Permit persistent system-state changes\n"
        << "  --recovery-ready      Record watchdog/rollback/emergency input as ready\n"
        << "  --anti-cheat          Mark the target as protected; invasive backends are denied\n"
        << "  --allow-suppression   Permit physical cloaking/suppression in this profile\n"
        << "  --help                Show this help\n";
}

void printCapabilitySet(std::string_view title,
                        InputIsolationCapability capabilities) {
    std::cout << title << ":\n";
    const auto entries = hydra::enumerateCapabilities(capabilities);
    if (entries.empty()) {
        std::cout << "  (none)\n";
        return;
    }
    for (const auto capability : entries) {
        std::cout << "  - " << hydra::capabilityName(capability) << '\n';
    }
}

void printDiagnostics(std::string_view title,
                      const std::vector<hydra::IsolationDiagnostic>& diagnostics) {
    std::cout << title << ":\n";
    if (diagnostics.empty()) {
        std::cout << "  (none)\n";
        return;
    }
    for (const auto& diagnostic : diagnostics) {
        std::cout << "  - [" << hydra::diagnosticSeverityName(diagnostic.severity)
                  << "/" << hydra::diagnosticCodeName(diagnostic.code) << "]";
        if (!diagnostic.backendId.empty()) {
            std::cout << " " << diagnostic.backendId;
        }
        std::cout << ": " << diagnostic.message << '\n';
        if (!diagnostic.remediation.empty()) {
            std::cout << "      Remediation: " << diagnostic.remediation << '\n';
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(std::cerr);
        return 2;
    }

    const std::string first = argv[1];
    if (first == "--help" || first == "-h") {
        printUsage(std::cout);
        return 0;
    }
    if (first == "--list") {
        std::cout << "Profile templates:\n";
        for (const auto name : hydra::isolationProfileTemplateNames()) {
            std::cout << "  - " << name << '\n';
        }
        return 0;
    }

    auto profile = hydra::compatibilityProfileTemplate(first);
    if (!profile) {
        std::cerr << "Unknown profile template: " << first << "\n\n";
        printUsage(std::cerr);
        return 2;
    }

    hydra::BackendEnvironment environment;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--protoinput") {
            environment.protoInputAvailable = true;
        } else if (option == "--hidhide-unverified") {
            environment.hidHideAvailability =
                hydra::HidHideAvailability::InstalledUnverified;
        } else if (option == "--hidhide-verified") {
            environment.hidHideAvailability =
                hydra::HidHideAvailability::VerifiedSupported;
        } else if (option == "--directinput") {
            environment.directInputAdapterAvailable = true;
        } else if (option == "--allow-injection") {
            environment.processInjectionApproved = true;
        } else if (option == "--admin") {
            environment.administratorAvailable = true;
        } else if (option == "--allow-driver-install") {
            environment.driverInstallationApproved = true;
        } else if (option == "--allow-persistent") {
            environment.persistentSystemStateChangesAllowed = true;
        } else if (option == "--recovery-ready") {
            environment.recoveryGuardReady = true;
        } else if (option == "--anti-cheat") {
            profile->antiCheatDetected = true;
            profile->antiCheatPolicy =
                hydra::AntiCheatPolicy::DenyInvasiveBackends;
        } else if (option == "--allow-suppression") {
            profile->allowGlobalInputSuppression = true;
        } else if (option == "--help" || option == "-h") {
            printUsage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown option: " << option << "\n\n";
            printUsage(std::cerr);
            return 2;
        }
    }

    const auto catalog = hydra::builtInIsolationBackends(environment);
    const hydra::IsolationPlanner planner(catalog);
    const auto plan = planner.plan(*profile, environment);

    std::cout << "HydraSeat isolation plan (analysis only; no backend activated)\n"
              << "Profile: " << profile->id << '\n'
              << "Status: " << hydra::planStatusName(plan.status) << "\n\n";

    std::cout << "Selected backends:\n";
    if (plan.selectedBackends.empty()) {
        std::cout << "  (none)\n";
    } else {
        for (const auto& step : plan.selectedBackends) {
            std::cout << "  - " << step.backendId
                      << " [risk=" << hydra::backendRiskName(step.risk)
                      << ", reversible=" << (step.reversible ? "yes" : "no")
                      << "]\n";
            for (const auto capability :
                 hydra::enumerateCapabilities(step.assignedCapabilities)) {
                std::cout << "      * " << hydra::capabilityName(capability)
                          << '\n';
            }
        }
    }
    std::cout << '\n';

    printCapabilitySet("Covered capabilities", plan.coveredCapabilities);
    std::cout << '\n';
    printCapabilitySet("Missing required capabilities", plan.missingCapabilities);
    std::cout << '\n';
    printDiagnostics("Rejected backends / missing requirements", plan.rejections);
    std::cout << '\n';
    printDiagnostics("Warnings", plan.warnings);

    return plan.status == hydra::PlanStatus::Unsupported ? 3 : 0;
}
