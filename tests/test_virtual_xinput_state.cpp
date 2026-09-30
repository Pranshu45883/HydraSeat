#include "hydra/virtual_xinput_state.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using namespace hydra::gatec;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

ControllerSourceIdentity source(std::uint64_t key,
                                std::uint8_t runtimeSlot) {
    return {ControllerSourceKind::Synthetic, runtimeSlot, key};
}

NormalizedXInputGamepad stateA() {
    NormalizedXInputGamepad value;
    value.buttons = 0x1100u;
    value.leftTrigger = 20u;
    value.rightTrigger = 200u;
    value.thumbLX = -12000;
    value.thumbLY = 9000;
    value.thumbRX = -321;
    value.thumbRY = 654;
    return value;
}

NormalizedXInputGamepad stateB() {
    NormalizedXInputGamepad value;
    value.buttons = 0x2200u;
    value.leftTrigger = 180u;
    value.rightTrigger = 5u;
    value.thumbLX = 123;
    value.thumbLY = -456;
    value.thumbRX = 16000;
    value.thumbRY = -7000;
    return value;
}

NormalizedXInputCapabilities capabilitiesA() {
    NormalizedXInputCapabilities value;
    value.subtype = 1u;
    value.flags = 0x0001u;
    value.gamepad = stateA();
    value.vibrationSupported = true;
    value.leftMotorMaximum = 65535u;
    value.rightMotorMaximum = 60000u;
    return value;
}

NormalizedXInputBattery batteryA() {
    return {true, XInputBatteryDeviceType::Gamepad,
            XInputBatteryType::Alkaline, XInputBatteryLevel::Full};
}

void testSlotsMappingAndPacketSemantics() {
    VirtualXInputContext context;
    VirtualXInputState queried;
    check(context.getState(0, queried) == VirtualXInputResult::NotMapped,
          "empty context reports logical slot 0 as unmapped");
    for (std::uint8_t slot = 0; slot < 4u; ++slot) {
        check(context.mapLogicalSlot(
                  static_cast<std::uint64_t>(slot) + 1u, slot,
                  source(static_cast<std::uint64_t>(slot) + 1u, slot), 1u) ==
                  VirtualXInputResult::Success,
              "logical slots 0 through 3 are bounded and accepted");
    }
    check(context.mapLogicalSlot(5u, 4u, source(8u, 0u), 1u) ==
              VirtualXInputResult::InvalidLogicalSlot,
          "logical slot 4 is rejected before array access");
    check(context.mapLogicalSlot(5u, 3u, source(1u, 0u), 1u) ==
              VirtualXInputResult::DuplicateSource,
          "one context rejects duplicate source ownership");
    check(context.mapLogicalSlot(5u, 3u, source(1u, 3u), 1u) ==
              VirtualXInputResult::DuplicateSource,
          "runtime slot hint changes cannot bypass duplicate source ownership");
    check(sameControllerSourceIdentity(source(1u, 0u), source(1u, 3u)) &&
              source(1u, 0u) != source(1u, 3u),
          "opaque source identity is independent from its runtime slot hint");

    VirtualXInputContext hintContext;
    const auto hintedA = source(88u, 0u);
    const auto movedHintA = source(88u, 1u);
    VirtualXInputMapping hintBefore;
    VirtualXInputMapping hintAfter;
    check(hintContext.mapLogicalSlot(1u, 0u, hintedA, 1u) ==
              VirtualXInputResult::Success &&
              hintContext.applySourceState(2u, hintedA, 1u, stateA()) ==
                  VirtualXInputResult::Success &&
              hintContext.getMapping(0u, hintBefore) ==
                  VirtualXInputResult::Success,
          "a source starts with explicit runtime routing metadata");
    check(hintContext.applySourceCapabilities(
              3u, movedHintA, 1u, capabilitiesA()) ==
              VirtualXInputResult::InvalidState &&
              hintContext.applySourceBattery(4u, movedHintA, 1u, batteryA()) ==
              VirtualXInputResult::InvalidState &&
              hintContext.applySourceState(5u, movedHintA, 1u, stateA()) ==
              VirtualXInputResult::InvalidState,
          "runtime slot metadata changes require an explicit remap");
    check(hintContext.mapLogicalSlot(6u, 0u, movedHintA, 1u) ==
              VirtualXInputResult::Success &&
              hintContext.getMapping(0u, hintAfter) ==
                  VirtualXInputResult::Success &&
              hintAfter.source == movedHintA &&
              hintAfter.mappingGeneration == hintBefore.mappingGeneration + 1u &&
              hintContext.applySourceState(7u, movedHintA, 1u, stateA()) ==
                  VirtualXInputResult::Success,
          "explicit remap refreshes routing metadata and mapping generation");

    VirtualXInputContext packetContext;
    const auto a = source(101u, 0u);
    check(packetContext.mapLogicalSlot(1u, 0u, a, 10u) ==
              VirtualXInputResult::Success &&
              packetContext.applySourceState(2u, a, 10u, stateA()) ==
                  VirtualXInputResult::Success &&
              packetContext.getState(0u, queried) ==
                  VirtualXInputResult::Success &&
              queried.packetNumber == 1u,
          "first connected state advances the packet number once");
    check(packetContext.applySourceState(3u, a, 10u, stateA()) ==
              VirtualXInputResult::Success &&
              packetContext.getState(0u, queried) ==
                  VirtualXInputResult::Success &&
              queried.packetNumber == 1u,
          "identical state consumes sequence but not packet number");
    auto changed = stateA();
    changed.rightTrigger = 201u;
    check(packetContext.applySourceState(4u, a, 10u, changed) ==
              VirtualXInputResult::Success &&
              packetContext.getState(0u, queried) ==
                  VirtualXInputResult::Success &&
              queried.packetNumber == 2u,
          "a real gamepad-state change increments packet number");
    check(nextXInputPacketNumber(
              (std::numeric_limits<std::uint32_t>::max)()) == 0u,
          "packet number wraps modulo 2^32");

    const auto b = source(202u, 1u);
    VirtualXInputMapping before;
    check(packetContext.getMapping(0u, before) ==
              VirtualXInputResult::Success &&
              packetContext.mapLogicalSlot(5u, 0u, b, 20u) ==
                  VirtualXInputResult::Success,
          "logical slot 0 remaps from source A to source B");
    VirtualXInputMapping after;
    check(packetContext.getMapping(0u, after) ==
              VirtualXInputResult::Success &&
              after.mappingGeneration == before.mappingGeneration + 1u &&
              packetContext.getState(0u, queried) ==
                  VirtualXInputResult::Disconnected &&
              queried.gamepad == NormalizedXInputGamepad{},
          "remap increments mapping generation and exposes no stale state");
}

