#include "hydra/controller_inventory.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

const wchar_t* yesNo(bool value) noexcept {
    return value ? L"yes" : L"no";
}

const wchar_t* qualityName(hydra::controller::IdentityQuality quality) noexcept {
    switch (quality) {
    case hydra::controller::IdentityQuality::RuntimeOnly:
        return L"runtime-only";
    case hydra::controller::IdentityQuality::Stable:
        return L"stable";
    }
    return L"unknown";
}

const wchar_t* apiName(hydra::controller::ApiSurface api) noexcept {
    switch (api) {
    case hydra::controller::ApiSurface::XInput:
        return L"xinput";
    case hydra::controller::ApiSurface::DirectInput:
        return L"directinput";
    case hydra::controller::ApiSurface::GameInput:
        return L"gameinput";
    }
    return L"unknown";
}

void usage() {
    std::cout
        << "HydraSeat controller inventory diagnostics\n"
        << "  --list   scan current stable physical identities and XInput runtime slots\n"
        << "  --help   show this help\n\n"
        << "XInput slot numbers are runtime hints only. Pairing requires explicit stable\n"
        << "physical identity plus current runtime slot evidence. No vibration or policy\n"
        << "mutation is performed by this tool.\n";
}

int listSources() {
    hydra::controller::ControllerInventory inventory;
    const auto snapshot = inventory.scan();
    if (!snapshot.authoritative) {
        std::cerr << "controller inventory is not authoritative: "
                  << snapshot.error << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "runtime_source_count=" << snapshot.sources.size()
              << " physical_controller_count="
              << snapshot.physicalControllers.size() << '\n';

    for (const auto& physical : snapshot.physicalControllers) {
        std::wcout
            << L"physical"
            << L" persistent_id=" << physical.persistentId
            << L" vendor_id=" << physical.vendorId
            << L" product_id=" << physical.productId
            << L" device_path=" << physical.devicePath
            << L" name=" << physical.displayName << L'\n';
    }

    for (const auto& source : snapshot.sources) {
        const std::wstring persistent =
            source.persistentId ? *source.persistentId : L"-";
        const std::wstring slot =
            source.runtimeXInputSlot
                ? std::to_wstring(*source.runtimeXInputSlot)
                : L"-";
        std::wcout
            << L"runtime"
            << L" api=" << apiName(source.api)
            << L" identity=" << qualityName(source.identityQuality)
            << L" connected=" << yesNo(source.connected)
            << L" source_generation=" << source.sourceGeneration
            << L" xinput_slot=" << slot
            << L" persistent_id=" << persistent
            << L" runtime_key="
            << std::wstring(source.runtimeKey.begin(), source.runtimeKey.end())
            << L" name=" << source.displayName << L'\n';
    }

    return EXIT_SUCCESS;
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

    const std::string_view argument = argv[1];
    if (argument == "--list") return listSources();
    if (argument == "--help" || argument == "-h") {
        usage();
        return EXIT_SUCCESS;
    }

    usage();
    return EXIT_FAILURE;
}
