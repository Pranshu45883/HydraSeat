#include "hydra/virtual_raw_input.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace hydra::gatec;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::uint16_t readU16(std::span<const std::byte> bytes,
                      std::size_t offset) {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8u));
}

std::uint32_t readU32(std::span<const std::byte> bytes,
                      std::size_t offset) {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        result |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(bytes[offset + index]))
            << (index * 8u);
    }
    return result;
}

VirtualRawRegistrationRequest registration(
    std::uint16_t usage, std::uint64_t window,
    std::uint32_t flags = 0) {
    return {{kRawUsagePageGenericDesktop, usage}, flags, window,
            window != 0};
}

InputEventMessage keyboard(std::uint32_t vkey = 0x41,
                           KeyTransition transition = KeyTransition::Down) {
    InputEventMessage input;
    input.kind = InputKind::Keyboard;
    input.keyTransition = transition;
    input.timestampMicros = 1234;
    input.vkey = vkey;
    input.scanCode = 0x1e;
    input.keyboardFlags = transition == KeyTransition::Up ? 1u : 0u;
    return input;
}

InputEventMessage mouse() {
    InputEventMessage input;
    input.kind = InputKind::Mouse;
    input.keyTransition = KeyTransition::None;
    input.timestampMicros = 5678;
    input.deltaX = -12;
    input.deltaY = 34;
    input.mouseButtonFlags = 0x0001u | 0x0400u;
    input.wheelDelta = 120;
    return input;
}

VirtualRawPacket dummyPacket(std::uint64_t sequence,
                             std::uint16_t usage = kRawUsageKeyboard) {
    VirtualRawPacket packet;
    packet.sequence = sequence;
    packet.seatId = 1;
    packet.inputKind = usage == kRawUsageKeyboard
        ? InputKind::Keyboard
        : InputKind::Mouse;
    packet.usage = {kRawUsagePageGenericDesktop, usage};
    packet.registrationGeneration = 1;
    packet.targetWindowRuntimeValue = 0x1001;
    packet.bytes.assign(48, std::byte{0});
    return packet;
}

void testPacketValidation() {
    auto packet = dummyPacket(1);
    packet.bytes[0] = static_cast<std::byte>(kRawTypeKeyboard);
    packet.bytes[4] = std::byte{48};
    check(validVirtualRawPacket(packet, RawArchitecture::X64),
          "well-formed immutable x64 packet validates");
    packet.bytes[4] = std::byte{0};
    check(!validVirtualRawPacket(packet, RawArchitecture::X64),
          "zero/non-progressing dwSize is rejected");
    packet.bytes[4] = std::byte{48};
    packet.bytes.pop_back();
    check(!validVirtualRawPacket(packet, RawArchitecture::X64),
          "truncated internal packet is rejected before data or buffer reads");
}