void testContextIsolationAndExtrema() {
    VirtualXInputContext first;
    VirtualXInputContext second;
    const auto a = source(1001u, 0u);
    const auto b = source(2002u, 1u);
    check(first.mapLogicalSlot(1u, 0u, a, 5u) ==
              VirtualXInputResult::Success &&
              second.mapLogicalSlot(1u, 0u, b, 8u) ==
                  VirtualXInputResult::Success &&
              first.applySourceState(2u, a, 5u, stateA()) ==
                  VirtualXInputResult::Success &&
              second.applySourceState(2u, b, 8u, stateB()) ==
                  VirtualXInputResult::Success,
          "two contexts map logical slot 0 to different sources");
    VirtualXInputState firstState;
    VirtualXInputState secondState;
    check(first.getState(0u, firstState) == VirtualXInputResult::Success &&
              second.getState(0u, secondState) ==
                  VirtualXInputResult::Success &&
              firstState.gamepad == stateA() &&
              secondState.gamepad == stateB() &&
              firstState.mapping.source != secondState.mapping.source,
          "state and source identity never cross adapter contexts");

    NormalizedXInputGamepad extrema;
    extrema.buttons = 0xffffu;
    extrema.leftTrigger = 0u;
    extrema.rightTrigger = 255u;
    extrema.thumbLX = (std::numeric_limits<std::int16_t>::min)();
    extrema.thumbLY = (std::numeric_limits<std::int16_t>::max)();
    extrema.thumbRX = (std::numeric_limits<std::int16_t>::max)();
    extrema.thumbRY = (std::numeric_limits<std::int16_t>::min)();
    check(first.applySourceState(3u, a, 5u, extrema) ==
              VirtualXInputResult::Success &&
              first.getState(0u, firstState) ==
                  VirtualXInputResult::Success &&
              firstState.gamepad == extrema,
          "all button bits, trigger endpoints, and signed thumb extrema round-trip");
}

