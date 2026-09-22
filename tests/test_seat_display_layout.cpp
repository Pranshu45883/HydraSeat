#include "hydra/seat_display_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
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

bool near(double left, double right, double tolerance = 1e-9) {
    return std::abs(left - right) <= tolerance;
}

DisplayOutput makeOutput(std::uint32_t targetId, std::wstring path,
                         DisplayRect bounds, std::uint32_t dpi = 96,
                         DisplayOrientation orientation = DisplayOrientation::Identity,
                         bool active = true, bool windowsPrimary = false) {
    DisplayOutput output;
    output.identity.adapterLuid = AdapterLuid{1u, 0};
    output.identity.targetId = targetId;
    output.identity.monitorDevicePath = std::move(path);
    output.desktopBounds = bounds;
    output.mode.width = static_cast<std::uint32_t>(std::max(0, bounds.width()));
    output.mode.height = static_cast<std::uint32_t>(std::max(0, bounds.height()));
    output.mode.orientation = orientation;
    output.dpiX = dpi;
    output.dpiY = dpi;
    output.active = active;
    output.attached = active;
    output.primary = windowsPrimary;
    return output;
}

DisplayTopologySnapshot topologyOf(std::vector<DisplayOutput> outputs) {
    DisplayTopologySnapshot topology;
    topology.querySucceeded = true;
    topology.generation = 1;
    topology.outputs = std::move(outputs);
    return topology;
}

SeatDisplaySelection select(const DisplayOutput& output, bool required = true,
                            bool shareable = false) {
    return SeatDisplaySelection{output.identity.stableKey(), required, shareable};
}

void testHorizontalLayoutAndSeatPrimaryOrigin() {
    const auto left = makeOutput(1, L"MONITOR#LEFT", {-1920, 0, 0, 1080}, 96,
                                 DisplayOrientation::Identity, true, true);
    const auto right = makeOutput(2, L"MONITOR#RIGHT", {0, 0, 2560, 1440}, 144);
    const auto topology = topologyOf({left, right});

    SeatDisplayRequest request;
    request.seatId = 1;
    request.outputs = {select(left), select(right)};
    request.primaryOutputId = right.identity.stableKey();

    const auto validation = buildSeatDisplayLayouts(topology, {request});
    check(validation.valid && !validation.degraded && validation.groups.size() == 1,
          "horizontal multi-output Seat layout resolves without degradation");
    if (!validation.valid || validation.groups.empty()) return;
    const auto& group = validation.groups.front();
    check(group.globalBounds == DisplayRect{-1920, 0, 2560, 1440},
          "horizontal Seat union preserves negative global origin");
    check(group.primaryOriginX == 0 && group.primaryOriginY == 0 &&
              group.primaryOutputId == right.identity.stableKey(),
          "Seat primary origin follows profile, not Windows global primary flag");

    CoordinateTransform transform(group);
    const auto leftSeat = transform.globalToSeat({-1920.0, 100.0});
    check(near(leftSeat.x, -1920.0) && near(leftSeat.y, 100.0),
          "global-to-Seat transform permits negative coordinates left of Seat primary");
    const auto roundTrip = transform.seatToGlobal(leftSeat);
    check(near(roundTrip.x, -1920.0) && near(roundTrip.y, 100.0),
          "Seat/global transform round trip is reversible");

    const auto rightLocal = transform.globalToOutput(right.identity.stableKey(), {1280.0, 720.0});
    check(rightLocal && near(rightLocal->x, 1280.0) && near(rightLocal->y, 720.0),
          "global-to-output transform uses selected output origin");
    const auto leftLocal = transform.globalToOutput(left.identity.stableKey(), {-960.0, 540.0});
    check(leftLocal && near(leftLocal->x, 960.0) && near(leftLocal->y, 540.0),
          "negative global display converts to positive output-local coordinates");

    const auto dip = transform.physicalPixelsToDip(right.identity.stableKey(), {144.0, 288.0});
    check(dip && near(dip->x, 96.0) && near(dip->y, 192.0),
          "mixed-DPI output converts physical pixels to DIPs using per-output DPI");
    const auto pixels = dip ? transform.dipToPhysicalPixels(right.identity.stableKey(), *dip)
                            : std::nullopt;
    check(pixels && near(pixels->x, 144.0) && near(pixels->y, 288.0),
          "DIP/physical conversion round trip is reversible");
}