void testRegistrationPolicy() {
    VirtualRawInputContext context(RawArchitecture::X64, 7);
    check(context.registrations().empty(), "registration table starts empty");
    check(context.configure(1, 100) == VirtualRawResult::Success,
          "context configuration succeeds");

    const auto keyA = registration(kRawUsageKeyboard, 0x1001,
        kRawRidevInputSink | kRawRidevDeviceNotify);
    check(context.registerDevices(std::span(&keyA, 1)) ==
              VirtualRawResult::Success,
          "keyboard INPUTSINK plus DEVNOTIFY registration succeeds");
    auto values = context.registrations();
    check(values.size() == 1 &&
              values[0].requestedFlags ==
                  (kRawRidevInputSink | kRawRidevDeviceNotify) &&
              values[0].observableFlags == kRawRidevInputSink &&
              values[0].deviceNotificationRequested,
          "DEVNOTIFY intent is retained but not observable in query flags");
    const auto firstGeneration = values[0].generation;
    check(context.registerDevices(std::span(&keyA, 1)) ==
              VirtualRawResult::Success &&
              context.registrations()[0].generation > firstGeneration,
          "repeated same-target registration advances generation deterministically");

    const auto pointer = registration(kRawUsageMouse, 0x1001,
                                      kRawRidevDeviceNotify);
    check(context.registerDevices(std::span(&pointer, 1)) ==
              VirtualRawResult::Success,
          "mouse registration succeeds independently");
    values = context.registrations();
    check(values.size() == 2 && values[0].key.usage == kRawUsageMouse &&
              values[1].key.usage == kRawUsageKeyboard,
          "registration query ordering is deterministic by usage");

    const auto keyB = registration(kRawUsageKeyboard, 0x2002);
    check(context.registerDevices(std::span(&keyB, 1)) ==
              VirtualRawResult::Success,
          "last keyboard registration replaces only keyboard");
    values = context.registrations();
    const auto keyboardValue = std::find_if(
        values.begin(), values.end(), [](const auto& value) {
            return value.key.usage == kRawUsageKeyboard;
        });
    const auto mouseValue = std::find_if(
        values.begin(), values.end(), [](const auto& value) {
            return value.key.usage == kRawUsageMouse;
        });
    check(keyboardValue != values.end() && mouseValue != values.end() &&
              keyboardValue->targetWindowRuntimeValue == 0x2002 &&
              keyboardValue->generation > firstGeneration &&
              mouseValue->targetWindowRuntimeValue == 0x1001,
          "replacement advances generation without changing mouse target");

    const VirtualRawRegistrationRequest removeKey{
        {kRawUsagePageGenericDesktop, kRawUsageKeyboard},
        kRawRidevRemove, 0, false};
    check(context.registerDevices(std::span(&removeKey, 1)) ==
              VirtualRawResult::Success &&
              context.registrations().size() == 1 &&
              context.registrations()[0].key.usage == kRawUsageMouse,
          "keyboard removal is usage-local");
    check(context.registerDevices(std::span(&removeKey, 1)) ==
              VirtualRawResult::Success,
          "removing an absent usage is deterministic success");

    auto invalidRemove = removeKey;
    invalidRemove.targetWindowRuntimeValue = 1;
    invalidRemove.targetWindowCurrentProcess = true;
    check(context.registerDevices(std::span(&invalidRemove, 1)) ==
              VirtualRawResult::InvalidFlags,
          "RIDEV_REMOVE rejects a non-null target");
    const auto foreign = VirtualRawRegistrationRequest{
        {kRawUsagePageGenericDesktop, kRawUsageKeyboard}, 0, 0x9999, false};
    check(context.registerDevices(std::span(&foreign, 1)) ==
              VirtualRawResult::InvalidTarget,
          "foreign target registration fails closed");
    const auto unknown = VirtualRawRegistrationRequest{{2, 1}, 0, 1, true};
    check(context.registerDevices(std::span(&unknown, 1)) ==
              VirtualRawResult::UnsupportedUsage,
          "unallowlisted usage fails closed");
    auto badFlags = keyB;
    badFlags.flags = 0x10u;
    check(context.registerDevices(std::span(&badFlags, 1)) ==
              VirtualRawResult::InvalidFlags,
          "untested Raw Input flags fail closed");
    const VirtualRawRegistrationRequest removeMouse{
        {kRawUsagePageGenericDesktop, kRawUsageMouse},
        kRawRidevRemove, 0, false};
    check(context.registerDevices(std::span(&removeMouse, 1)) ==
              VirtualRawResult::Success && context.registrations().empty(),
          "mouse removal is independent and empties the remaining usage");
}

void testSyntheticHandleTable(RawArchitecture architecture) {
    SyntheticRawHandleTable table(architecture, 3);
    SyntheticRawHandleTable other(architecture, 4);
    const auto first = table.allocate(dummyPacket(1));
    const auto second = table.allocate(dummyPacket(2));
    check(first.result == VirtualRawResult::Success && first.token != 0 &&
              second.result == VirtualRawResult::Success &&
              second.token != first.token,
          "synthetic handle allocation returns unique nonzero tokens");
    if (architecture == RawArchitecture::X86) {
        check(first.token <= 0xffffffffu,
              "x86 token fits the pointer-sized boundary");
    } else {
        check(first.token <= 0x00007fffffffffffull,
              "x64 token remains a canonical opaque runtime value");
    }
    VirtualRawPacket packet;
    check(table.resolve(first.token, packet) == VirtualRawResult::Success &&
              packet.sequence == 1,
          "allocated token resolves its immutable packet");
    check(table.resolve(0, packet) == VirtualRawResult::UnknownToken &&
              table.resolve(1, packet) == VirtualRawResult::UnknownToken,
          "zero and malformed-marker tokens are rejected");
    check(other.resolve(first.token, packet) == VirtualRawResult::UnknownToken,
          "cross-context token is rejected");
    check(table.markDelivered(first.token) == VirtualRawResult::Success,
          "allocated token advances to delivered");
    check(table.consume(first.token) == VirtualRawResult::Success &&
              table.consume(first.token) == VirtualRawResult::ConsumedToken,
          "successful consume is single-shot and double consume is explicit");
    const auto reused = table.allocate(dummyPacket(3));
    check(reused.result == VirtualRawResult::Success &&
              reused.token != first.token &&
              table.resolve(first.token, packet) == VirtualRawResult::StaleToken,
          "slot reuse advances generation and rejects the stale token");
    table.reset();
    check(table.resolve(second.token, packet) == VirtualRawResult::StaleToken &&
              table.activeCount() == 0,
          "reset invalidates every active token");

    SyntheticRawHandleTable full(architecture, 5);
    std::vector<std::uint64_t> tokens;
    for (std::size_t index = 0; index < kVirtualRawMaximumHandles; ++index) {
        const auto allocation = full.allocate(dummyPacket(index + 1));
        check(allocation.result == VirtualRawResult::Success,
              "bounded handle table fills to its declared capacity");
        tokens.push_back(allocation.token);
    }
    check(full.allocate(dummyPacket(999)).result ==
              VirtualRawResult::HandleTableFull,
          "full handle table fails visibly");
    check(full.expire(tokens.front()) == VirtualRawResult::Success,
          "allocated handle may expire explicitly");
    check(table.maximumGeneration() ==
              (architecture == RawArchitecture::X86 ? 0x7ffu : 0xffffu),
          "generation width has an explicit architecture-specific retirement bound");
}

