#include "hydra/display_topology.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

std::wstring widen(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

const wchar_t* likelihoodName(hydra::display::VirtualDisplayLikelihood value) {
    using hydra::display::VirtualDisplayLikelihood;
    switch (value) {
        case VirtualDisplayLikelihood::PhysicalLikely: return L"physical-likely";
        case VirtualDisplayLikelihood::VirtualLikely: return L"virtual-likely";
        case VirtualDisplayLikelihood::Unknown: return L"unknown";
    }
    return L"unknown";
}

} // namespace

int main() {
    hydra::display::DisplayTopologyInventory inventory;
    const auto snapshot = inventory.refresh();
    std::wcout << L"HydraSeat display topology generation " << snapshot.generation
               << L" attempts=" << snapshot.queryAttempts
               << L" success=" << (snapshot.querySucceeded ? L"yes" : L"no") << L'\n';

    for (const auto& output : snapshot.outputs) {
        std::wcout << L"- " << widen(output.identity.stableKey())
                   << L" gdi=" << output.gdiDeviceName
                   << L" name=\"" << output.friendlyName << L"\""
                   << L" bounds=[" << output.desktopBounds.left << L',' << output.desktopBounds.top
                   << L".." << output.desktopBounds.right << L',' << output.desktopBounds.bottom << L']'
                   << L" dpi=" << output.dpiX << L'x' << output.dpiY
                   << L" scale=" << output.effectiveScalePercent << L'%'
                   << L" active=" << (output.active ? L"yes" : L"no")
                   << L" attached=" << (output.attached ? L"yes" : L"no")
                   << L" primary=" << (output.primary ? L"yes" : L"no")
                   << L" dxgi=" << (output.dxgiMatched ? L"yes" : L"no")
                   << L" class=" << likelihoodName(output.virtualLikelihood)
                   << L'\n';
        for (const auto& diagnostic : output.diagnostics) {
            std::wcout << L"    diagnostic: " << widen(diagnostic) << L'\n';
        }
    }
    for (const auto& diagnostic : snapshot.diagnostics) {
        std::wcout << L"diagnostic: " << widen(diagnostic) << L'\n';
    }
    return snapshot.querySucceeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