void testVerticalAndLShapedClipping() {
    const auto primary = makeOutput(10, L"MONITOR#P", {0, 0, 1920, 1080});
    const auto above = makeOutput(11, L"MONITOR#A", {0, -1200, 1920, 0});
    const auto rightLow = makeOutput(12, L"MONITOR#R", {1920, 540, 3200, 1564}, 120,
                                     DisplayOrientation::Rotate90);
    const auto topology = topologyOf({primary, above, rightLow});

    SeatDisplayRequest request;
    request.seatId = 2;
    request.outputs = {select(primary), select(above), select(rightLow)};
    request.primaryOutputId = primary.identity.stableKey();
    const auto validation = buildSeatDisplayLayouts(topology, {request});
    check(validation.valid, "vertical/L-shaped three-output layout resolves");
    if (!validation.valid) return;
    const auto& group = validation.groups.front();
    check(group.globalBounds == DisplayRect{0, -1200, 3200, 1564},
          "L-shaped union computes bounding rectangle without assuming contiguous desktop");

    CoordinateTransform transform(group);
    const auto pieces = transform.clipGlobalRectToOutputs({1500.0, -200.0, 2500.0, 900.0});
    check(pieces.size() == 3u,
          "rectangle clipping returns separate pieces for each intersected L-shaped output");
    double clippedArea = 0.0;
    for (const auto& piece : pieces) clippedArea += piece.width() * piece.height();
    check(near(clippedArea, 420.0 * 200.0 + 420.0 * 900.0 + 580.0 * 360.0),
          "clipping preserves output gaps instead of filling Seat union holes");

    const auto rotated = std::find_if(group.outputs.begin(), group.outputs.end(),
                                      [&](const SeatDisplayOutput& output) {
                                          return output.outputId == rightLow.identity.stableKey();
                                      });
    check(rotated != group.outputs.end() &&
              rotated->orientation == DisplayOrientation::Rotate90 &&
              rotated->dpiX == 120u,
          "orientation and per-output DPI remain explicit in Seat layout contract");
}

void testMissingRequiredBlockAndDegrade() {
    const auto present = makeOutput(20, L"MONITOR#PRESENT", {0, 0, 1920, 1080});
    auto missing = makeOutput(21, L"MONITOR#MISSING", {1920, 0, 3840, 1080}, 96,
                              DisplayOrientation::Identity, false);
    const auto topology = topologyOf({present, missing});

    SeatDisplayRequest blocked;
    blocked.seatId = 3;
    blocked.outputs = {select(present), select(missing)};
    blocked.primaryOutputId = present.identity.stableKey();
    blocked.missingOutputPolicy = MissingOutputPolicy::Block;
    const auto blockedResult = buildSeatDisplayLayouts(topology, {blocked});
    check(!blockedResult.valid && !blockedResult.errors.empty(),
          "missing required active output blocks strict Seat layout");

    auto degraded = blocked;
    degraded.missingOutputPolicy = MissingOutputPolicy::Degrade;
    const auto degradedResult = buildSeatDisplayLayouts(topology, {degraded});
    check(degradedResult.valid && degradedResult.degraded &&
              degradedResult.groups.size() == 1 &&
              degradedResult.groups.front().outputs.size() == 1,
          "missing required output can explicitly degrade to remaining active output");
    check(!degradedResult.warnings.empty(),
          "degraded missing-output decision is visible in validation diagnostics");

    SeatDisplayRequest missingPrimary = degraded;
    missingPrimary.primaryOutputId = missing.identity.stableKey();
    const auto fallbackPrimary = buildSeatDisplayLayouts(topology, {missingPrimary});
    check(fallbackPrimary.valid && fallbackPrimary.degraded &&
              fallbackPrimary.groups.front().primaryOutputId == present.identity.stableKey(),
          "degraded missing Seat-primary chooses a deterministic resolved fallback");
}

void testZeroAndOptionalOutputs() {
    const auto output = makeOutput(30, L"MONITOR#ONLY", {0, 0, 1280, 720});
    const auto topology = topologyOf({output});

    SeatDisplayRequest zero;
    zero.seatId = 4;
    const auto zeroResult = buildSeatDisplayLayouts(topology, {zero});
    check(!zeroResult.valid, "Seat with zero requested outputs is invalid");

    SeatDisplayRequest optional;
    optional.seatId = 4;
    optional.outputs = {select(output), SeatDisplaySelection{"missing-optional", false, false}};
    optional.primaryOutputId = output.identity.stableKey();
    const auto optionalResult = buildSeatDisplayLayouts(topology, {optional});
    check(optionalResult.valid && !optionalResult.degraded &&
              !optionalResult.warnings.empty(),
          "missing optional output warns without degrading a valid required layout");
}

void testSeatExclusivityAndExplicitSharing() {
    const auto shared = makeOutput(40, L"MONITOR#SHARED", {0, 0, 1920, 1080});
    const auto topology = topologyOf({shared});

    SeatDisplayRequest one;
    one.seatId = 5;
    one.outputs = {select(shared, true, false)};
    SeatDisplayRequest two = one;
    two.seatId = 6;
    const auto rejected = buildSeatDisplayLayouts(topology, {one, two});
    check(!rejected.valid,
          "same output assigned to two Seats is rejected unless every owner marks it shareable");

    one.outputs.front().shareable = true;
    two.outputs.front().shareable = true;
    const auto sharedResult = buildSeatDisplayLayouts(topology, {one, two});
    check(sharedResult.valid && sharedResult.groups.size() == 2,
          "explicitly shareable display can be resolved into two Seat groups");
}

