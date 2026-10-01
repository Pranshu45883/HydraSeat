#include "hydra/display_recovery.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using namespace hydra;
using namespace hydra::display;
int failures = 0;
void check(bool condition, const char* message) { if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }

DisplayOutput makeOutput(std::uint32_t target, const wchar_t* path, DisplayRect bounds,
                         bool active = true, std::uint32_t dpi = 96) {
    DisplayOutput output;
    output.identity.adapterLuid = {1u, 0};
    output.identity.targetId = target;
    output.identity.monitorDevicePath = path;
    output.desktopBounds = bounds;
    output.active = active;
    output.attached = active;
    output.dpiX = dpi;
    output.dpiY = dpi;
    return output;
}
DisplayTopologySnapshot topology(std::uint64_t generation, std::vector<DisplayOutput> outputs) {
    DisplayTopologySnapshot t; t.generation = generation; t.querySucceeded = true; t.outputs = std::move(outputs); return t;
}
SeatDisplayRecoveryProfile profileFor(const DisplayOutput& primary, const DisplayOutput& secondary) {
    SeatDisplayRecoveryProfile p; p.request.seatId = 1;
    p.request.primaryOutputId = primary.identity.stableKey();
    p.request.outputs.push_back({primary.identity.stableKey(), true, false});
    p.request.outputs.push_back({secondary.identity.stableKey(), false, false});
    return p;
}

void testSecondaryAndPrimaryLoss() {
    const auto primary = makeOutput(1, L"MON#P", {0,0,1920,1080});
    const auto secondary = makeOutput(2, L"MON#S", {1920,0,3840,1080});
    const auto before = topology(1, {primary, secondary});
    auto missingSecondary = secondary; missingSecondary.active = false; missingSecondary.attached = false;
    const auto after = topology(2, {primary, missingSecondary});
    auto profile = profileFor(primary, secondary);
    auto decision = planSeatDisplayRecovery(before, after, profile);
    check(decision.disposition == DisplayRecoveryDisposition::DegradedToSeatPrimary && decision.degraded,
          "optional secondary loss degrades only to same Seat primary");

    auto missingPrimary = primary; missingPrimary.active = false; missingPrimary.attached = false;
    decision = planSeatDisplayRecovery(before, topology(3, {missingPrimary, secondary}), profile);
    check(decision.disposition == DisplayRecoveryDisposition::PauseRequired,
          "required primary loss requires pause by default");
    profile.primaryLossPolicy = RequiredPrimaryLossPolicy::StopAndReturnToWindows;
    decision = planSeatDisplayRecovery(before, topology(4, {missingPrimary, secondary}), profile);
    check(decision.disposition == DisplayRecoveryDisposition::StopRequired,
          "profile may require StopAndReturnToWindows on primary loss");
}

void testReconnectStableIdentityAndReorder() {
    const auto primary = makeOutput(10, L"MON#P2", {0,0,1920,1080});
    const auto secondary = makeOutput(11, L"MON#S2", {1920,0,3840,1080});
    const auto before = topology(10, {primary, secondary});
    auto profile = profileFor(primary, secondary);
    const auto initial = buildSeatDisplayLayouts(before, {profile.request});
    check(initial.valid && !initial.groups.empty(), "initial layout resolves");
    auto changedSecondary = secondary; changedSecondary.desktopBounds = {-2560,0,0,1440}; changedSecondary.dpiX = 144; changedSecondary.dpiY = 144;
    const auto reordered = topology(11, {changedSecondary, primary});
    const auto decision = planSeatDisplayRecovery(before, reordered, profile, &initial.groups.front());
    check(decision.disposition == DisplayRecoveryDisposition::RestoreStableLayout && decision.stableIdentityConfirmed,
          "reconnect/reorder uses stable identity and detects coordinate/DPI change");
}

void testDebounceBurst() {
    DisplayTopologyDebouncer debounce(250u); std::uint64_t accepted = 0;
    debounce.observe(2, 1000); debounce.observe(3, 1100); debounce.observe(4, 1200);
    check(!debounce.accept(1400, accepted), "rapid topology burst waits for quiet period");
    check(debounce.accept(1450, accepted) && accepted == 4u,
          "debouncer accepts only final generation after quiet period");
    debounce.observe(3, 1500);
    check(!debounce.accept(2000, accepted) && accepted == 4u,
          "delayed older topology generation cannot replace the accepted hot-plug state");
    debounce.observe(5, 2100);
    debounce.observe(4, 2200);
    check(debounce.accept(2350, accepted) && accepted == 5u,
          "older callback cannot move a newer pending topology generation backwards");
    check(!debounce.accept(2600, accepted), "accepted generation is idempotent");
}
}

int main() {
    testSecondaryAndPrimaryLoss(); testReconnectStableIdentityAndReorder(); testDebounceBurst();
    if (failures) return EXIT_FAILURE;
    std::cout << "display recovery tests passed\n"; return EXIT_SUCCESS;
}
