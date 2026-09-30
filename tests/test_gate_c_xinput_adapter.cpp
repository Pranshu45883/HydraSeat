#include "hydra/gate_c_adapter.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

HydraGateCAdapterXInputMappingV4 mapping(
    std::uint64_t sourceKey, std::uint64_t generation,
    std::uint8_t runtimeSlot) {
    HydraGateCAdapterXInputMappingV4 value{};
    value.struct_size = sizeof(value);
    value.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    value.source_kind = HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_SYNTHETIC;
    value.runtime_xinput_slot_hint = runtimeSlot;
    value.source_key = sourceKey;
    value.source_generation = generation;
    return value;
}

HydraGateCAdapterXInputSourceStateV4 sourceState(
    std::uint64_t sourceKey, std::uint64_t generation,
    std::uint16_t buttons) {
    HydraGateCAdapterXInputSourceStateV4 value{};
    value.struct_size = sizeof(value);
    value.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    value.source_kind = HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_SYNTHETIC;
    value.runtime_xinput_slot_hint = 0u;
    value.source_key = sourceKey;
    value.source_generation = generation;
    value.buttons = buttons;
    value.left_trigger = 20u;
    value.right_trigger = 200u;
    value.thumb_lx = -12000;
    value.thumb_ly = 9000;
    return value;
}

HydraGateCAdapterXInputSourceCapabilitiesV4 capabilities(
    std::uint64_t sourceKey, std::uint64_t generation) {
    HydraGateCAdapterXInputSourceCapabilitiesV4 value{};
    value.struct_size = sizeof(value);
    value.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    value.source_kind = HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_SYNTHETIC;
    value.runtime_xinput_slot_hint = 0u;
    value.type = HYDRA_GATE_C_ADAPTER_XINPUT_CAPABILITY_GAMEPAD;
    value.subtype = 1u;
    value.vibration_supported = 1u;
    value.source_key = sourceKey;
    value.source_generation = generation;
    value.buttons = 0xffffu;
    value.left_trigger = 255u;
    value.right_trigger = 255u;
    value.thumb_lx = 32767;
    value.thumb_ly = 32767;
    value.thumb_rx = 32767;
    value.thumb_ry = 32767;
    value.left_motor_maximum = 65535u;
    value.right_motor_maximum = 65535u;
    return value;
}

HydraGateCAdapterXInputSourceBatteryV4 battery(
    std::uint64_t sourceKey, std::uint64_t generation,
    std::uint8_t level) {
    HydraGateCAdapterXInputSourceBatteryV4 value{};
    value.struct_size = sizeof(value);
    value.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    value.source_kind = HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_SYNTHETIC;
    value.runtime_xinput_slot_hint = 0u;
    value.available = 1u;
    value.device_type = 0u;
    value.battery_type = 2u;
    value.battery_level = level;
    value.source_key = sourceKey;
    value.source_generation = generation;
    return value;
}

