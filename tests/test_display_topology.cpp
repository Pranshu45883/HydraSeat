#include "hydra/display_topology.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace hydra::display;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

AdapterLuid luid(std::uint32_t low, std::int32_t high = 0) {
    return AdapterLuid{low, high};
}

DisplayPathObservation path(AdapterLuid adapter, std::uint32_t target,
                            std::uint32_t source, std::wstring gdi,
                            std::wstring devicePath, DisplayRect bounds,
                            bool active = true, bool attached = true) {
    DisplayPathObservation value;
    value.identity.adapterLuid = adapter;
    value.identity.targetId = target;
    value.identity.monitorDevicePath = std::move(devicePath);
    value.sourceId = source;
    value.gdiDeviceName = std::move(gdi);
    value.friendlyName = L"Fixture Monitor";
    value.edidManufacturerId = 0x1234;
    value.edidProductCodeId = static_cast<std::uint16_t>(target + 1u);
    value.desktopBounds = bounds;
    value.mode.width = static_cast<std::uint32_t>(std::max(0, bounds.width()));
    value.mode.height = static_cast<std::uint32_t>(std::max(0, bounds.height()));
    value.mode.refreshNumerator = 60000;
    value.mode.refreshDenominator = 1000;
    value.mode.orientation = DisplayOrientation::Identity;
    value.active = active;
    value.attached = attached;
    return value;
}

DxgiOutputObservation dxgi(AdapterLuid adapter, std::wstring gdi,
                           DisplayRect bounds, std::wstring description = L"Fixture GPU") {
    DxgiOutputObservation value;
    value.adapterLuid = adapter;
    value.gdiDeviceName = std::move(gdi);
    value.desktopBounds = bounds;
    value.attachedToDesktop = true;
    value.adapterDescription = std::move(description);
    return value;
}

void testStableCorrelationIndependentOfEnumerationOrder() {
    DisplayTopologyObservation first;
    const auto adapter = luid(0x1111);
    first.paths.push_back(path(adapter, 4, 1, L"\\\\.\\DISPLAY2",
                               L"MONITOR#AAA#1", {-1920, 0, 0, 1080}));
    first.paths.push_back(path(adapter, 2, 0, L"\\\\.\\DISPLAY1",
                               L"MONITOR#BBB#2", {0, 0, 2560, 1440}));
    first.dxgiOutputs.push_back(dxgi(adapter, L"\\\\.\\DISPLAY1", {0, 0, 2560, 1440}));
    first.dxgiOutputs.push_back(dxgi(adapter, L"\\\\.\\DISPLAY2", {-1920, 0, 0, 1080}));

    DxgiAdapterObservation adapterObservation;
    adapterObservation.identity.luid = adapter;
    adapterObservation.description = L"Fixture GPU";
    first.adapters.push_back(adapterObservation);

    auto second = first;
    std::reverse(second.paths.begin(), second.paths.end());
    std::reverse(second.dxgiOutputs.begin(), second.dxgiOutputs.end());

    const auto a = assembleDisplayTopology(first, 10);
    const auto b = assembleDisplayTopology(second, 11);
    check(a.outputs.size() == 2 && b.outputs.size() == 2,
          "synthetic two-output topology is assembled");
    if (a.outputs.size() == 2 && b.outputs.size() == 2) {
        check(a.outputs[0].identity.stableKey() == b.outputs[0].identity.stableKey() &&
                  a.outputs[1].identity.stableKey() == b.outputs[1].identity.stableKey(),
              "stable output ordering is independent of enumeration order");
        check(a.outputs[0].dxgiMatched && a.outputs[1].dxgiMatched,
              "DisplayConfig targets correlate to DXGI by adapter LUID plus GDI name");
    }
}

