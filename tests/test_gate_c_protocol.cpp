#include "hydra/gate_c_protocol.hpp"
#include "hydra/gate_c_transport.hpp"
#include "hydra/virtual_input_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

hydra::gatec::DecodedFrame decodeOrFail(
    const std::vector<std::byte>& bytes, std::string_view message) {
    const auto decoded = hydra::gatec::decodeFrame(bytes);
    check(static_cast<bool>(decoded), message);
    return *decoded.frame;
}

void testProtocolRoundTrips() {
    using namespace hydra::gatec;

    const auto rawCapabilities = testCapabilityBits(
        kControlledRawInputProbeCapabilities);
    check((rawCapabilities & testCapabilityBits(
               TestCapability::RawInputApiShim)) != 0 &&
              (rawCapabilities & testCapabilityBits(
               TestCapability::PollingApiShim)) != 0 &&
              (rawCapabilities & testCapabilityBits(
               TestCapability::CursorFocusApiShim)) == 0,
          "Raw Input probe capability is explicit and preserves polling without enabling cursor/focus");

    HelloMessage hello;
    for (std::size_t index = 0; index < hello.token.size(); ++index) {
        hello.token[index] = static_cast<std::uint8_t>(index * 7 + 3);
    }
    hello.seatId = 2;
    hello.processId = 12345;
    hello.architectureBits = 64;
    hello.targetWindow = 0x123456789abcdef0ull;
    const auto helloFrame = decodeOrFail(
        encodeHello(11, hello), "Hello frame decodes");
    HelloMessage decodedHello;
    std::string error;
    check(decodeHello(helloFrame, decodedHello, &error) &&
              decodedHello == hello && helloFrame.sequence == 11,
          "Hello payload round-trips exactly");

    HelloAckMessage ack;
    ack.accepted = true;
    ack.serverProcessId = 9876;
    ack.grantedCapabilities = 0x445566778899aabbull;
    ack.errorCode = 0;
    const auto ackFrame = decodeOrFail(
        encodeHelloAck(12, ack), "HelloAck frame decodes");
    HelloAckMessage decodedAck;
    check(decodeHelloAck(ackFrame, decodedAck, &error) &&
              decodedAck == ack,
          "HelloAck payload round-trips exactly");

    InputEventMessage input;
    input.kind = InputKind::Keyboard;
    input.keyTransition = KeyTransition::Down;
    input.timestampMicros = 55555;
    input.vkey = 0x41;
    input.scanCode = 0x1e;
    input.keyboardFlags = 2;
    const auto inputFrame = decodeOrFail(
        encodeInputEvent(13, input), "Input frame decodes");
    InputEventMessage decodedInput;
    check(decodeInputEvent(inputFrame, decodedInput, &error) &&
              decodedInput == input,
          "Input payload round-trips exactly");

    ControlStateMessage control;
    control.cursorX = 900;
    control.cursorY = -12;
    control.clipEnabled = true;
    control.virtualForeground = true;
    control.virtualCapture = true;
    control.clipLeft = 10;
    control.clipTop = 20;
    control.clipRight = 800;
    control.clipBottom = 600;
    const auto controlFrame = decodeOrFail(
        encodeControlState(14, control), "Control frame decodes");
    ControlStateMessage decodedControl;
    check(decodeControlState(controlFrame, decodedControl, &error) &&
              decodedControl == control,
          "Control payload round-trips exactly");

    StateSnapshotMessage snapshot;
    snapshot.lastAppliedSequence = 91;
    snapshot.keyDownBits[8] = 0x02;
    snapshot.keyPressedEdgeBits[8] = 0x02;
    snapshot.mouseButtonsDown = 5;
    snapshot.wheelAccumulator = -240;
    snapshot.probeVkey = 0x41;
    snapshot.asyncKeyStateValue = 0x8001u;
    snapshot.keyboardStateByte = 0x80u;
    snapshot.cursorX = 123;
    snapshot.cursorY = 456;
    snapshot.clipEnabled = true;
    snapshot.virtualForeground = true;
    snapshot.virtualCapture = false;
    snapshot.clipLeft = 0;
    snapshot.clipTop = 0;
    snapshot.clipRight = 1920;
    snapshot.clipBottom = 1080;
    const auto snapshotFrame = decodeOrFail(
        encodeStateSnapshot(15, snapshot), "Snapshot frame decodes");
    StateSnapshotMessage decodedSnapshot;
    check(decodeStateSnapshot(snapshotFrame, decodedSnapshot, &error) &&
              decodedSnapshot == snapshot,
          "Snapshot payload round-trips exactly");

    QuerySnapshotMessage query;
    query.probeVkey = 0x41;
    QuerySnapshotMessage decodedQuery;
    check(decodeQuerySnapshot(
              decodeOrFail(encodeQuerySnapshot(16, query),
                           "QuerySnapshot frame decodes"),
              decodedQuery, &error) && decodedQuery == query,
          "QuerySnapshot probe payload round-trips exactly");
    check(decodeShutdown(
              decodeOrFail(encodeShutdown(17), "Shutdown frame decodes"),
              &error),
          "Shutdown requires an empty payload");

    ErrorMessage protocolError{77};
    const auto errorFrame = decodeOrFail(
        encodeError(18, protocolError), "Error frame decodes");
    ErrorMessage decodedProtocolError;
    check(decodeError(errorFrame, decodedProtocolError, &error) &&
              decodedProtocolError == protocolError,
          "Error payload round-trips exactly");
}