void testCapabilitiesBatteryAndVibration() {
    VirtualXInputContext first;
    VirtualXInputContext second;
    const auto a = source(3003u, 0u);
    const auto b = source(4004u, 1u);
    auto capA = capabilitiesA();
    auto capB = capabilitiesA();
    capB.subtype = 2u;
    capB.flags = 0x0002u;
    capB.gamepad = stateB();
    auto batA = batteryA();
    NormalizedXInputBattery batB{
        true, XInputBatteryDeviceType::Gamepad,
        XInputBatteryType::Wired, XInputBatteryLevel::Medium};
    check(first.mapLogicalSlot(1u, 0u, a, 5u) ==
              VirtualXInputResult::Success &&
              second.mapLogicalSlot(1u, 0u, b, 7u) ==
                  VirtualXInputResult::Success &&
              first.applySourceState(2u, a, 5u, stateA()) ==
                  VirtualXInputResult::Success &&
              second.applySourceState(2u, b, 7u, stateB()) ==
                  VirtualXInputResult::Success &&
              first.applySourceCapabilities(3u, a, 5u, capA) ==
                  VirtualXInputResult::Success &&
              second.applySourceCapabilities(3u, b, 7u, capB) ==
                  VirtualXInputResult::Success &&
              first.applySourceBattery(4u, a, 5u, batA) ==
                  VirtualXInputResult::Success &&
              second.applySourceBattery(4u, b, 7u, batB) ==
                  VirtualXInputResult::Success,
          "distinct capabilities and battery data apply to each source");
    VirtualXInputCapabilities queriedCapA;
    VirtualXInputCapabilities queriedCapB;
    VirtualXInputBattery queriedBatA;
    VirtualXInputBattery queriedBatB;
    check(first.getCapabilities(0u, queriedCapA) ==
              VirtualXInputResult::Success &&
              second.getCapabilities(0u, queriedCapB) ==
                  VirtualXInputResult::Success &&
              queriedCapA.capabilities == capA &&
              queriedCapB.capabilities == capB &&
              first.getBattery(0u, queriedBatA) ==
                  VirtualXInputResult::Success &&
              second.getBattery(0u, queriedBatB) ==
                  VirtualXInputResult::Success &&
              queriedBatA.battery == batA && queriedBatB.battery == batB,
          "capabilities and battery preserve source/mapping consistency");
    check(first.applySourceCapabilities(5u, a, 4u, capA) ==
              VirtualXInputResult::StaleGeneration &&
              first.applySourceBattery(5u, a, 4u, batA) ==
                  VirtualXInputResult::StaleGeneration,
          "stale-generation capabilities and battery updates are rejected");

    VirtualXInputMapping mappingA;
    VirtualXInputMapping mappingB;
    check(first.getMapping(0u, mappingA) == VirtualXInputResult::Success &&
              second.getMapping(0u, mappingB) ==
                  VirtualXInputResult::Success,
          "both vibration mappings are queryable");
    VirtualXInputVibrationRoute routeA;
    VirtualXInputVibrationRoute routeB;
    VirtualXInputVibrationRequest requestA{
        0u, 100u, 200u, mappingA.mappingGeneration,
        mappingA.sourceGeneration};
    VirtualXInputVibrationRequest requestB{
        0u, 65535u, 0u, mappingB.mappingGeneration,
        mappingB.sourceGeneration};
    auto invalidSlotRequest = requestA;
    invalidSlotRequest.logicalSlot = 4u;
    check(first.routeVibration(5u, invalidSlotRequest, routeA) ==
              VirtualXInputResult::InvalidLogicalSlot,
          "vibration rejects logical slot 4 without consuming sequence");
    check(first.routeVibration(5u, requestA, routeA) ==
              VirtualXInputResult::Success &&
              second.routeVibration(5u, requestB, routeB) ==
                  VirtualXInputResult::Success &&
              routeA.source == a && routeB.source == b &&
              routeA.source != b && routeB.source != a,
          "Seat-local vibration routes only to the mapped source");
    requestA.leftMotor = 0u;
    requestA.rightMotor = 0u;
    VirtualXInputVibrationRoute stopRoute;
    check(first.routeVibration(6u, requestA, stopRoute) ==
              VirtualXInputResult::Success &&
              stopRoute.routeCount == 2u && stopRoute.leftMotor == 0u &&
              stopRoute.rightMotor == 0u,
          "repeated and zero/stop vibration commands are deterministic");
    requestA.expectedMappingGeneration += 1u;
    check(first.routeVibration(7u, requestA, stopRoute) ==
              VirtualXInputResult::MappingGenerationMismatch,
          "stale mapping-generation vibration fails before routing");
    requestA.expectedMappingGeneration -= 1u;
    requestA.expectedSourceGeneration -= 1u;
    check(first.routeVibration(7u, requestA, stopRoute) ==
              VirtualXInputResult::StaleGeneration,
          "stale source-generation vibration fails before routing");

    const auto remapped = source(5000u, 3u);
    check(first.mapLogicalSlot(7u, 0u, remapped, 9u) ==
              VirtualXInputResult::Success &&
              first.applySourceState(8u, remapped, 9u, stateB()) ==
                  VirtualXInputResult::Success &&
              first.applySourceCapabilities(9u, remapped, 9u, capB) ==
                  VirtualXInputResult::Success,
          "slot remap installs a distinct source and connection generation");
    requestA.expectedSourceGeneration = 5u;
    requestA.expectedMappingGeneration = mappingA.mappingGeneration;
    check(first.routeVibration(10u, requestA, stopRoute) ==
              VirtualXInputResult::MappingGenerationMismatch,
          "a vibration created before remap cannot route afterward");
    VirtualXInputMapping remappedMapping;
    check(first.getMapping(0u, remappedMapping) ==
              VirtualXInputResult::Success,
          "remapped vibration generation is queryable");
    VirtualXInputVibrationRequest remappedRequest{
        0u, 7u, 8u, remappedMapping.mappingGeneration,
        remappedMapping.sourceGeneration};
    check(first.routeVibration(10u, remappedRequest, stopRoute) ==
              VirtualXInputResult::Success &&
              stopRoute.source == remapped && stopRoute.source != a,
          "post-remap vibration routes only to the new source");

    NormalizedXInputBattery unavailable;
    check(second.applySourceBattery(6u, b, 7u, unavailable) ==
              VirtualXInputResult::Success &&
              second.getBattery(0u, queriedBatB) ==
                  VirtualXInputResult::Success &&
              !queriedBatB.battery.available,
          "battery-unavailable is explicit without disconnecting the source");
}