void testDuplicatePathsDisconnectedAndNegativeCoordinates() {
    DisplayTopologyObservation observation;
    const auto adapter = luid(0x2222);
    auto inactive = path(adapter, 7, 3, L"\\\\.\\DISPLAY7", L"MONITOR#DUP#7",
                         {0, 0, 0, 0}, false, false);
    inactive.friendlyName.clear();
    inactive.edidManufacturerId = 0;
    inactive.edidProductCodeId = 0;
    observation.paths.push_back(inactive);

    auto active = path(adapter, 7, 4, L"\\\\.\\DISPLAY7", L"MONITOR#DUP#7",
                       {-1200, -800, 0, 1120}, true, true);
    active.mode.orientation = DisplayOrientation::Rotate90;
    active.dpiX = 144;
    active.dpiY = 144;
    active.primary = true;
    observation.paths.push_back(active);

    auto disconnected = path(adapter, 8, 5, L"", L"MONITOR#OFF#8",
                             {0, 0, 0, 0}, false, false);
    disconnected.friendlyName = L"Disconnected Panel";
    observation.paths.push_back(disconnected);

    observation.dxgiOutputs.push_back(dxgi(adapter, L"\\\\.\\DISPLAY7",
                                           {-1200, -800, 0, 1120}));
    const auto snapshot = assembleDisplayTopology(observation, 20);
    check(snapshot.outputs.size() == 2,
          "duplicate paths for one adapter/target merge while disconnected target is preserved");
    check(snapshot.adapters.size() == 1u && snapshot.adapters.front().identity.luid == adapter,
          "DisplayConfig-only adapter identity is preserved when DXGI metadata is absent");

    const auto merged = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(),
                                     [](const DisplayOutput& output) {
                                         return output.identity.targetId == 7u;
                                     });
    check(merged != snapshot.outputs.end(), "merged active target remains present");
    if (merged != snapshot.outputs.end()) {
        check(merged->active && merged->attached && merged->primary,
              "active/attached/primary flags survive duplicate path merge");
        check(merged->sourceId == 4u,
              "active duplicate path replaces stale inactive source identity");
        check(merged->desktopBounds == DisplayRect{-1200, -800, 0, 1120},
              "negative coordinates are preserved exactly");
        check(merged->mode.orientation == DisplayOrientation::Rotate90,
              "orientation is preserved");
        check(merged->effectiveScalePercent == 150u,
              "effective DPI scale is derived deterministically");
    }

    const auto off = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(),
                                  [](const DisplayOutput& output) {
                                      return output.identity.targetId == 8u;
                                  });
    check(off != snapshot.outputs.end() && !off->active && !off->attached,
          "disconnected/disabled output is retained rather than silently dropped");
}

void testDuplicateNamesDoNotCrossAdapters() {
    DisplayTopologyObservation observation;
    const auto adapterA = luid(0x3333);
    const auto adapterB = luid(0x4444);
    observation.paths.push_back(path(adapterA, 1, 0, L"\\\\.\\DISPLAY1",
                                     L"MONITOR#A#1", {0, 0, 1920, 1080}));
    observation.paths.push_back(path(adapterB, 1, 0, L"\\\\.\\DISPLAY1",
                                     L"MONITOR#B#1", {1920, 0, 3840, 1080}));
    observation.dxgiOutputs.push_back(dxgi(adapterB, L"\\\\.\\DISPLAY1",
                                           {1920, 0, 3840, 1080}, L"GPU B"));
    observation.dxgiOutputs.push_back(dxgi(adapterA, L"\\\\.\\DISPLAY1",
                                           {0, 0, 1920, 1080}, L"GPU A"));
    const auto snapshot = assembleDisplayTopology(observation, 30);
    check(snapshot.outputs.size() == 2, "duplicate GDI names on distinct adapters remain distinct");
    for (const auto& output : snapshot.outputs) {
        check(output.dxgiMatched, "duplicate-name target still gets a DXGI match");
        if (output.identity.adapterLuid == adapterA) {
            check(output.dxgiAdapterDescription == L"GPU A",
                  "adapter A never correlates to duplicate name on adapter B");
        } else if (output.identity.adapterLuid == adapterB) {
            check(output.dxgiAdapterDescription == L"GPU B",
                  "adapter B never correlates to duplicate name on adapter A");
        }
    }
}

void testVirtualClassificationIsHeuristic() {
    DisplayTopologyObservation observation;
    auto virtualPath = path(luid(0x5555), 1, 0, L"\\\\.\\DISPLAYV",
                            L"ROOT#INDIRECTDISPLAY#0001", {0, 0, 1280, 720});
    virtualPath.friendlyName = L"Hydra Virtual Display";
    virtualPath.technologySuggestsVirtual = true;
    observation.paths.push_back(virtualPath);

    auto unknownPath = path(luid(0x5555), 2, 1, L"", L"", {1280, 0, 2560, 720});
    unknownPath.friendlyName.clear();
    unknownPath.edidManufacturerId = 0;
    unknownPath.edidProductCodeId = 0;
    observation.paths.push_back(unknownPath);

    const auto snapshot = assembleDisplayTopology(observation, 40);
    const auto virtualOutput = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(),
                                            [](const DisplayOutput& output) {
                                                return output.identity.targetId == 1u;
                                            });
    check(virtualOutput != snapshot.outputs.end() &&
              virtualOutput->virtualLikelihood == VirtualDisplayLikelihood::VirtualLikely &&
              virtualOutput->classificationConfidence == ClassificationConfidence::High,
          "virtual metadata produces a likely/high-confidence heuristic, not certainty");
    const auto unknownOutput = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(),
                                            [](const DisplayOutput& output) {
                                                return output.identity.targetId == 2u;
                                            });
    check(unknownOutput != snapshot.outputs.end() &&
              unknownOutput->virtualLikelihood == VirtualDisplayLikelihood::Unknown,
          "missing physical/virtual evidence remains explicitly unknown");
}