void testQueueBounds() {
    VirtualRawInputQueue queue;
    check(queue.packetCount() == 0 && queue.payloadBytes() == 0,
          "virtual queue starts empty");
    check(queue.enqueue(1, 40) == VirtualRawResult::Success &&
              queue.enqueue(2, 48) == VirtualRawResult::Success &&
              queue.tokens()[0] == 1 && queue.tokens()[1] == 2,
          "virtual queue preserves mixed FIFO order");
    check(queue.erase(1) && queue.packetCount() == 1 &&
              queue.payloadBytes() == 48,
          "individual token removal updates byte accounting");
    queue.clear();
    check(queue.enqueue(1, kVirtualRawMaximumPayloadBytes) ==
              VirtualRawResult::Success &&
              queue.enqueue(2, 1) == VirtualRawResult::QueueFull,
          "queue byte overflow is visible and does not mutate the queue");
    queue.clear();
    check(queue.enqueue(1, (std::numeric_limits<std::size_t>::max)()) ==
              VirtualRawResult::InvalidArgument &&
              queue.packetCount() == 0 && queue.payloadBytes() == 0,
          "oversized arithmetic input is rejected without mutating the queue");
    queue.clear();
    for (std::size_t index = 0; index < kVirtualRawMaximumPackets; ++index) {
        check(queue.enqueue(index + 1, 1) == VirtualRawResult::Success,
              "queue accepts packets through its hard count bound");
    }
    check(queue.enqueue(999, 1) == VirtualRawResult::QueueFull,
          "queue packet-count overflow is visible");
}