void testDisconnectReconnectAndReset() {
    VirtualXInputContext context;
    const auto a = source(5005u, 2u);
    check(context.mapLogicalSlot(1u, 0u, a, 10u) ==
              VirtualXInputResult::Success &&
              context.applySourceState(2u, a, 10u, stateA()) ==
                  VirtualXInputResult::Success &&
              context.applySourceCapabilities(
                  3u, a, 10u, capabilitiesA()) ==
                  VirtualXInputResult::Success &&
              context.applySourceBattery(4u, a, 10u, batteryA()) ==
                  VirtualXInputResult::Success,
          "connected source has state, capabilities, and battery");
    VirtualXInputMapping mapping;
    check(context.getMapping(0u, mapping) == VirtualXInputResult::Success,
          "connected mapping is available before disconnect");
    VirtualXInputVibrationRequest vibration{
        0u, 1u, 2u, mapping.mappingGeneration, 10u};
    VirtualXInputVibrationRoute route;
    check(context.routeVibration(5u, vibration, route) ==
              VirtualXInputResult::Success &&
              context.disconnectSource(6u, a, 10u) ==
                  VirtualXInputResult::Success,
          "disconnect follows a routed vibration command");
    VirtualXInputState state;
    VirtualXInputCapabilities caps;
    VirtualXInputBattery battery;
    check(context.getState(0u, state) ==
              VirtualXInputResult::Disconnected && !state.connected &&
              state.gamepad == NormalizedXInputGamepad{} &&
              context.getCapabilities(0u, caps) ==
                  VirtualXInputResult::Disconnected &&
              context.getBattery(0u, battery) ==
                  VirtualXInputResult::Disconnected &&
              context.getLastVibration(0u, route) ==
                  VirtualXInputResult::Disconnected,
          "disconnect clears buttons, triggers, axes, metadata, and vibration intent");
    check(context.routeVibration(7u, vibration, route) ==
              VirtualXInputResult::Disconnected,
          "disconnected vibration is rejected without reaching a source");
    check(context.applySourceState(7u, a, 10u, stateA()) ==
              VirtualXInputResult::StaleGeneration,
          "same-generation delayed state cannot reconnect");
    check(context.applySourceState(7u, a, 11u, stateB()) ==
              VirtualXInputResult::Success &&
              context.getState(0u, state) == VirtualXInputResult::Success &&
              state.mapping.sourceGeneration == 11u &&
              state.gamepad == stateB(),
          "reconnect requires and records a newer generation");
    vibration.expectedSourceGeneration = 10u;
    check(context.routeVibration(8u, vibration, route) ==
              VirtualXInputResult::StaleGeneration,
          "pre-reconnect vibration cannot reach the new connection");
    check(context.applySourceState(7u, a, 11u, stateB()) ==
              VirtualXInputResult::StaleSequence,
          "duplicate controller sequence is rejected");

    context.reset();
    check(context.lastAppliedSequence() == 0u &&
              context.getState(0u, state) ==
                  VirtualXInputResult::NotMapped,
          "reset removes all mappings and controller state deterministically");
}

