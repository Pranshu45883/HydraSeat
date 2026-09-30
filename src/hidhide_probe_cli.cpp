#include "hydra/hidhide_probe.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void printUsage(std::ostream& out) {
    out << "Usage: hydra_hidhide_probe [--probe|--self-test|--help]\n"
        << "Read-only observation only; never requests elevation or changes HidHide state.\n";
}

bool selfTest() {
    hydra::HidHideProbeReport report;
    report.availability = hydra::HidHideAvailability::VerifiedSupported;
    report.installedEvidence = true;
    report.controlInterfacePresent = true;
    report.controlInterfaceReadable = true;
    report.driverVersion = hydra::HidHideVersion{1, 7, 346, 0};
    report.active = true;
    report.inverseWhitelist = false;
    report.sessionBlacklistSupported = true;
    report.diagnostic = hydra::HidHideProbeDiagnostic::None;

    const auto text = hydra::formatHidHideProbeReport(report);
    return text.find("state: verified-supported") != std::string::npos &&
           text.find("driver_version: 1.7.346.0") != std::string::npos &&
           text.find("mutation_performed: no") != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        printUsage(std::cerr);
        return 2;
    }
    const std::string_view option = argc == 2 ? argv[1] : "--probe";
    if (option == "--help" || option == "-h") {
        printUsage(std::cout);
        return 0;
    }
    if (option == "--self-test") {
        if (!selfTest()) {
            std::cerr << "HidHide probe CLI self-test failed.\n";
            return EXIT_FAILURE;
        }
        std::cout << "HidHide probe CLI self-test passed.\n";
        return EXIT_SUCCESS;
    }
    if (option != "--probe") {
        printUsage(std::cerr);
        return 2;
    }

    std::cout << hydra::formatHidHideProbeReport(hydra::probeHidHide());
    return EXIT_SUCCESS;
}