void testDataSemantics(RawArchitecture architecture) {
    VirtualRawInputContext context(architecture, 9);
    check(context.configure(1, 123) == VirtualRawResult::Success,
          "data context configures");
    const auto keyRegistration = registration(kRawUsageKeyboard, 0x1001);
    const auto mouseRegistration = registration(
        kRawUsageMouse, 0x1001, kRawRidevInputSink);
    const std::array registrations{keyRegistration, mouseRegistration};
    check(context.registerDevices(registrations) == VirtualRawResult::Success,
          "keyboard and mouse register for data tests");

    const auto keyDelivery = context.enqueueInput(1, keyboard());
    check(keyDelivery.result == VirtualRawResult::Success &&
              keyDelivery.messageWParam == kRawRimInput,
          "keyboard packet enqueues with foreground delivery code");
    check(context.completeDelivery(keyDelivery.token, true, true) ==
              VirtualRawResult::Success,
          "keyboard WM_INPUT delivery commits");
    const auto headerBytes = context.rawInputHeaderBytes();
    const auto inputBytes = context.rawInputBytes();
    std::vector<std::byte> output(inputBytes, std::byte{0x55});

    auto result = context.readData(keyDelivery.token, kRawRidHeader,
                                   headerBytes, {}, true);
    check(result.result == VirtualRawResult::Success &&
              result.returnValue == 0 && result.sizeAfter == headerBytes &&
              context.activeHandles() == 1,
          "RID_HEADER size query does not consume");
    result = context.readData(keyDelivery.token, kRawRidHeader,
                              headerBytes, output, false);
    const auto pointerBytes = architecture == RawArchitecture::X64 ? 8u : 4u;
    check(result.result == VirtualRawResult::Success &&
              result.returnValue == headerBytes &&
              readU32(output, 0) == kRawTypeKeyboard &&
              readU32(output, 4) == inputBytes &&
              readU32(output, 8) == 0 &&
              (pointerBytes == 4u || readU32(output, 12) == 0) &&
              readU32(output, 8u + pointerBytes) == kRawRimInput,
          "RID_HEADER returns architecture-correct header and full dwSize");

    result = context.readData(keyDelivery.token, kRawRidInput,
                              headerBytes, {}, true);
    check(result.result == VirtualRawResult::Success &&
              result.sizeAfter == inputBytes &&
              context.activeHandles() == 1,
          "RID_INPUT size query is non-consuming");
    std::vector<std::byte> shortOutput(inputBytes - 1, std::byte{0x55});
    const auto before = shortOutput;
    result = context.readData(keyDelivery.token, kRawRidInput,
                              headerBytes, shortOutput, false);
    check(result.result == VirtualRawResult::BufferTooSmall &&
              result.sizeAfter == inputBytes && shortOutput == before &&
              context.activeHandles() == 1,
          "undersized RID_INPUT leaves caller bytes and token unchanged");
    result = context.readData(keyDelivery.token, kRawRidInput,
                              headerBytes, output, false);
    const auto payload = static_cast<std::size_t>(headerBytes);
    check(result.result == VirtualRawResult::Success &&
              result.returnValue == inputBytes &&
              readU16(output, payload) == 0x1e &&
              readU16(output, payload + 6) == 0x41 &&
              readU32(output, payload + 8) == kRawWmKeyDown &&
              context.activeHandles() == 0 && context.queuedPackets() == 0,
          "full keyboard RID_INPUT read returns mapped fields and consumes once");
    check(context.readData(keyDelivery.token, kRawRidInput, headerBytes,
                           output, false).result ==
              VirtualRawResult::ConsumedToken,
          "consumed RID_INPUT token is rejected deterministically");
    check(context.readData(0, kRawRidInput, headerBytes, output, false).result ==
              VirtualRawResult::UnknownToken,
          "all-zero token is invalid");
    check(context.readData(123, 9, headerBytes, output, false).result ==
              VirtualRawResult::UnsupportedCommand,
          "unsupported data command fails closed");
    check(context.readData(123, kRawRidInput, headerBytes - 1,
                           output, false).result ==
              VirtualRawResult::InvalidArgument,
          "bad cbSizeHeader fails before token resolution");

    const auto mouseDelivery = context.enqueueInput(2, mouse());
    check(mouseDelivery.result == VirtualRawResult::Success &&
              mouseDelivery.messageWParam == kRawRimInputSink &&
              context.completeDelivery(mouseDelivery.token, true, true) ==
                  VirtualRawResult::Success,
          "mouse packet enqueues with INPUTSINK delivery code");
    output.assign(inputBytes, std::byte{0});
    result = context.readData(mouseDelivery.token, kRawRidInput,
                              headerBytes, output, false);
    check(result.result == VirtualRawResult::Success &&
              readU32(output, 0) == kRawTypeMouse &&
              readU16(output, payload + 4) == (0x0001u | 0x0400u) &&
              readU16(output, payload + 6) == 120 &&
              static_cast<std::int32_t>(readU32(output, payload + 12)) == -12 &&
              static_cast<std::int32_t>(readU32(output, payload + 16)) == 34,
          "mouse RID_INPUT maps relative movement, button flags, and wheel");

    auto keyUp = keyboard(0x41);
    keyUp.keyTransition = KeyTransition::Up;
    keyUp.keyboardFlags = 0;
    const auto keyUpDelivery = context.enqueueInput(3, keyUp);
    check(keyUpDelivery.result == VirtualRawResult::Success &&
              context.completeDelivery(keyUpDelivery.token, true, true) ==
                  VirtualRawResult::Success,
          "keyboard break packet enqueues and delivers");
    output.assign(inputBytes + 16u, std::byte{0x44});
    result = context.readData(keyUpDelivery.token, kRawRidInput,
                              headerBytes, output, false);
    check(result.result == VirtualRawResult::Success &&
              (readU16(output, payload + 2) & kRawKeyboardBreak) != 0 &&
              readU32(output, payload + 8) == kRawWmKeyUp &&
              std::all_of(output.begin() + inputBytes, output.end(),
                          [](std::byte value) {
                              return value == std::byte{0x44};
                          }),
          "keyboard key-up maps break semantics without touching oversized tail bytes");
}