void testProtocolRejectsMalformedFrames() {
    using namespace hydra::gatec;

    auto bytes = encodeQuerySnapshot(1);
    bytes[0] = std::byte{0};
    check(!decodeFrame(bytes), "wrong protocol magic is rejected");

    bytes = encodeQuerySnapshot(1);
    bytes[4] = std::byte{2};
    check(!decodeFrame(bytes), "unsupported protocol version is rejected");

    bytes = encodeQuerySnapshot(1);
    bytes.push_back(std::byte{0});
    check(!decodeFrame(bytes), "frame size mismatch is rejected");

    bytes = encodeHello(2, HelloMessage{});
    auto frame = decodeOrFail(bytes, "baseline Hello decodes");
    HelloMessage message;
    std::string error;
    check(!decodeHello(frame, message, &error),
          "invalid Hello architecture is rejected");

    InputEventMessage input;
    input.kind = InputKind::Keyboard;
    input.keyTransition = KeyTransition::Down;
    input.vkey = 300;
    frame = decodeOrFail(encodeInputEvent(3, input),
                         "out-of-range key frame decodes structurally");
    check(!decodeInputEvent(frame, input, &error),
          "out-of-range virtual key is rejected semantically");

    input = {};
    input.kind = InputKind::Keyboard;
    input.keyTransition = KeyTransition::None;
    input.vkey = 0x41;
    frame = decodeOrFail(encodeInputEvent(31, input),
                         "transitionless key frame decodes structurally");
    check(!decodeInputEvent(frame, input, &error),
          "keyboard input without a down/up transition is rejected");

    input = {};
    input.kind = InputKind::Mouse;
    input.keyTransition = KeyTransition::Down;
    frame = decodeOrFail(encodeInputEvent(32, input),
                         "mouse key-transition frame decodes structurally");
    check(!decodeInputEvent(frame, input, &error),
          "mouse input carrying a key transition is rejected");

    QuerySnapshotMessage invalidQuery;
    invalidQuery.probeVkey = 300;
    frame = decodeOrFail(encodeQuerySnapshot(33, invalidQuery),
                         "invalid snapshot query decodes structurally");
    check(!decodeQuerySnapshot(frame, invalidQuery, &error),
          "out-of-range snapshot probe is rejected");

    StateSnapshotMessage invalidSnapshot;
    invalidSnapshot.probeVkey = 300;
    frame = decodeOrFail(encodeStateSnapshot(34, invalidSnapshot),
                         "invalid snapshot response decodes structurally");
    check(!decodeStateSnapshot(frame, invalidSnapshot, &error),
          "out-of-range snapshot response probe is rejected");

    ControlStateMessage control;
    control.clipEnabled = true;
    control.clipLeft = 10;
    control.clipRight = 10;
    control.clipTop = 0;
    control.clipBottom = 100;
    frame = decodeOrFail(encodeControlState(4, control),
                         "invalid clip frame decodes structurally");
    check(!decodeControlState(frame, control, &error),
          "invalid clip rectangle is rejected");

    const auto token = tokenFromHex("00112233445566778899aabbccddeeff");
    check(token.has_value() &&
              tokenToHex(*token) == "00112233445566778899aabbccddeeff",
          "session token hex conversion round-trips");
    check(!tokenFromHex("0011"), "short session token is rejected");
    check(!tokenFromHex("00112233445566778899aabbccddeefg"),
          "non-hex session token is rejected");
}