void testExplicitRemapGenerationInvariants() {
    VirtualXInputContext context;
    const auto hint0 = source(6006u, 0u);
    const auto hint1 = source(6006u, 1u);
    const auto hint2 = source(6006u, 2u);
    check(context.mapLogicalSlot(1u, 0u, hint0, 10u) ==
              VirtualXInputResult::Success &&
              context.applySourceState(2u, hint0, 10u, stateA()) ==
                  VirtualXInputResult::Success &&
              context.applySourceCapabilities(
                  3u, hint0, 10u, capabilitiesA()) ==
                  VirtualXInputResult::Success,
          "same-source remap fixture starts connected at generation 10");
    VirtualXInputMapping original;
    check(context.getMapping(0u, original) == VirtualXInputResult::Success,
          "original mapping is queryable");
    VirtualXInputVibrationRequest originalRequest{
        0u, 10u, 20u, original.mappingGeneration, 10u};
    VirtualXInputVibrationRoute originalRoute;
    check(context.routeVibration(4u, originalRequest, originalRoute) ==
              VirtualXInputResult::Success,
          "pre-remap vibration route is established");

    check(context.mapLogicalSlot(5u, 0u, hint1, 9u) ==
              VirtualXInputResult::StaleGeneration &&
              context.lastAppliedSequence() == 4u,
          "explicit hint remap cannot resurrect a stale generation or consume sequence");
    VirtualXInputMapping unchanged;
    VirtualXInputState queried;
    VirtualXInputVibrationRoute retainedRoute;
    check(context.getMapping(0u, unchanged) == VirtualXInputResult::Success &&
              unchanged == original &&
              context.getState(0u, queried) == VirtualXInputResult::Success &&
              queried.gamepad == stateA() &&
              context.getLastVibration(0u, retainedRoute) ==
                  VirtualXInputResult::Success &&
              retainedRoute == originalRoute,
          "rejected stale remap leaves mapping, state, and vibration unchanged");

    check(context.mapLogicalSlot(5u, 0u, hint1, 10u) ==
              VirtualXInputResult::Success,
          "same-generation explicit hint remap is accepted while connected");
    VirtualXInputMapping sameGenerationRemap;
    check(context.getMapping(0u, sameGenerationRemap) ==
              VirtualXInputResult::Success &&
              sameGenerationRemap.source == hint1 &&
              sameGenerationRemap.sourceGeneration == 10u &&
              sameGenerationRemap.mappingGeneration ==
                  original.mappingGeneration + 1u &&
              context.getState(0u, queried) ==
                  VirtualXInputResult::Disconnected &&
              queried.gamepad == NormalizedXInputGamepad{} &&
              context.getLastVibration(0u, retainedRoute) ==
                  VirtualXInputResult::Disconnected,
          "accepted hint remap advances mapping generation and clears connection state");
    check(context.routeVibration(6u, originalRequest, retainedRoute) ==
              VirtualXInputResult::Disconnected,
          "pre-remap vibration cannot route after same-generation metadata remap");

    check(context.applySourceState(6u, hint1, 10u, stateB()) ==
              VirtualXInputResult::Success &&
              context.mapLogicalSlot(7u, 0u, hint2, 11u) ==
                  VirtualXInputResult::Success,
          "same stable source may explicitly remap to a newer generation");
    VirtualXInputMapping newerRemap;
    check(context.getMapping(0u, newerRemap) ==
              VirtualXInputResult::Success &&
              newerRemap.source == hint2 &&
              newerRemap.sourceGeneration == 11u &&
              newerRemap.mappingGeneration ==
                  sameGenerationRemap.mappingGeneration + 1u &&
              context.applySourceState(8u, hint2, 10u, stateA()) ==
                  VirtualXInputResult::StaleGeneration &&
              context.lastAppliedSequence() == 7u,
          "newer explicit remap makes generation-10 updates stale without consuming sequence");

    VirtualXInputVibrationRequest generation10Request{
        0u, 30u, 40u, sameGenerationRemap.mappingGeneration, 10u};
    check(context.applySourceState(8u, hint2, 11u, stateA()) ==
              VirtualXInputResult::Success &&
              context.routeVibration(9u, generation10Request,
                                     retainedRoute) ==
                  VirtualXInputResult::MappingGenerationMismatch &&
              context.lastAppliedSequence() == 8u &&
              context.disconnectSource(9u, hint2, 11u) ==
                  VirtualXInputResult::Success &&
              context.mapLogicalSlot(10u, 0u, hint1, 11u) ==
                  VirtualXInputResult::StaleGeneration &&
              context.lastAppliedSequence() == 9u,
          "disconnect requires a newer generation even through a changed hint remap");
    check(context.mapLogicalSlot(10u, 0u, hint1, 12u) ==
              VirtualXInputResult::Success,
          "post-disconnect explicit remap accepts the required newer generation");

    const auto different = source(7007u, 3u);
    VirtualXInputMapping beforeDifferent;
    check(context.getMapping(0u, beforeDifferent) ==
              VirtualXInputResult::Success &&
              context.mapLogicalSlot(11u, 0u, different, 1u) ==
                  VirtualXInputResult::Success,
          "a genuinely different stable source has an independent generation namespace");
    VirtualXInputMapping afterDifferent;
    check(context.getMapping(0u, afterDifferent) ==
              VirtualXInputResult::Success &&
              afterDifferent.source == different &&
              afterDifferent.sourceGeneration == 1u &&
              afterDifferent.mappingGeneration ==
                  beforeDifferent.mappingGeneration + 1u &&
              context.getState(0u, queried) ==
                  VirtualXInputResult::Disconnected &&
              queried.gamepad == NormalizedXInputGamepad{},
          "different-source remap advances mapping generation and clears old state");
}