void testBufferAndLifecycle(RawArchitecture architecture) {
    VirtualRawInputContext context(architecture, 11);
    check(context.configure(2, 456) == VirtualRawResult::Success,
          "buffer context configures");
    const std::array registrations{
        registration(kRawUsageKeyboard, 0x2002),
        registration(kRawUsageMouse, 0x2002)};
    check(context.registerDevices(registrations) == VirtualRawResult::Success,
          "buffer usages register");
    const auto key = context.enqueueInput(1, keyboard(0x42));
    const auto pointer = context.enqueueInput(2, mouse());
    check(key.result == VirtualRawResult::Success &&
              pointer.result == VirtualRawResult::Success &&
              context.completeDelivery(key.token, true, true) ==
                  VirtualRawResult::Success &&
              context.completeDelivery(pointer.token, true, true) ==
                  VirtualRawResult::Success,
          "mixed buffer packets enqueue and deliver");
    const auto header = context.rawInputHeaderBytes();
    auto query = context.readBuffer(header, {}, true);
    check(query.result == VirtualRawResult::Success &&
              query.packetCount == 0 &&
              query.sizeAfter == context.rawInputBytes() * 2u &&
              context.queuedPackets() == 2,
          "buffer size query reports required bytes without draining");
    std::vector<std::byte> shortBuffer(query.sizeAfter - 1,
                                       std::byte{0x77});
    const auto before = shortBuffer;
    auto read = context.readBuffer(header, shortBuffer, false);
    check(read.result == VirtualRawResult::BufferTooSmall &&
              read.sizeAfter == query.sizeAfter && shortBuffer == before &&
              context.queuedPackets() == 2,
          "insufficient raw buffer preserves bytes, queue, and tokens");
    std::vector<std::byte> buffer(query.sizeAfter, std::byte{0});
    read = context.readBuffer(header, buffer, false);
    const auto secondOffset = static_cast<std::size_t>(
        context.rawInputBytes());
    check(read.result == VirtualRawResult::Success &&
              read.packetCount == 2 && context.queuedPackets() == 0 &&
              context.activeHandles() == 0 &&
              readU32(buffer, 0) == kRawTypeKeyboard &&
              readU32(buffer, secondOffset) == kRawTypeMouse &&
              secondOffset % kVirtualRawBufferAlignment == 0,
          "successful mixed buffer read drains FIFO with eight-byte traversal");
    check(context.readBuffer(header, {}, false).result ==
              VirtualRawResult::Success,
          "empty raw buffer read succeeds with zero packets");

    const auto oneKey = context.enqueueInput(30, keyboard());
    check(oneKey.result == VirtualRawResult::Success &&
              context.completeDelivery(oneKey.token, true, true) ==
                  VirtualRawResult::Success,
          "single keyboard buffer packet delivers");
    query = context.readBuffer(header, {}, true);
    buffer.assign(query.sizeAfter, std::byte{0});
    read = context.readBuffer(header, buffer, false);
    check(read.result == VirtualRawResult::Success &&
              read.packetCount == 1 && readU32(buffer, 0) == kRawTypeKeyboard,
          "one-keyboard buffer read returns exactly one block");

    const auto oneMouse = context.enqueueInput(31, mouse());
    check(oneMouse.result == VirtualRawResult::Success &&
              context.completeDelivery(oneMouse.token, true, true) ==
                  VirtualRawResult::Success,
          "single mouse buffer packet delivers");
    query = context.readBuffer(header, {}, true);
    buffer.assign(query.sizeAfter, std::byte{0});
    read = context.readBuffer(header, buffer, false);
    check(read.result == VirtualRawResult::Success &&
              read.packetCount == 1 && readU32(buffer, 0) == kRawTypeMouse,
          "one-mouse buffer read returns exactly one block");

    const auto stale = context.enqueueInput(3, keyboard());
    check(stale.result == VirtualRawResult::Success,
          "packet exists before registration replacement");
    const auto replacement = registration(kRawUsageKeyboard, 0x3003);
    check(context.registerDevices(std::span(&replacement, 1)) ==
              VirtualRawResult::Success &&
              context.completeDelivery(stale.token, true, true) ==
                  VirtualRawResult::StaleToken,
          "re-registration expires queued old-generation packets");
    const auto invalidTarget = context.enqueueInput(4, keyboard());
    check(invalidTarget.result == VirtualRawResult::Success &&
              context.completeDelivery(invalidTarget.token, false, false) ==
                  VirtualRawResult::InvalidTarget &&
              context.queuedPackets() == 0,
          "destroyed target delivery fails closed without reroute");
    auto values = context.registrations();
    check(std::any_of(values.begin(), values.end(), [](const auto& value) {
              return value.key.usage == kRawUsageKeyboard &&
                     value.targetWindowRuntimeValue == 0x3003;
          }),
          "destroyed-target failure preserves the queried registration runtime value");
    const auto freshReplacement = registration(kRawUsageKeyboard, 0x4004);
    check(context.registerDevices(std::span(&freshReplacement, 1)) ==
              VirtualRawResult::Success,
          "a fresh valid registration replaces a destroyed runtime target");
    values = context.registrations();
    check(std::any_of(values.begin(), values.end(), [](const auto& value) {
              return value.key.usage == kRawUsageKeyboard &&
                     value.targetWindowRuntimeValue == 0x4004;
          }),
          "replacement query exposes only the fresh target");

    const auto future = context.enqueueInput(5, keyboard());
    check(future.result == VirtualRawResult::Success,
          "packet exists before usage removal");
    const VirtualRawRegistrationRequest remove{
        {kRawUsagePageGenericDesktop, kRawUsageKeyboard},
        kRawRidevRemove, 0, false};
    check(context.registerDevices(std::span(&remove, 1)) ==
              VirtualRawResult::Success &&
              context.queuedPackets() == 0 &&
              context.enqueueInput(6, keyboard()).result ==
                  VirtualRawResult::RegistrationMissing &&
              context.enqueueInput(7, mouse()).result ==
                  VirtualRawResult::Success,
          "removal expires only its usage and leaves mouse registered");
    context.beginStopping();
    check(context.enqueueInput(8, mouse()).result ==
              VirtualRawResult::SessionStopping,
          "stopping context rejects new packets visibly");
    context.reset();
    check(context.registrations().empty() && context.queuedPackets() == 0 &&
              context.activeHandles() == 0,
          "reset clears registrations, queue, and token table");
}

