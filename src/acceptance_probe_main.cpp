#include "hydra/acceptance_probe.hpp"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

void usage() {
    std::cout
        << "HydraSeat read-only v1 acceptance probe\n\n"
        << "Usage: hydraseat_acceptance_probe --state <install-state.json> "
           "[--samples <1..3600>] [--interval-ms <10..60000>]\n"
        << "Development-only: --allow-unsigned-development --acknowledge-development-evidence\n";
}

template <typename T>
bool parseUnsigned(std::string_view text, T& output) {
    unsigned long long value = 0u;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value > std::numeric_limits<T>::max()) return false;
    output = static_cast<T>(value);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path statePath;
    std::size_t samples = 1u;
    std::uint32_t interval = 1000u;
    bool development = false;
    bool acknowledged = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--state" && index + 1 < argc) {
            statePath = std::filesystem::path(argv[++index]);
        } else if (argument == "--samples" && index + 1 < argc) {
            if (!parseUnsigned(std::string_view(argv[++index]), samples)) {
                usage(); return EXIT_FAILURE;
            }
        } else if (argument == "--interval-ms" && index + 1 < argc) {
            if (!parseUnsigned(std::string_view(argv[++index]), interval)) {
                usage(); return EXIT_FAILURE;
            }
        } else if (argument == "--allow-unsigned-development") {
            development = true;
        } else if (argument == "--acknowledge-development-evidence") {
            acknowledged = true;
        } else if (argument == "--help" || argument == "-h") {
            usage(); return EXIT_SUCCESS;
        } else {
            usage(); return EXIT_FAILURE;
        }
    }
    if (statePath.empty() || development != acknowledged) {
        std::cerr << "A state path is required and development evidence needs both switches.\n";
        return EXIT_FAILURE;
    }

    hydra::acceptance::InstalledReleaseClaim claim;
    std::string stateSha;
    auto diagnostic = hydra::acceptance::loadInstalledReleaseClaim(
        statePath, claim, &stateSha);
    if (!diagnostic.succeeded()) {
        std::cerr << hydra::acceptance::probeCodeName(diagnostic.code)
                  << ": " << diagnostic.message << '\n';
        return EXIT_FAILURE;
    }
    hydra::acceptance::AcceptanceProbeReport report;
    diagnostic = hydra::acceptance::runAcceptanceProbe(
        claim, stateSha, development, samples, interval, report);
    if (!diagnostic.succeeded()) {
        std::cerr << hydra::acceptance::probeCodeName(diagnostic.code)
                  << ": " << diagnostic.message << '\n';
        return EXIT_FAILURE;
    }
    std::cout << hydra::acceptance::encodeAcceptanceProbeJson(report) << '\n';
    return EXIT_SUCCESS;
}