void testAtomicSeatSnapshotConsistency() {
    VirtualXInputContext context;
    const auto original = source(8080u, 0u);
    const auto rebound = source(8080u, 1u);
    auto caps = capabilitiesA();
    auto battery = batteryA();
    check(context.mapLogicalSlot(1u, 0u, original, 1u) ==
              VirtualXInputResult::Success &&
              context.applySourceSnapshot(
                  2u, original, 1u, stateA(), &caps, &battery) ==
                  VirtualXInputResult::Success,
          "one atomic Seat snapshot publishes state, capabilities, and battery");

    VirtualXInputState state;
    VirtualXInputCapabilities queriedCaps;
    VirtualXInputBattery queriedBattery;
    VirtualXInputMapping firstMapping;
    check(context.getState(0u, state) == VirtualXInputResult::Success &&
              context.getCapabilities(0u, queriedCaps) ==
                  VirtualXInputResult::Success &&
              context.getBattery(0u, queriedBattery) ==
                  VirtualXInputResult::Success &&
              context.getMapping(0u, firstMapping) ==
                  VirtualXInputResult::Success &&
              state.mapping == firstMapping && queriedCaps.mapping == firstMapping &&
              queriedBattery.mapping == firstMapping,
          "all supported metadata observes exactly the state mapping generation");

    const NormalizedXInputGamepad neutral{};
    check(context.applySourceSnapshot(
              3u, original, 1u, neutral, nullptr, nullptr) ==
              VirtualXInputResult::Success &&
              context.getState(0u, state) == VirtualXInputResult::Success &&
              state.connected && state.gamepad == neutral &&
              context.getCapabilities(0u, queriedCaps) ==
                  VirtualXInputResult::Disconnected &&
              context.getBattery(0u, queriedBattery) ==
                  VirtualXInputResult::Disconnected,
          "connected neutral state remains success while unavailable metadata fails explicitly");

    auto reboundCaps = capabilitiesA();
    reboundCaps.subtype = 2u;
    NormalizedXInputBattery reboundBattery{
        true, XInputBatteryDeviceType::Gamepad,
        XInputBatteryType::Wired, XInputBatteryLevel::Medium};
    check(context.applySourceSnapshot(
              4u, rebound, 2u, stateB(), &reboundCaps, &reboundBattery) ==
              VirtualXInputResult::Success,
          "same stable source can atomically rebind runtime slot with a new generation");
    VirtualXInputMapping reboundMapping;
    check(context.getMapping(0u, reboundMapping) == VirtualXInputResult::Success &&
              context.getState(0u, state) == VirtualXInputResult::Success &&
              context.getCapabilities(0u, queriedCaps) ==
                  VirtualXInputResult::Success &&
              context.getBattery(0u, queriedBattery) ==
                  VirtualXInputResult::Success &&
              reboundMapping.source == rebound &&
              reboundMapping.sourceGeneration == 2u &&
              reboundMapping.mappingGeneration == firstMapping.mappingGeneration + 1u &&
              state.mapping == reboundMapping && queriedCaps.mapping == reboundMapping &&
              queriedBattery.mapping == reboundMapping && state.gamepad == stateB() &&
              queriedCaps.capabilities == reboundCaps &&
              queriedBattery.battery == reboundBattery,
          "rebind changes mapping/state/metadata together without a mixed-generation window");
    check(context.applySourceSnapshot(
              5u, original, 1u, stateA(), &caps, &battery) ==
              VirtualXInputResult::StaleGeneration &&
              context.lastAppliedSequence() == 4u,
          "pre-rebind source generation cannot resurrect after the atomic rebind");

    auto invalidCaps = reboundCaps;
    invalidCaps.type = static_cast<XInputCapabilityType>(9u);
    check(context.applySourceSnapshot(
              5u, rebound, 2u, stateA(), &invalidCaps, &reboundBattery) ==
              VirtualXInputResult::InvalidState &&
              context.lastAppliedSequence() == 4u &&
              context.getState(0u, state) == VirtualXInputResult::Success &&
              state.gamepad == stateB(),
          "malformed snapshot metadata fails closed without partially committing state");
}