void testNoCrossContext() {
    VirtualRawInputContext first(RawArchitecture::X64, 21);
    VirtualRawInputContext second(RawArchitecture::X64, 22);
    check(first.configure(1, 1001) == VirtualRawResult::Success &&
              second.configure(2, 1002) == VirtualRawResult::Success,
          "two Seat contexts configure independently");
    const auto firstRegistration = registration(kRawUsageKeyboard, 0x1111);
    const auto secondRegistration = registration(kRawUsageKeyboard, 0x2222);
    check(first.registerDevices(std::span(&firstRegistration, 1)) ==
              VirtualRawResult::Success &&
              second.registerDevices(std::span(&secondRegistration, 1)) ==
                  VirtualRawResult::Success,
          "two Seat registrations remain process-local");
    const auto firstPacket = first.enqueueInput(1, keyboard(0x41));
    const auto secondPacket = second.enqueueInput(1, keyboard(0x42));
    std::vector<std::byte> bytes(first.rawInputBytes());
    check(first.readData(secondPacket.token, kRawRidInput,
                         first.rawInputHeaderBytes(), bytes, false).result ==
              VirtualRawResult::UnknownToken &&
              second.readData(firstPacket.token, kRawRidInput,
                          second.rawInputHeaderBytes(), bytes, false).result ==
              VirtualRawResult::UnknownToken &&
              first.queuedPackets() == 1 && second.queuedPackets() == 1,
          "tokens and packets never resolve across Seat contexts");
}

} // namespace

int main() {
    testPacketValidation();
    testRegistrationPolicy();
    testSyntheticHandleTable(RawArchitecture::X86);
    testSyntheticHandleTable(RawArchitecture::X64);
    testQueueBounds();
    testDataSemantics(RawArchitecture::X86);
    testDataSemantics(RawArchitecture::X64);
    testBufferAndLifecycle(RawArchitecture::X86);
    testBufferAndLifecycle(RawArchitecture::X64);
    testNoCrossContext();
    std::cout << "Virtual Raw Input component tests passed.\n";
    return EXIT_SUCCESS;
}