void testExtremeDpiScaleDoesNotOverflow() {
    DisplayTopologyObservation observation;
    auto extreme = path(luid(0x5a5a), 9, 0, L"\\\\.\\DISPLAY9",
                        L"MONITOR#DPI#9", {0, 0, 1920, 1080});
    extreme.dpiX = std::numeric_limits<std::uint32_t>::max();
    observation.paths.push_back(extreme);
    const auto snapshot = assembleDisplayTopology(observation, 45);
    check(snapshot.outputs.size() == 1u &&
              snapshot.outputs.front().effectiveScalePercent ==
                  std::numeric_limits<std::uint32_t>::max(),
          "extreme DPI scale uses wide arithmetic and saturates instead of overflowing");
}

void testBoundedTopologyChangeRetry() {
    int calls = 0;
    DisplayTopologyQuery query = [&]() {
        ++calls;
        DisplayQueryResult result;
        if (calls < 3) {
            result.status = DisplayQueryStatus::TopologyChanged;
            result.error = "synthetic topology race";
            return result;
        }
        result.status = DisplayQueryStatus::Success;
        result.observation.paths.push_back(path(luid(0x6666), 1, 0,
                                                L"\\\\.\\DISPLAY1", L"MONITOR#R#1",
                                                {0, 0, 1920, 1080}));
        return result;
    };
    const auto recovered = collectDisplayTopology(query, 50, 3);
    check(recovered.querySucceeded && recovered.queryAttempts == 3u && calls == 3,
          "topology-changing race retries only up to the bounded successful attempt");

    calls = 0;
    DisplayTopologyQuery alwaysChanging = [&]() {
        ++calls;
        DisplayQueryResult result;
        result.status = DisplayQueryStatus::TopologyChanged;
        result.error = "still changing";
        return result;
    };
    const auto failed = collectDisplayTopology(alwaysChanging, 51, 2);
    check(!failed.querySucceeded && failed.queryAttempts == 2u && calls == 2 &&
              !failed.diagnostics.empty(),
          "repeated topology races stop at bounded retry count with diagnostic");
}

void testStableKeyUsesPersistentPathWhenAvailable() {
    DisplayOutputIdentity first;
    first.adapterLuid = luid(1);
    first.targetId = 3;
    first.monitorDevicePath = L"MONITOR#ABC#0001";
    auto reordered = first;
    reordered.adapterLuid = luid(9999); // array/adapter observation order must not matter.
    check(first.stableKey() == reordered.stableKey(),
          "monitor device path is preferred over volatile adapter enumeration identity");

    DisplayOutputIdentity fallback;
    fallback.adapterLuid = luid(2, 1);
    fallback.targetId = 7;
    check(fallback.stableKey().find("output-") == 0,
          "adapter LUID plus target ID is an explicit fallback when path metadata is unavailable");
}

#ifdef _WIN32
void testActualWindowsReadOnlyInventory() {
    DisplayTopologyInventory inventory;
    const auto first = inventory.refresh();
    check(first.querySucceeded, "actual Windows read-only display topology query succeeds");
    check(first.generation == 1u && first.queryAttempts >= 1u && first.queryAttempts <= 3u,
          "actual topology generation/retry metadata is bounded");
    check(!first.outputs.empty(), "actual Windows topology exposes at least one target");
    const auto activeCount = std::count_if(first.outputs.begin(), first.outputs.end(),
                                           [](const DisplayOutput& output) { return output.active; });
    check(activeCount >= 1, "actual Windows topology exposes at least one active output");

    std::set<std::string> keys;
    for (const auto& output : first.outputs) {
        check(keys.insert(output.identity.stableKey()).second,
              "actual topology stable output IDs are unique after target de-duplication");
        if (output.active) {
            check(output.desktopBounds.width() > 0 && output.desktopBounds.height() > 0,
                  "active output has non-empty desktop bounds");
            check(output.dpiX > 0 && output.dpiY > 0,
                  "active output has explicit effective DPI");
        }
    }

    const auto second = inventory.refresh();
    check(second.generation == 2u,
          "read-only refresh increments topology generation without mutating display mode");
}
#endif

} // namespace

int main() {
    testStableCorrelationIndependentOfEnumerationOrder();
    testDuplicatePathsDisconnectedAndNegativeCoordinates();
    testDuplicateNamesDoNotCrossAdapters();
    testVirtualClassificationIsHeuristic();
    testExtremeDpiScaleDoesNotOverflow();
    testBoundedTopologyChangeRetry();
    testStableKeyUsesPersistentPathWhenAvailable();
#ifdef _WIN32
    testActualWindowsReadOnlyInventory();
#endif

    if (failures != 0) {
        std::cerr << failures << " display topology test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Display topology tests passed.\n";
    return EXIT_SUCCESS;
}