void testValidationBoundaries() {
    ControllerSourceIdentity invalidSource{};
    invalidSource.sourceKey = 0u;
    check(!validControllerSourceIdentity(invalidSource),
          "zero opaque source key is invalid");
    invalidSource.sourceKey = 1u;
    invalidSource.runtimeXInputSlotHint = 4u;
    check(!validControllerSourceIdentity(invalidSource),
          "runtime XInput slot hint is only 0..3 or unavailable");
    auto caps = capabilitiesA();
    caps.type = static_cast<XInputCapabilityType>(9u);
    check(!validXInputCapabilities(caps),
          "malformed capability type is rejected");
    caps = capabilitiesA();
    caps.vibrationSupported = false;
    check(!validXInputCapabilities(caps),
          "unsupported vibration requires zero motor capability");
    auto battery = batteryA();
    battery.batteryLevel = static_cast<XInputBatteryLevel>(9u);
    check(!validXInputBattery(battery),
          "malformed battery level is rejected");
}

} // namespace

int main() {
    testSlotsMappingAndPacketSemantics();
    testContextIsolationAndExtrema();
    testCapabilitiesBatteryAndVibration();
    testDisconnectReconnectAndReset();
    testExplicitRemapGenerationInvariants();
    testAtomicSeatSnapshotConsistency();
    testValidationBoundaries();
    std::cout << "Virtual XInput state tests passed.\n";
    return EXIT_SUCCESS;
}