void testControllerProtocol() {
    using namespace hydra::gatec;

    ControllerUpdateMessage mapping;
    mapping.seatId = 1u;
    mapping.kind = ControllerUpdateKind::Map;
    mapping.logicalSlot = 0u;
    mapping.source = {ControllerSourceKind::Synthetic, 0u, 0x1111u};
    mapping.sourceGeneration = 5u;
    std::string error;
    ControllerUpdateMessage decodedUpdate;
    auto frame = decodeOrFail(
        encodeControllerUpdate(20u, mapping),
        "controller mapping frame decodes");
    check(decodeControllerUpdate(frame, decodedUpdate, &error) &&
              decodedUpdate == mapping && frame.sequence == 20u,
          "controller mapping round-trips with fixed-width source identity");

    auto state = mapping;
    state.kind = ControllerUpdateKind::State;
    state.gamepad.buttons = 0x1100u;
    state.gamepad.leftTrigger = 20u;
    state.gamepad.rightTrigger = 200u;
    state.gamepad.thumbLX = -12000;
    state.gamepad.thumbLY = 9000;
    frame = decodeOrFail(encodeControllerUpdate(21u, state),
                         "controller state frame decodes");
    check(decodeControllerUpdate(frame, decodedUpdate, &error) &&
              decodedUpdate == state,
          "normalized controller state round-trips without raw XINPUT structs");

    auto capabilities = mapping;
    capabilities.kind = ControllerUpdateKind::Capabilities;
    capabilities.capabilities.subtype = 1u;
    capabilities.capabilities.flags = 2u;
    capabilities.capabilities.gamepad.buttons = 0xffffu;
    capabilities.capabilities.vibrationSupported = true;
    capabilities.capabilities.leftMotorMaximum = 65535u;
    capabilities.capabilities.rightMotorMaximum = 60000u;
    frame = decodeOrFail(encodeControllerUpdate(22u, capabilities),
                         "controller capabilities frame decodes");
    check(decodeControllerUpdate(frame, decodedUpdate, &error) &&
              decodedUpdate == capabilities,
          "controller capabilities round-trip with explicit vibration support");

    auto battery = mapping;
    battery.kind = ControllerUpdateKind::Battery;
    battery.battery = {true, XInputBatteryDeviceType::Gamepad,
                       XInputBatteryType::Alkaline,
                       XInputBatteryLevel::Full};
    frame = decodeOrFail(encodeControllerUpdate(23u, battery),
                         "controller battery frame decodes");
    check(decodeControllerUpdate(frame, decodedUpdate, &error) &&
              decodedUpdate == battery,
          "controller battery contract round-trips exactly");

    ControllerQueryMessage query;
    query.seatId = 1u;
    query.logicalSlot = 0u;
    ControllerQueryMessage decodedQuery;
    frame = decodeOrFail(encodeControllerQuery(24u, query),
                         "controller snapshot query decodes");
    check(decodeControllerQuery(frame, decodedQuery, &error) &&
              decodedQuery == query,
          "controller snapshot query round-trips");
    query.kind = ControllerQueryKind::Vibration;
    query.expectedMappingGeneration = 2u;
    query.expectedSourceGeneration = 5u;
    query.leftMotor = 123u;
    query.rightMotor = 456u;
    frame = decodeOrFail(encodeControllerQuery(25u, query),
                         "controller vibration query decodes");
    check(decodeControllerQuery(frame, decodedQuery, &error) &&
              decodedQuery == query,
          "controller vibration request carries both generations");

    ControllerSnapshotMessage snapshot;
    snapshot.seatId = 1u;
    snapshot.logicalSlot = 0u;
    snapshot.stateResult = VirtualXInputResult::Success;
    snapshot.capabilitiesResult = VirtualXInputResult::Success;
    snapshot.batteryResult = VirtualXInputResult::Success;
    snapshot.vibrationResult = VirtualXInputResult::Success;
    snapshot.state.mapping = {0u, mapping.source, 5u, 2u};
    snapshot.state.connected = true;
    snapshot.state.packetNumber = 9u;
    snapshot.state.gamepad = state.gamepad;
    snapshot.capabilities.mapping = snapshot.state.mapping;
    snapshot.capabilities.capabilities = capabilities.capabilities;
    snapshot.battery.mapping = snapshot.state.mapping;
    snapshot.battery.battery = battery.battery;
    snapshot.vibration.logicalSlot = 0u;
    snapshot.vibration.source = mapping.source;
    snapshot.vibration.sourceGeneration = 5u;
    snapshot.vibration.mappingGeneration = 2u;
    snapshot.vibration.commandSequence = 25u;
    snapshot.vibration.routeCount = 1u;
    snapshot.vibration.leftMotor = 123u;
    snapshot.vibration.rightMotor = 456u;
    ControllerSnapshotMessage decodedSnapshot;
    frame = decodeOrFail(encodeControllerSnapshot(25u, snapshot),
                         "controller snapshot response decodes");
    check(decodeControllerSnapshot(frame, decodedSnapshot, &error) &&
              decodedSnapshot == snapshot,
          "controller state/capabilities/battery/vibration snapshot round-trips consistently");

    auto malformed = encodeControllerUpdate(30u, mapping);
    malformed[kFrameHeaderBytes + 4u] = std::byte{0xff};
    frame = decodeOrFail(malformed,
                         "malformed controller enum frame is structural");
    check(!decodeControllerUpdate(frame, decodedUpdate, &error),
          "malformed controller update enum is rejected");
    malformed = encodeControllerUpdate(30u, mapping);
    malformed[kFrameHeaderBytes + 5u] = std::byte{4};
    frame = decodeOrFail(malformed,
                         "invalid controller slot frame is structural");
    check(!decodeControllerUpdate(frame, decodedUpdate, &error),
          "controller logical slot 4 is rejected");
    malformed = encodeControllerUpdate(30u, mapping);
    malformed[kFrameHeaderBytes + 6u] = std::byte{1};
    frame = decodeOrFail(malformed,
                         "nonzero controller reserved frame is structural");
    check(!decodeControllerUpdate(frame, decodedUpdate, &error),
          "controller reserved fields must be zero");
    auto invalidSource = mapping;
    invalidSource.source.sourceKey = 0u;
    frame = decodeOrFail(encodeControllerUpdate(31u, invalidSource),
                         "zero source-key frame is structural");
    check(!decodeControllerUpdate(frame, decodedUpdate, &error),
          "zero controller source key is rejected");
    invalidSource = mapping;
    invalidSource.sourceGeneration = 0u;
    frame = decodeOrFail(encodeControllerUpdate(32u, invalidSource),
                         "zero source-generation frame is structural");
    check(!decodeControllerUpdate(frame, decodedUpdate, &error),
          "zero controller source generation is rejected");
    ControllerUpdateMessage malformedUnmap;
    malformedUnmap.seatId = 1u;
    malformedUnmap.kind = ControllerUpdateKind::Unmap;
    malformedUnmap.source.runtimeXInputSlotHint = 0u;
    frame = decodeOrFail(encodeControllerUpdate(32u, malformedUnmap),
                         "noncanonical controller unmap frame is structural");
    check(!decodeControllerUpdate(frame, decodedUpdate, &error),
          "controller unmap rejects hidden source fields");
    auto truncated = encodeControllerUpdate(33u, mapping);
    truncated.pop_back();
    check(!decodeFrame(truncated), "truncated controller frame is rejected");

    auto inconsistent = snapshot;
    inconsistent.capabilities.mapping.source.sourceKey = 0x2222u;
    frame = decodeOrFail(encodeControllerSnapshot(34u, inconsistent),
                         "inconsistent controller snapshot is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "state/capabilities source mismatch is rejected");
    auto staleDisconnected = snapshot;
    staleDisconnected.stateResult = VirtualXInputResult::Disconnected;
    staleDisconnected.state.connected = false;
    frame = decodeOrFail(encodeControllerSnapshot(35u, staleDisconnected),
                         "stale disconnected snapshot is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "disconnected controller snapshot cannot retain gamepad state");

    auto disconnected = snapshot;
    disconnected.stateResult = VirtualXInputResult::Disconnected;
    disconnected.capabilitiesResult = VirtualXInputResult::Disconnected;
    disconnected.batteryResult = VirtualXInputResult::Disconnected;
    disconnected.vibrationResult = VirtualXInputResult::Disconnected;
    disconnected.state.connected = false;
    disconnected.state.gamepad = {};
    disconnected.capabilities = {};
    disconnected.battery = {};
    disconnected.vibration = {};
    frame = decodeOrFail(encodeControllerSnapshot(36u, disconnected),
                         "canonical disconnected snapshot is structural");
    check(decodeControllerSnapshot(frame, decodedSnapshot, &error) &&
              decodedSnapshot == disconnected,
          "disconnected state retains only its valid mapping identity");

    auto malformedDisconnected = disconnected;
    malformedDisconnected.state.mapping.source = {};
    frame = decodeOrFail(
        encodeControllerSnapshot(37u, malformedDisconnected),
        "malformed disconnected mapping is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "disconnected state requires a valid retained mapping identity");

    ControllerSnapshotMessage notMapped;
    notMapped.seatId = 1u;
    notMapped.logicalSlot = 2u;
    notMapped.state.mapping.logicalSlot = 2u;
    notMapped.capabilities.mapping.logicalSlot = 2u;
    notMapped.battery.mapping.logicalSlot = 2u;
    notMapped.vibration.logicalSlot = 2u;
    frame = decodeOrFail(encodeControllerSnapshot(38u, notMapped),
                         "canonical not-mapped snapshot is structural");
    check(decodeControllerSnapshot(frame, decodedSnapshot, &error) &&
              decodedSnapshot == notMapped,
          "not-mapped snapshot is canonically empty except for its outer logical slot");

    auto hiddenNotMapped = notMapped;
    hiddenNotMapped.state.mapping = {2u, mapping.source, 5u, 2u};
    frame = decodeOrFail(encodeControllerSnapshot(39u, hiddenNotMapped),
                         "not-mapped snapshot with hidden identity is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "not-mapped state rejects hidden mapping metadata");
    auto staleNotMappedPacket = notMapped;
    staleNotMappedPacket.state.packetNumber = 1u;
    frame = decodeOrFail(
        encodeControllerSnapshot(40u, staleNotMappedPacket),
        "not-mapped snapshot with stale packet number is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "not-mapped state rejects hidden packet metadata");

    auto hiddenCapabilities = disconnected;
    hiddenCapabilities.capabilities.mapping = disconnected.state.mapping;
    frame = decodeOrFail(encodeControllerSnapshot(41u, hiddenCapabilities),
                         "failed capabilities with hidden mapping is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "failed capabilities reject hidden mapping metadata");
    auto hiddenBattery = disconnected;
    hiddenBattery.battery.mapping = disconnected.state.mapping;
    frame = decodeOrFail(encodeControllerSnapshot(42u, hiddenBattery),
                         "failed battery with hidden mapping is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "failed battery rejects hidden mapping metadata");
    auto hiddenVibration = disconnected;
    hiddenVibration.vibration = snapshot.vibration;
    frame = decodeOrFail(encodeControllerSnapshot(43u, hiddenVibration),
                         "failed vibration with hidden route is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "failed vibration rejects every hidden route field");

    auto impossibleCapabilities = disconnected;
    impossibleCapabilities.capabilitiesResult =
        VirtualXInputResult::Success;
    impossibleCapabilities.capabilities.mapping =
        disconnected.state.mapping;
    impossibleCapabilities.capabilities.capabilities =
        capabilities.capabilities;
    frame = decodeOrFail(
        encodeControllerSnapshot(44u, impossibleCapabilities),
        "disconnected state with successful capabilities is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "successful capabilities require a successful connected state");
    auto impossibleBattery = disconnected;
    impossibleBattery.batteryResult = VirtualXInputResult::Success;
    impossibleBattery.battery.mapping = disconnected.state.mapping;
    impossibleBattery.battery.battery = battery.battery;
    frame = decodeOrFail(
        encodeControllerSnapshot(45u, impossibleBattery),
        "disconnected state with successful battery is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "successful battery requires a successful connected state");
    auto impossibleVibration = disconnected;
    impossibleVibration.vibrationResult = VirtualXInputResult::Success;
    impossibleVibration.vibration = snapshot.vibration;
    frame = decodeOrFail(
        encodeControllerSnapshot(46u, impossibleVibration),
        "disconnected state with successful vibration is structural");
    check(!decodeControllerSnapshot(frame, decodedSnapshot, &error),
          "successful vibration requires a successful connected state");

    auto unavailableBattery = snapshot;
    unavailableBattery.vibrationResult = VirtualXInputResult::Disconnected;
    unavailableBattery.vibration = {};
    unavailableBattery.battery.battery = {};
    frame = decodeOrFail(encodeControllerSnapshot(47u, unavailableBattery),
                         "successful unavailable battery snapshot is structural");
    check(decodeControllerSnapshot(frame, decodedSnapshot, &error) &&
              decodedSnapshot == unavailableBattery &&
              !decodedSnapshot.battery.battery.available,
          "battery available=false remains successful canonical metadata");
    check(controllerSeatAuthorityMatches(1u, 1u) &&
              !controllerSeatAuthorityMatches(1u, 2u) &&
              !controllerSeatAuthorityMatches(0u, 0u),
          "controller messages require the authenticated Seat authority");
}

void testSessionTokenGeneration() {
    const auto token = hydra::gatec::generateSessionToken();
    check(token.has_value(), "session token generation succeeds");
    const bool allZero = std::all_of(
        token->begin(), token->end(),
        [](std::uint8_t byte) { return byte == 0; });
    check(!allZero, "session token is not the all-zero sentinel");
    const auto encoded = hydra::gatec::tokenToHex(*token);
    check(encoded.size() == 32 &&
              hydra::gatec::tokenFromHex(encoded) == token,
          "generated session token round-trips through hexadecimal form");
    const auto pipeName = hydra::gatec::makeGateCPipeName(1234, 2, *token);
    check(pipeName.find(L"HydraSeat.GateC.1234.2.") != std::wstring::npos,
          "Gate C pipe name contains the host and Seat identity");
}

void testVirtualInputState() {
    using namespace hydra::gatec;

    VirtualInputState state;
    ControlStateMessage control;
    control.cursorX = 50;
    control.cursorY = 60;
    control.clipEnabled = true;
    control.virtualForeground = true;
    control.virtualCapture = true;
    control.clipLeft = 0;
    control.clipTop = 0;
    control.clipRight = 100;
    control.clipBottom = 100;
    check(state.applyControl(1, control),
          "control state applies with a new sequence");
    check(state.cursorX() == 50 && state.cursorY() == 60 &&
              state.virtualForeground() && state.virtualCapture(),
          "control state creates a process-local cursor/focus view");

    InputEventMessage keyDown;
    keyDown.kind = InputKind::Keyboard;
    keyDown.keyTransition = KeyTransition::Down;
    keyDown.vkey = 0x41;
    check(state.applyInput(2, keyDown), "key-down input applies");
    check(state.keyDown(0x41), "key-down state is visible");
    const auto keyboardState = state.keyboardState();
    check(keyboardState[0x41] == 0x80,
          "GetKeyboardState-style high bit is generated");
    check(state.consumeAsyncKeyState(0x41) == 0x8001u,
          "first async-state query returns down and edge bits");
    check(state.consumeAsyncKeyState(0x41) == 0x8000u,
          "async edge bit is consumed exactly once");

    check(!state.applyInput(2, keyDown),
          "duplicate/stale sequence is rejected");

    InputEventMessage mouse;
    mouse.kind = InputKind::Mouse;
    mouse.deltaX = 80;
    mouse.deltaY = 80;
    mouse.mouseButtonFlags = kMouseLeftDown;
    mouse.wheelDelta = 120;
    check(state.applyInput(3, mouse), "mouse input applies");
    check(state.cursorX() == 99 && state.cursorY() == 99,
          "virtual cursor is clipped without moving the global cursor");
    check((state.mouseButtonsDown() & 1u) != 0 &&
              state.wheelAccumulator() == 120,
          "mouse button and wheel state are process-local");

    InputEventMessage keyUp = keyDown;
    keyUp.keyTransition = KeyTransition::Up;
    check(state.applyInput(4, keyUp), "key-up input applies");
    check(!state.keyDown(0x41) &&
              state.consumeAsyncKeyState(0x41) == 0,
          "key-up clears down state without inventing a press edge");

    const auto snapshot = state.snapshot();
    check(snapshot.lastAppliedSequence == 4 &&
              !snapshotKeyDown(snapshot, 0x41) &&
              snapshot.cursorX == 99 && snapshot.cursorY == 99 &&
              snapshot.virtualForeground && snapshot.virtualCapture,
          "snapshot exposes the complete virtual process state");

    state.reset();
    check(state.lastAppliedSequence() == 0 &&
              state.mouseButtonsDown() == 0 &&
              !state.virtualForeground(),
          "reset restores a safe empty process-local state");
}

} // namespace

int main() {
    testProtocolRoundTrips();
    testProtocolRejectsMalformedFrames();
    testControllerProtocol();
    testSessionTokenGeneration();
    testVirtualInputState();
    std::cout << "Gate C protocol and virtual input-state tests passed.\n";
    return EXIT_SUCCESS;
}