void testDistinctCloneOutputsRequireExplicitSharing() {
    const auto cloneA = makeOutput(45, L"MONITOR#CLONE-A", {0, 0, 1920, 1080});
    const auto cloneB = makeOutput(46, L"MONITOR#CLONE-B", {0, 0, 1920, 1080});
    const auto topology = topologyOf({cloneA, cloneB});

    SeatDisplayRequest seatA;
    seatA.seatId = 10;
    seatA.outputs = {select(cloneA)};
    SeatDisplayRequest seatB;
    seatB.seatId = 11;
    seatB.outputs = {select(cloneB)};
    const auto rejected = buildSeatDisplayLayouts(topology, {seatA, seatB});
    check(!rejected.valid,
          "distinct cloned outputs with overlapping desktop bounds cannot silently split across Seats");

    seatA.outputs.front().shareable = true;
    seatB.outputs.front().shareable = true;
    const auto allowed = buildSeatDisplayLayouts(topology, {seatA, seatB});
    check(allowed.valid,
          "distinct spatially overlapping outputs require explicit sharing on both Seat assignments");
}

void testDeterministicPrimaryFallback() {
    const auto first = makeOutput(47, L"MONITOR#PRIMARY-A", {0, 0, 1280, 720});
    const auto second = makeOutput(48, L"MONITOR#PRIMARY-B", {1280, 0, 2560, 720});
    const auto topology = topologyOf({first, second});

    SeatDisplayRequest forward;
    forward.seatId = 12;
    forward.outputs = {select(first), select(second)};
    SeatDisplayRequest reversed = forward;
    reversed.outputs = {select(second), select(first)};

    const auto a = buildSeatDisplayLayouts(topology, {forward});
    const auto b = buildSeatDisplayLayouts(topology, {reversed});
    check(a.valid && b.valid && !a.groups.empty() && !b.groups.empty() &&
              a.groups.front().primaryOutputId == b.groups.front().primaryOutputId,
          "implicit Seat primary fallback is deterministic independent of selection enumeration order");
}

void testTwoDisjointSeatsAndClientTransforms() {
    const auto left = makeOutput(50, L"MONITOR#S1", {-1600, 0, 0, 900});
    const auto right = makeOutput(51, L"MONITOR#S2", {0, 0, 1920, 1080});
    const auto topology = topologyOf({left, right});

    SeatDisplayRequest seat1;
    seat1.seatId = 7;
    seat1.outputs = {select(left)};
    SeatDisplayRequest seat2;
    seat2.seatId = 8;
    seat2.outputs = {select(right)};
    const auto result = buildSeatDisplayLayouts(topology, {seat2, seat1});
    check(result.valid && result.groups.size() == 2 &&
              result.groups[0].seatId == 7 && result.groups[1].seatId == 8,
          "two disjoint Seat groups resolve deterministically by Seat ID");

    if (!result.valid) return;
    CoordinateTransform transform(result.groups[1]);
    const CoordinatePoint clientOrigin{300.0, 200.0};
    const auto client = transform.globalToClient({350.0, 275.0}, clientOrigin);
    check(near(client.x, 50.0) && near(client.y, 75.0),
          "global-to-client translation is explicit and independent of Seat primary");
    const auto global = transform.clientToGlobal(client, clientOrigin);
    check(near(global.x, 350.0) && near(global.y, 275.0),
          "client/global transform round trip is reversible");
}

void testInvalidTopologyAndDuplicateStableIds() {
    DisplayTopologySnapshot unavailable;
    SeatDisplayRequest request;
    request.seatId = 9;
    request.outputs.push_back({"anything", true, false});
    const auto unavailableResult = buildSeatDisplayLayouts(unavailable, {request});
    check(!unavailableResult.valid && !unavailableResult.errors.empty(),
          "unavailable topology blocks Seat display layout construction");

    auto first = makeOutput(60, L"MONITOR#DUPLICATE", {0, 0, 800, 600});
    auto second = makeOutput(61, L"MONITOR#DUPLICATE", {800, 0, 1600, 600});
    second.identity.targetId = first.identity.targetId;
    auto duplicateTopology = topologyOf({first, second});
    request.outputs = {select(first)};
    const auto duplicateResult = buildSeatDisplayLayouts(duplicateTopology, {request});
    check(!duplicateResult.valid,
          "duplicate active stable output IDs fail closed instead of choosing by enumeration order");
}

} // namespace

int main() {
    testHorizontalLayoutAndSeatPrimaryOrigin();
    testVerticalAndLShapedClipping();
    testMissingRequiredBlockAndDegrade();
    testZeroAndOptionalOutputs();
    testSeatExclusivityAndExplicitSharing();
    testDistinctCloneOutputsRequireExplicitSharing();
    testDeterministicPrimaryFallback();
    testTwoDisjointSeatsAndClientTransforms();
    testInvalidTopologyAndDuplicateStableIds();

    if (failures != 0) {
        std::cerr << failures << " Seat display layout test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Seat display layout tests passed.\n";
    return EXIT_SUCCESS;
}