void testAbiMigrationAndIndependentContexts() {
    check(hydra_gate_c_adapter_api_version() == 4u,
          "controller ABI addition advances the adapter to v4");
    check(sizeof(HydraGateCAdapterXInputSourceV4) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_V4_BYTES &&
              sizeof(HydraGateCAdapterXInputMappingV4) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_MAPPING_V4_BYTES &&
              sizeof(HydraGateCAdapterXInputSourceStateV4) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_STATE_V4_BYTES &&
              sizeof(HydraGateCAdapterXInputStateV4) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_STATE_V4_BYTES &&
              sizeof(HydraGateCAdapterXInputSourceCapabilitiesV4) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_CAPABILITIES_V4_BYTES &&
              sizeof(HydraGateCAdapterXInputCapabilitiesV4) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_CAPABILITIES_V4_BYTES &&
              sizeof(HydraGateCAdapterXInputSourceBatteryV4) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_BATTERY_V4_BYTES &&
              sizeof(HydraGateCAdapterXInputBatteryV4) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_BATTERY_V4_BYTES &&
              sizeof(HydraGateCAdapterXInputVibrationV4) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_VIBRATION_V4_BYTES &&
              alignof(HydraGateCAdapterXInputStateV4) == 1u &&
              alignof(HydraGateCAdapterXInputVibrationV4) == 1u,
          "Gate C XInput public layouts stay packed and fixed-width on x86 and x64");
    auto first = hydra_gate_c_adapter_create();
    auto second = hydra_gate_c_adapter_create();
    check(first != nullptr && second != nullptr,
          "two adapter contexts are created");

    auto firstMapping = mapping(0xaaaau, 10u, 0u);
    auto secondMapping = mapping(0xbbbbu, 20u, 1u);
    check(hydra_gate_c_adapter_xinput_map_slot(
              first, 1u, &firstMapping) == HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_map_slot(
                  second, 1u, &secondMapping) ==
                  HYDRA_GATE_C_ADAPTER_OK,
          "both process-local contexts map logical slot 0 independently");
    constexpr std::uint16_t kOrdinalCompatibleExtendedButton = 0x0400u;
    auto firstState = sourceState(
        0xaaaau, 10u,
        static_cast<std::uint16_t>(0x1100u | kOrdinalCompatibleExtendedButton));
    auto secondState = sourceState(0xbbbbu, 20u, 0x2200u);
    secondState.runtime_xinput_slot_hint = 1u;
    secondState.left_trigger = 180u;
    secondState.right_trigger = 5u;
    check(hydra_gate_c_adapter_xinput_apply_state(
              first, 2u, &firstState) == HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_apply_state(
                  second, 2u, &secondState) ==
                  HYDRA_GATE_C_ADAPTER_OK,
          "distinct normalized controller states apply through C ABI v4");

    HydraGateCAdapterXInputStateV4 firstQuery{};
    firstQuery.struct_size = sizeof(firstQuery);
    firstQuery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    HydraGateCAdapterXInputStateV4 secondQuery = firstQuery;
    check(hydra_gate_c_adapter_xinput_get_state(
              first, 0u, &firstQuery) == HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_get_state(
                  second, 0u, &secondQuery) == HYDRA_GATE_C_ADAPTER_OK &&
              firstQuery.source_key == 0xaaaau &&
              secondQuery.source_key == 0xbbbbu &&
              firstQuery.buttons ==
                  static_cast<std::uint16_t>(0x1100u |
                                             kOrdinalCompatibleExtendedButton) &&
              secondQuery.buttons == 0x2200u,
          "C ABI queries preserve process-local source separation and ordinal-compatible extended state bits");

    auto firstCaps = capabilities(0xaaaau, 10u);
    auto secondCaps = capabilities(0xbbbbu, 20u);
    secondCaps.runtime_xinput_slot_hint = 1u;
    secondCaps.subtype = 2u;
    auto firstBattery = battery(0xaaaau, 10u, 3u);
    auto secondBattery = battery(0xbbbbu, 20u, 1u);
    secondBattery.runtime_xinput_slot_hint = 1u;
    check(hydra_gate_c_adapter_xinput_apply_capabilities(
              first, 3u, &firstCaps) == HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_apply_capabilities(
                  second, 3u, &secondCaps) ==
                  HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_apply_battery(
                  first, 4u, &firstBattery) ==
                  HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_apply_battery(
                  second, 4u, &secondBattery) ==
                  HYDRA_GATE_C_ADAPTER_OK,
          "capabilities and battery apply independently through C ABI v4");
    HydraGateCAdapterXInputCapabilitiesV4 queriedCaps{};
    queriedCaps.struct_size = sizeof(queriedCaps);
    queriedCaps.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    HydraGateCAdapterXInputCapabilitiesV4 secondQueriedCaps = queriedCaps;
    HydraGateCAdapterXInputBatteryV4 queriedBattery{};
    queriedBattery.struct_size = sizeof(queriedBattery);
    queriedBattery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    HydraGateCAdapterXInputBatteryV4 secondQueriedBattery = queriedBattery;
    check(hydra_gate_c_adapter_xinput_get_capabilities(
              first, 0u, &queriedCaps) == HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_get_capabilities(
                  second, 0u, &secondQueriedCaps) ==
                  HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_get_battery(
                  first, 0u, &queriedBattery) ==
                  HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_get_battery(
                  second, 0u, &secondQueriedBattery) ==
                  HYDRA_GATE_C_ADAPTER_OK &&
              queriedCaps.source_key == firstQuery.source_key &&
              queriedCaps.source_generation == firstQuery.source_generation &&
              queriedCaps.mapping_generation == firstQuery.mapping_generation &&
              queriedCaps.subtype == 1u &&
              queriedBattery.source_key == firstQuery.source_key &&
              queriedBattery.source_generation == firstQuery.source_generation &&
              queriedBattery.mapping_generation == firstQuery.mapping_generation &&
              queriedBattery.battery_level == 3u &&
              secondQueriedCaps.source_key == secondQuery.source_key &&
              secondQueriedCaps.source_generation == secondQuery.source_generation &&
              secondQueriedCaps.mapping_generation == secondQuery.mapping_generation &&
              secondQueriedCaps.subtype == 2u &&
              secondQueriedBattery.source_key == secondQuery.source_key &&
              secondQueriedBattery.source_generation == secondQuery.source_generation &&
              secondQueriedBattery.mapping_generation == secondQuery.mapping_generation &&
              secondQueriedBattery.battery_level == 1u,
          "capabilities and battery queries preserve the same identity and generation as state in each Seat context");

    HydraGateCAdapterXInputVibrationV4 vibration{};
    vibration.struct_size = sizeof(vibration);
    vibration.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    vibration.logical_slot = 0u;
    vibration.source_generation = firstQuery.source_generation;
    vibration.mapping_generation = firstQuery.mapping_generation;
    vibration.left_motor = 123u;
    vibration.right_motor = 456u;
    check(hydra_gate_c_adapter_xinput_route_vibration(
              first, 5u, &vibration) == HYDRA_GATE_C_ADAPTER_OK &&
              vibration.source_key == 0xaaaau &&
              vibration.route_count == 1u,
          "vibration returns only the validated source route descriptor");

    auto stale = firstState;
    stale.source_generation = 9u;
    check(hydra_gate_c_adapter_xinput_apply_state(
              first, 6u, &stale) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_STALE_GENERATION,
          "stale source generation fails closed at the C boundary");
    auto staleRemap = mapping(0xaaaau, 9u, 1u);
    check(hydra_gate_c_adapter_xinput_map_slot(
              first, 6u, &staleRemap) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_STALE_GENERATION,
          "explicit C ABI hint remap cannot resurrect a stale stable source");
    HydraGateCAdapterXInputStateV4 afterRejectedRemap{};
    afterRejectedRemap.struct_size = sizeof(afterRejectedRemap);
    afterRejectedRemap.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    check(hydra_gate_c_adapter_xinput_get_state(
              first, 0u, &afterRejectedRemap) ==
                  HYDRA_GATE_C_ADAPTER_OK &&
              afterRejectedRemap.runtime_xinput_slot_hint == 0u &&
              afterRejectedRemap.source_generation == 10u &&
              afterRejectedRemap.mapping_generation ==
                  firstQuery.mapping_generation &&
              afterRejectedRemap.buttons ==
                  static_cast<std::uint16_t>(0x1100u |
                                             kOrdinalCompatibleExtendedButton),
          "rejected C ABI remap preserves mapping metadata and controller state");
    HydraGateCAdapterXInputSourceV4 disconnect{};
    disconnect.struct_size = sizeof(disconnect);
    disconnect.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    disconnect.source_kind = HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_SYNTHETIC;
    disconnect.runtime_xinput_slot_hint = 0u;
    disconnect.source_key = 0xaaaau;
    disconnect.source_generation = 10u;
    check(hydra_gate_c_adapter_xinput_disconnect(
              first, 6u, &disconnect) == HYDRA_GATE_C_ADAPTER_OK,
          "disconnect clears the mapped source at C ABI boundary");
    firstQuery = {};
    firstQuery.struct_size = sizeof(firstQuery);
    firstQuery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    check(hydra_gate_c_adapter_xinput_get_state(
              first, 0u, &firstQuery) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_DISCONNECTED &&
              firstQuery.connected == 0u && firstQuery.buttons == 0u &&
              firstQuery.left_trigger == 0u &&
              firstQuery.thumb_lx == 0,
          "disconnected C ABI query returns no stale controller state");

    auto reconnectedState = sourceState(0xaaaau, 11u, 0x4400u);
    check(hydra_gate_c_adapter_xinput_apply_state(
              first, 7u, &reconnectedState) == HYDRA_GATE_C_ADAPTER_OK,
          "reconnect requires and accepts a newer source generation");
    firstQuery = {};
    firstQuery.struct_size = sizeof(firstQuery);
    firstQuery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    queriedCaps = {};
    queriedCaps.struct_size = sizeof(queriedCaps);
    queriedCaps.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    queriedBattery = {};
    queriedBattery.struct_size = sizeof(queriedBattery);
    queriedBattery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    check(hydra_gate_c_adapter_xinput_get_state(
              first, 0u, &firstQuery) == HYDRA_GATE_C_ADAPTER_OK &&
              firstQuery.source_generation == 11u &&
              firstQuery.buttons == 0x4400u &&
              hydra_gate_c_adapter_xinput_get_capabilities(
                  first, 0u, &queriedCaps) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_DISCONNECTED &&
              hydra_gate_c_adapter_xinput_get_battery(
                  first, 0u, &queriedBattery) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_DISCONNECTED &&
              queriedCaps.source_key == 0u && queriedBattery.source_key == 0u,
          "new state generation cannot expose stale capability or battery payloads before same-generation metadata arrives");

    auto reconnectedCaps = capabilities(0xaaaau, 11u);
    reconnectedCaps.subtype = 3u;
    auto reconnectedBattery = battery(0xaaaau, 11u, 2u);
    check(hydra_gate_c_adapter_xinput_apply_capabilities(
              first, 8u, &reconnectedCaps) == HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_apply_battery(
                  first, 9u, &reconnectedBattery) == HYDRA_GATE_C_ADAPTER_OK,
          "same-generation reconnect metadata is accepted after state");
    queriedCaps = {};
    queriedCaps.struct_size = sizeof(queriedCaps);
    queriedCaps.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    queriedBattery = {};
    queriedBattery.struct_size = sizeof(queriedBattery);
    queriedBattery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    check(hydra_gate_c_adapter_xinput_get_capabilities(
              first, 0u, &queriedCaps) == HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_get_battery(
                  first, 0u, &queriedBattery) == HYDRA_GATE_C_ADAPTER_OK &&
              queriedCaps.source_key == firstQuery.source_key &&
              queriedCaps.source_generation == firstQuery.source_generation &&
              queriedCaps.mapping_generation == firstQuery.mapping_generation &&
              queriedCaps.subtype == 3u &&
              queriedBattery.source_key == firstQuery.source_key &&
              queriedBattery.source_generation == firstQuery.source_generation &&
              queriedBattery.mapping_generation == firstQuery.mapping_generation &&
              queriedBattery.battery_level == 2u,
          "reconnect state, capabilities, and battery converge on one coherent generation");

    auto staleCaps = firstCaps;
    staleCaps.source_generation = 10u;
    HydraGateCAdapterXInputVibrationV4 staleVibration{};
    staleVibration.struct_size = sizeof(staleVibration);
    staleVibration.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    staleVibration.logical_slot = 0u;
    staleVibration.source_generation = 10u;
    staleVibration.mapping_generation = firstQuery.mapping_generation;
    staleVibration.left_motor = 1u;
    staleVibration.right_motor = 2u;
    check(hydra_gate_c_adapter_xinput_apply_capabilities(
              first, 10u, &staleCaps) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_STALE_GENERATION &&
              hydra_gate_c_adapter_xinput_route_vibration(
                  first, 10u, &staleVibration) ==
                  HYDRA_GATE_C_ADAPTER_XINPUT_STALE_GENERATION,
          "stale reconnect metadata and rumble cannot target the current controller generation");

    secondQuery = {};
    secondQuery.struct_size = sizeof(secondQuery);
    secondQuery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    secondQueriedCaps = {};
    secondQueriedCaps.struct_size = sizeof(secondQueriedCaps);
    secondQueriedCaps.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    secondQueriedBattery = {};
    secondQueriedBattery.struct_size = sizeof(secondQueriedBattery);
    secondQueriedBattery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    check(hydra_gate_c_adapter_xinput_get_state(
              second, 0u, &secondQuery) == HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_get_capabilities(
                  second, 0u, &secondQueriedCaps) ==
                  HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_xinput_get_battery(
                  second, 0u, &secondQueriedBattery) ==
                  HYDRA_GATE_C_ADAPTER_OK &&
              secondQuery.source_key == 0xbbbbu &&
              secondQuery.source_generation == 20u &&
              secondQuery.buttons == 0x2200u &&
              secondQueriedCaps.source_generation == 20u &&
              secondQueriedCaps.subtype == 2u &&
              secondQueriedBattery.source_generation == 20u &&
              secondQueriedBattery.battery_level == 1u,
          "Seat A disconnect and reconnect never mutate Seat B controller generation or metadata");

    check(hydra_gate_c_adapter_reset(second) == HYDRA_GATE_C_ADAPTER_OK,
          "adapter reset succeeds with controller state present");
    secondQuery = {};
    secondQuery.struct_size = sizeof(secondQuery);
    secondQuery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    check(hydra_gate_c_adapter_xinput_get_state(
              second, 0u, &secondQuery) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_DISCONNECTED,
          "adapter reset clears all controller mappings");

    auto malformed = mapping(0xccccu, 1u, 0u);
    malformed.api_version = 3u;
    check(hydra_gate_c_adapter_xinput_map_slot(
              first, 7u, &malformed) ==
              HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH,
          "old adapter ABI version fails closed");
    malformed = mapping(0xccccu, 1u, 0u);
    malformed.struct_size -= 1u;
    check(hydra_gate_c_adapter_xinput_map_slot(
              first, 7u, &malformed) ==
              HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH,
          "malformed controller struct size fails closed");
    malformed = mapping(0xccccu, 12u, 0u);
    malformed.source_kind = 0xffu;
    check(hydra_gate_c_adapter_xinput_map_slot(
              first, 10u, &malformed) == HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT,
          "unknown controller source kind fails closed");
    auto malformedCaps = reconnectedCaps;
    malformedCaps.type = 0xffu;
    auto malformedBattery = reconnectedBattery;
    malformedBattery.battery_level = 0xffu;
    check(hydra_gate_c_adapter_xinput_apply_capabilities(
              first, 10u, &malformedCaps) ==
                  HYDRA_GATE_C_ADAPTER_INVALID_STATE &&
              hydra_gate_c_adapter_xinput_apply_battery(
                  first, 10u, &malformedBattery) ==
                  HYDRA_GATE_C_ADAPTER_INVALID_STATE,
          "unknown capability and battery enum values fail closed without consuming sequence");
    auto malformedState = reconnectedState;
    malformedState.reserved0[0] = 1u;
    check(hydra_gate_c_adapter_xinput_apply_state(
              first, 10u, &malformedState) == HYDRA_GATE_C_ADAPTER_INVALID_STATE,
          "nonzero bounded-protocol reserved bytes fail closed");
    firstQuery = {};
    firstQuery.struct_size = sizeof(firstQuery);
    firstQuery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    check(hydra_gate_c_adapter_xinput_get_state(
              first, 4u, &firstQuery) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_INVALID_SLOT,
          "C ABI rejects logical slot 4");

    hydra_gate_c_adapter_destroy(first);
    hydra_gate_c_adapter_destroy(second);
}

} // namespace

int main() {
    testAbiMigrationAndIndependentContexts();
    std::cout << "Gate C XInput adapter ABI tests passed.\n";
    return EXIT_SUCCESS;
}
