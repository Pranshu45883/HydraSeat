#include "hydra/virtual_raw_input.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace hydra::gatec {
namespace {

constexpr std::uint64_t kTokenMarker = 0x5du;

void writeU16(std::vector<std::byte>& bytes, std::size_t offset,
              std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xffu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xffu);
}

void writeU32(std::vector<std::byte>& bytes, std::size_t offset,
              std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8u)) & 0xffu);
    }
}

void writeU64(std::vector<std::byte>& bytes, std::size_t offset,
              std::uint64_t value, std::size_t width) noexcept {
    for (std::size_t index = 0; index < width; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8u)) & 0xffu);
    }
}

std::uint32_t readU32(std::span<const std::byte> bytes,
                      std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(bytes[offset + index]))
            << (index * 8u);
    }
    return value;
}

bool addWouldOverflow(std::size_t left, std::size_t right) noexcept {
    return left > (std::numeric_limits<std::size_t>::max)() - right;
}

std::size_t alignedSize(std::size_t value) noexcept {
    if (addWouldOverflow(value, kVirtualRawBufferAlignment - 1u)) return 0;
    return (value + kVirtualRawBufferAlignment - 1u) &
           ~(kVirtualRawBufferAlignment - 1u);
}

} // namespace

bool validVirtualRawPacket(const VirtualRawPacket& packet,
                           RawArchitecture architecture) noexcept {
    const auto expectedHeader =
        architecture == RawArchitecture::X64 ? 24u : 16u;
    const auto expectedInput =
        architecture == RawArchitecture::X64 ? 48u : 40u;
    if (packet.bytes.size() != expectedInput ||
        packet.bytes.size() < expectedHeader ||
        packet.bytes.size() > kVirtualRawMaximumPayloadBytes) {
        return false;
    }
    const std::span<const std::byte> bytes(packet.bytes);
    const auto type = readU32(bytes, 0);
    const auto size = readU32(bytes, 4);
    return size == packet.bytes.size() &&
           ((packet.inputKind == InputKind::Keyboard &&
             type == kRawTypeKeyboard) ||
            (packet.inputKind == InputKind::Mouse &&
             type == kRawTypeMouse));
}

SyntheticRawHandleTable::SyntheticRawHandleTable(
    RawArchitecture architecture, std::uint16_t contextDiscriminator)
    : m_architecture(architecture),
      m_contextDiscriminator(contextDiscriminator) {
    const auto mask = architecture == RawArchitecture::X86 ? 0x3fu : 0xffu;
    m_contextDiscriminator = static_cast<std::uint16_t>(
        contextDiscriminator & mask);
    if (m_contextDiscriminator == 0) m_contextDiscriminator = 1;
}

std::uint64_t SyntheticRawHandleTable::maximumGeneration() const noexcept {
    return m_architecture == RawArchitecture::X86 ? 0x7ffu : 0xffffu;
}

std::uint64_t SyntheticRawHandleTable::encode(
    std::size_t slot, std::uint32_t generation) const noexcept {
    const auto encodedSlot = static_cast<std::uint64_t>(slot + 1u);
    if (m_architecture == RawArchitecture::X86) {
        return (kTokenMarker << 25u) |
               (static_cast<std::uint64_t>(m_contextDiscriminator) << 19u) |
               (static_cast<std::uint64_t>(generation) << 8u) |
               encodedSlot;
    }
    return (kTokenMarker << 40u) |
           (static_cast<std::uint64_t>(m_contextDiscriminator) << 32u) |
           (static_cast<std::uint64_t>(generation) << 16u) |
           encodedSlot;
}

VirtualRawResult SyntheticRawHandleTable::decode(
    std::uint64_t token, std::size_t& slot,
    std::uint32_t& generation) const noexcept {
    if (token == 0) return VirtualRawResult::UnknownToken;
    std::uint64_t marker = 0;
    std::uint64_t context = 0;
    std::uint64_t encodedSlot = 0;
    if (m_architecture == RawArchitecture::X86) {
        if (token > 0xffffffffu) return VirtualRawResult::UnknownToken;
        marker = (token >> 25u) & 0x7fu;
        context = (token >> 19u) & 0x3fu;
        generation = static_cast<std::uint32_t>((token >> 8u) & 0x7ffu);
        encodedSlot = token & 0xffu;
    } else {
        marker = (token >> 40u) & 0x7fu;
        context = (token >> 32u) & 0xffu;
        generation = static_cast<std::uint32_t>((token >> 16u) & 0xffffu);
        encodedSlot = token & 0xffffu;
    }
    if (marker != kTokenMarker ||
        context != m_contextDiscriminator || encodedSlot == 0 ||
        encodedSlot > m_slots.size() || generation == 0) {
        return VirtualRawResult::UnknownToken;
    }
    slot = static_cast<std::size_t>(encodedSlot - 1u);
    return VirtualRawResult::Success;
}

SyntheticRawHandleAllocation SyntheticRawHandleTable::allocate(
    VirtualRawPacket packet) {
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        auto& slot = m_slots[index];
        if (slot.state == SyntheticRawHandleState::Retired ||
            slot.state == SyntheticRawHandleState::Allocated ||
            slot.state == SyntheticRawHandleState::Delivered) {
            continue;
        }
        if (slot.state == SyntheticRawHandleState::Consumed ||
            slot.state == SyntheticRawHandleState::Expired) {
            if (slot.generation >= maximumGeneration()) {
                slot.state = SyntheticRawHandleState::Retired;
                slot.packet.reset();
                continue;
            }
            ++slot.generation;
            slot.state = SyntheticRawHandleState::Free;
        }
        if (slot.generation == 0 ||
            slot.generation > maximumGeneration()) {
            slot.state = SyntheticRawHandleState::Retired;
            slot.packet.reset();
            continue;
        }
        const auto token = encode(index, slot.generation);
        if (token == 0) {
            slot.state = SyntheticRawHandleState::Retired;
            continue;
        }
        packet.syntheticToken = token;
        slot.packet = std::move(packet);
        slot.state = SyntheticRawHandleState::Allocated;
        return {VirtualRawResult::Success, token};
    }
    return {VirtualRawResult::HandleTableFull, 0};
}

VirtualRawResult SyntheticRawHandleTable::resolve(
    std::uint64_t token, VirtualRawPacket& packet) const {
    std::size_t index = 0;
    std::uint32_t generation = 0;
    const auto decoded = decode(token, index, generation);
    if (decoded != VirtualRawResult::Success) return decoded;
    const auto& slot = m_slots[index];
    if (generation != slot.generation) return VirtualRawResult::StaleToken;
    if (slot.state == SyntheticRawHandleState::Consumed) {
        return VirtualRawResult::ConsumedToken;
    }
    if (slot.state == SyntheticRawHandleState::Expired ||
        slot.state == SyntheticRawHandleState::Free ||
        slot.state == SyntheticRawHandleState::Retired || !slot.packet) {
        return VirtualRawResult::StaleToken;
    }
    packet = *slot.packet;
    return VirtualRawResult::Success;
}

VirtualRawResult SyntheticRawHandleTable::markDelivered(std::uint64_t token) {
    std::size_t index = 0;
    std::uint32_t generation = 0;
    const auto decoded = decode(token, index, generation);
    if (decoded != VirtualRawResult::Success) return decoded;
    auto& slot = m_slots[index];
    if (generation != slot.generation) return VirtualRawResult::StaleToken;
    if (slot.state != SyntheticRawHandleState::Allocated || !slot.packet) {
        return VirtualRawResult::StaleToken;
    }
    slot.state = SyntheticRawHandleState::Delivered;
    return VirtualRawResult::Success;
}

void SyntheticRawHandleTable::releaseSlot(
    Slot& slot, SyntheticRawHandleState terminal) {
    slot.packet.reset();
    slot.state = terminal;
}

VirtualRawResult SyntheticRawHandleTable::consume(std::uint64_t token) {
    std::size_t index = 0;
    std::uint32_t generation = 0;
    const auto decoded = decode(token, index, generation);
    if (decoded != VirtualRawResult::Success) return decoded;
    auto& slot = m_slots[index];
    if (generation != slot.generation) return VirtualRawResult::StaleToken;
    if (slot.state == SyntheticRawHandleState::Consumed) {
        return VirtualRawResult::ConsumedToken;
    }
    if ((slot.state != SyntheticRawHandleState::Allocated &&
         slot.state != SyntheticRawHandleState::Delivered) || !slot.packet) {
        return VirtualRawResult::StaleToken;
    }
    releaseSlot(slot, SyntheticRawHandleState::Consumed);
    return VirtualRawResult::Success;
}

VirtualRawResult SyntheticRawHandleTable::expire(std::uint64_t token) {
    std::size_t index = 0;
    std::uint32_t generation = 0;
    const auto decoded = decode(token, index, generation);
    if (decoded != VirtualRawResult::Success) return decoded;
    auto& slot = m_slots[index];
    if (generation != slot.generation) return VirtualRawResult::StaleToken;
    if (slot.state != SyntheticRawHandleState::Allocated &&
        slot.state != SyntheticRawHandleState::Delivered) {
        return VirtualRawResult::StaleToken;
    }
    releaseSlot(slot, SyntheticRawHandleState::Expired);
    return VirtualRawResult::Success;
}

void SyntheticRawHandleTable::expireUsage(const RawUsageKey& key) {
    for (auto& slot : m_slots) {
        if ((slot.state == SyntheticRawHandleState::Allocated ||
             slot.state == SyntheticRawHandleState::Delivered) &&
            slot.packet && slot.packet->usage == key) {
            releaseSlot(slot, SyntheticRawHandleState::Expired);
        }
    }
}

void SyntheticRawHandleTable::reset() {
    for (auto& slot : m_slots) {
        if (slot.state != SyntheticRawHandleState::Retired) {
            if (slot.generation >= maximumGeneration()) {
                slot.state = SyntheticRawHandleState::Retired;
                slot.packet.reset();
            } else {
                ++slot.generation;
                slot.state = SyntheticRawHandleState::Free;
                slot.packet.reset();
            }
        }
    }
}

std::size_t SyntheticRawHandleTable::activeCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        m_slots.begin(), m_slots.end(), [](const Slot& slot) {
            return slot.state == SyntheticRawHandleState::Allocated ||
                   slot.state == SyntheticRawHandleState::Delivered;
        }));
}

VirtualRawResult VirtualRawInputQueue::enqueue(
    std::uint64_t token, std::size_t payloadBytes) {
    if (token == 0 || payloadBytes == 0 ||
        payloadBytes > kVirtualRawMaximumPayloadBytes) {
        return VirtualRawResult::InvalidArgument;
    }
    if (m_tokens.size() >= kVirtualRawMaximumPackets ||
        payloadBytes > kVirtualRawMaximumPayloadBytes - m_payloadBytes) {
        return VirtualRawResult::QueueFull;
    }
    m_tokens.push_back(token);
    m_sizes.push_back({token, payloadBytes});
    m_payloadBytes += payloadBytes;
    return VirtualRawResult::Success;
}

bool VirtualRawInputQueue::erase(std::uint64_t token) noexcept {
    const auto found = std::find(m_tokens.begin(), m_tokens.end(), token);
    const auto sizeFound = std::find_if(
        m_sizes.begin(), m_sizes.end(), [token](const SizeRecord& record) {
            return record.token == token;
        });
    if (found == m_tokens.end() || sizeFound == m_sizes.end()) return false;
    m_payloadBytes -= sizeFound->bytes;
    m_tokens.erase(found);
    m_sizes.erase(sizeFound);
    return true;
}

void VirtualRawInputQueue::eraseTokens(
    std::span<const std::uint64_t> tokens) noexcept {
    for (const auto token : tokens) (void)erase(token);
}

void VirtualRawInputQueue::clear() noexcept {
    m_tokens.clear();
    m_sizes.clear();
    m_payloadBytes = 0;
}

VirtualRawInputContext::VirtualRawInputContext(
    RawArchitecture architecture, std::uint16_t contextDiscriminator)
    : m_architecture(architecture),
      m_handles(architecture, contextDiscriminator) {}

std::uint32_t VirtualRawInputContext::rawInputHeaderBytes() const noexcept {
    return m_architecture == RawArchitecture::X64 ? 24u : 16u;
}

std::uint32_t VirtualRawInputContext::rawInputBytes() const noexcept {
    return m_architecture == RawArchitecture::X64 ? 48u : 40u;
}

VirtualRawResult VirtualRawInputContext::configure(
    std::uint32_t seatId, std::uint32_t processId) {
    if (seatId == 0 || processId == 0) {
        return VirtualRawResult::InvalidArgument;
    }
    std::scoped_lock lock(m_mutex);
    if (m_configured && (m_seatId != seatId || m_processId != processId)) {
        return VirtualRawResult::InvalidArgument;
    }
    m_seatId = seatId;
    m_processId = processId;
    m_configured = true;
    m_stopping = false;
    return VirtualRawResult::Success;
}

bool VirtualRawInputContext::supportedUsage(const RawUsageKey& key) noexcept {
    return key.usagePage == kRawUsagePageGenericDesktop &&
           (key.usage == kRawUsageKeyboard || key.usage == kRawUsageMouse);
}

std::size_t VirtualRawInputContext::usageIndex(
    const RawUsageKey& key) noexcept {
    return key.usage == kRawUsageKeyboard ? 0u : 1u;
}

std::uint32_t VirtualRawInputContext::observableFlags(
    std::uint32_t requested) noexcept {
    return requested & ~kRawRidevDeviceNotify;
}

void VirtualRawInputContext::expireUsageLocked(const RawUsageKey& key) {
    std::vector<std::uint64_t> expired;
    for (const auto token : m_queue.tokens()) {
        VirtualRawPacket packet;
        if (m_handles.resolve(token, packet) == VirtualRawResult::Success &&
            packet.usage == key) {
            expired.push_back(token);
        }
    }
    for (const auto token : expired) (void)m_handles.expire(token);
    m_queue.eraseTokens(expired);
    m_handles.expireUsage(key);
}

VirtualRawResult VirtualRawInputContext::registerDevices(
    std::span<const VirtualRawRegistrationRequest> requests) {
    if (requests.empty() ||
        requests.size() > kVirtualRawMaximumRegistrationOperations) {
        return VirtualRawResult::InvalidArgument;
    }
    for (const auto& request : requests) {
        if (!supportedUsage(request.key)) {
            return VirtualRawResult::UnsupportedUsage;
        }
        const auto allowed = kRawRidevInputSink | kRawRidevDeviceNotify;
        if ((request.flags & kRawRidevRemove) != 0) {
            if (request.flags != kRawRidevRemove ||
                request.targetWindowRuntimeValue != 0 ||
                request.targetWindowCurrentProcess) {
                return VirtualRawResult::InvalidFlags;
            }
        } else if ((request.flags & ~allowed) != 0) {
            return VirtualRawResult::InvalidFlags;
        } else if (request.targetWindowRuntimeValue == 0 ||
                   !request.targetWindowCurrentProcess) {
            return VirtualRawResult::InvalidTarget;
        }
    }

    std::scoped_lock lock(m_mutex);
    if (!m_configured || m_stopping) {
        return m_stopping ? VirtualRawResult::SessionStopping
                          : VirtualRawResult::InvalidArgument;
    }
    for (const auto& request : requests) {
        const auto index = usageIndex(request.key);
        if ((request.flags & kRawRidevRemove) != 0) {
            if (m_registrations[index]) {
                expireUsageLocked(request.key);
                m_registrations[index].reset();
                ++m_registrationGeneration;
            }
            continue;
        }
        expireUsageLocked(request.key);
        ++m_registrationGeneration;
        VirtualRawRegistration registration;
        registration.key = request.key;
        registration.requestedFlags = request.flags;
        registration.observableFlags = observableFlags(request.flags);
        registration.targetWindowRuntimeValue =
            request.targetWindowRuntimeValue;
        registration.targetWindowValidatedAtRegistration = true;
        registration.deviceNotificationRequested =
            (request.flags & kRawRidevDeviceNotify) != 0;
        registration.generation = m_registrationGeneration;
        m_registrations[index] = registration;
    }
    return VirtualRawResult::Success;
}

std::vector<VirtualRawRegistration>
VirtualRawInputContext::registrations() const {
    std::scoped_lock lock(m_mutex);
    std::vector<VirtualRawRegistration> result;
    for (const auto& registration : m_registrations) {
        if (registration) result.push_back(*registration);
    }
    std::sort(result.begin(), result.end(), [](const auto& left,
                                               const auto& right) {
        if (left.key.usagePage != right.key.usagePage) {
            return left.key.usagePage < right.key.usagePage;
        }
        return left.key.usage < right.key.usage;
    });
    return result;
}

VirtualRawPacket VirtualRawInputContext::serializePacket(
    std::uint64_t sequence, const InputEventMessage& input,
    const VirtualRawRegistration& registration) const {
    VirtualRawPacket packet;
    packet.sequence = sequence;
    packet.seatId = m_seatId;
    packet.inputKind = input.kind;
    packet.usage = registration.key;
    packet.creationTimestampMicros = input.timestampMicros;
    packet.registrationGeneration = registration.generation;
    packet.targetWindowRuntimeValue = registration.targetWindowRuntimeValue;
    packet.deliveryCode =
        (registration.requestedFlags & kRawRidevInputSink) != 0
            ? kRawRimInputSink
            : kRawRimInput;
    packet.bytes.assign(rawInputBytes(), std::byte{0});
    writeU32(packet.bytes, 0, input.kind == InputKind::Keyboard
                                  ? kRawTypeKeyboard
                                  : kRawTypeMouse);
    writeU32(packet.bytes, 4, rawInputBytes());
    const std::size_t pointerBytes =
        m_architecture == RawArchitecture::X64 ? 8u : 4u;
    // hDevice is deliberately null. This controlled synthetic stream does not
    // claim a stable or physical device handle.
    writeU64(packet.bytes, 8, 0, pointerBytes);
    writeU64(packet.bytes, 8u + pointerBytes, packet.deliveryCode,
             pointerBytes);
    const std::size_t payload = rawInputHeaderBytes();
    if (input.kind == InputKind::Keyboard) {
        writeU16(packet.bytes, payload, input.scanCode);
        constexpr auto makeFlagsMask = static_cast<std::uint16_t>(
            ~static_cast<std::uint16_t>(kRawKeyboardBreak));
        const auto keyboardFlags = static_cast<std::uint16_t>(
            (input.keyboardFlags & makeFlagsMask) |
            (input.keyTransition == KeyTransition::Up
                 ? kRawKeyboardBreak
                 : 0u));
        writeU16(packet.bytes, payload + 2u, keyboardFlags);
        writeU16(packet.bytes, payload + 4u, 0);
        writeU16(packet.bytes, payload + 6u,
                 static_cast<std::uint16_t>(input.vkey));
        writeU32(packet.bytes, payload + 8u,
                 input.keyTransition == KeyTransition::Down
                     ? kRawWmKeyDown
                     : kRawWmKeyUp);
        writeU32(packet.bytes, payload + 12u, 0);
    } else {
        writeU16(packet.bytes, payload, 0); // MOUSE_MOVE_RELATIVE.
        const auto buttonFlags = static_cast<std::uint16_t>(
            input.mouseButtonFlags |
            (input.wheelDelta != 0 ? kRawMouseWheel : 0u));
        writeU16(packet.bytes, payload + 4u, buttonFlags);
        writeU16(packet.bytes, payload + 6u,
                 static_cast<std::uint16_t>(input.wheelDelta));
        writeU32(packet.bytes, payload + 8u, 0);
        writeU32(packet.bytes, payload + 12u,
                 static_cast<std::uint32_t>(input.deltaX));
        writeU32(packet.bytes, payload + 16u,
                 static_cast<std::uint32_t>(input.deltaY));
        writeU32(packet.bytes, payload + 20u, 0);
    }
    return packet;
}

bool VirtualRawInputContext::validatePacket(
    const VirtualRawPacket& packet) const noexcept {
    return validVirtualRawPacket(packet, m_architecture);
}

VirtualRawDelivery VirtualRawInputContext::enqueueInput(
    std::uint64_t sequence, const InputEventMessage& input) {
    std::scoped_lock lock(m_mutex);
    VirtualRawDelivery delivery;
    if (!m_configured || sequence == 0) {
        delivery.result = VirtualRawResult::InvalidArgument;
        return delivery;
    }
    if (m_stopping) {
        delivery.result = VirtualRawResult::SessionStopping;
        return delivery;
    }
    const RawUsageKey key{kRawUsagePageGenericDesktop,
                          input.kind == InputKind::Keyboard
                              ? kRawUsageKeyboard
                              : kRawUsageMouse};
    const auto& registration = m_registrations[usageIndex(key)];
    if (!registration) {
        delivery.result = VirtualRawResult::RegistrationMissing;
        return delivery;
    }
    auto packet = serializePacket(sequence, input, *registration);
    if (!validatePacket(packet)) {
        delivery.result = VirtualRawResult::MalformedPacket;
        return delivery;
    }
    const auto allocation = m_handles.allocate(std::move(packet));
    if (allocation.result != VirtualRawResult::Success) {
        delivery.result = allocation.result;
        return delivery;
    }
    VirtualRawPacket stored;
    if (m_handles.resolve(allocation.token, stored) !=
        VirtualRawResult::Success) {
        (void)m_handles.expire(allocation.token);
        delivery.result = VirtualRawResult::InternalFailure;
        return delivery;
    }
    const auto queued = m_queue.enqueue(allocation.token, stored.bytes.size());
    if (queued != VirtualRawResult::Success) {
        (void)m_handles.expire(allocation.token);
        delivery.result = queued;
        return delivery;
    }
    delivery.result = VirtualRawResult::Success;
    delivery.token = allocation.token;
    delivery.targetWindowRuntimeValue = stored.targetWindowRuntimeValue;
    delivery.registrationGeneration = stored.registrationGeneration;
    delivery.messageWParam = stored.deliveryCode;
    return delivery;
}

VirtualRawResult VirtualRawInputContext::completeDelivery(
    std::uint64_t token, bool targetCurrentlyValid, bool postSucceeded) {
    std::scoped_lock lock(m_mutex);
    VirtualRawPacket packet;
    const auto resolved = m_handles.resolve(token, packet);
    if (resolved != VirtualRawResult::Success) return resolved;
    const auto& registration = m_registrations[usageIndex(packet.usage)];
    if (!registration ||
        registration->generation != packet.registrationGeneration ||
        registration->targetWindowRuntimeValue !=
            packet.targetWindowRuntimeValue) {
        (void)m_queue.erase(token);
        (void)m_handles.expire(token);
        return VirtualRawResult::RegistrationChanged;
    }
    if (!targetCurrentlyValid || !postSucceeded) {
        (void)m_queue.erase(token);
        (void)m_handles.expire(token);
        return VirtualRawResult::InvalidTarget;
    }
    return m_handles.markDelivered(token);
}

VirtualRawDataResult VirtualRawInputContext::readData(
    std::uint64_t token, std::uint32_t command,
    std::uint32_t cbSizeHeader, std::span<std::byte> output,
    bool sizeQuery) {
    std::scoped_lock lock(m_mutex);
    VirtualRawDataResult result;
    if (cbSizeHeader != rawInputHeaderBytes()) {
        result.result = VirtualRawResult::InvalidArgument;
        return result;
    }
    if (command != kRawRidHeader && command != kRawRidInput) {
        result.result = VirtualRawResult::UnsupportedCommand;
        return result;
    }
    VirtualRawPacket packet;
    const auto resolved = m_handles.resolve(token, packet);
    if (resolved != VirtualRawResult::Success) {
        result.result = resolved;
        return result;
    }
    if (!validatePacket(packet)) {
        result.result = VirtualRawResult::MalformedPacket;
        return result;
    }
    const auto required = command == kRawRidHeader
        ? rawInputHeaderBytes()
        : static_cast<std::uint32_t>(packet.bytes.size());
    result.sizeAfter = required;
    if (sizeQuery) {
        result.result = VirtualRawResult::Success;
        result.returnValue = 0;
        return result;
    }
    if (output.size() < required) {
        result.result = VirtualRawResult::BufferTooSmall;
        return result;
    }
    std::copy_n(packet.bytes.begin(), required, output.begin());
    result.result = VirtualRawResult::Success;
    result.returnValue = required;
    if (command == kRawRidInput) {
        (void)m_queue.erase(token);
        const auto consumed = m_handles.consume(token);
        if (consumed != VirtualRawResult::Success) {
            result.result = consumed;
            result.returnValue = 0xffffffffu;
        }
    }
    return result;
}

VirtualRawBufferResult VirtualRawInputContext::readBuffer(
    std::uint32_t cbSizeHeader, std::span<std::byte> output,
    bool sizeQuery) {
    std::scoped_lock lock(m_mutex);
    VirtualRawBufferResult result;
    if (cbSizeHeader != rawInputHeaderBytes()) {
        result.result = VirtualRawResult::InvalidArgument;
        return result;
    }
    std::size_t required = 0;
    std::vector<VirtualRawPacket> packets;
    std::vector<std::uint64_t> tokens;
    packets.reserve(m_queue.packetCount());
    tokens.reserve(m_queue.packetCount());
    for (const auto token : m_queue.tokens()) {
        VirtualRawPacket packet;
        if (m_handles.resolve(token, packet) != VirtualRawResult::Success ||
            !validatePacket(packet)) {
            result.result = VirtualRawResult::MalformedPacket;
            return result;
        }
        const auto aligned = alignedSize(packet.bytes.size());
        if (aligned == 0 || addWouldOverflow(required, aligned) ||
            required + aligned > kVirtualRawMaximumPayloadBytes) {
            result.result = VirtualRawResult::MalformedPacket;
            return result;
        }
        required += aligned;
        packets.push_back(std::move(packet));
        tokens.push_back(token);
    }
    result.sizeAfter = static_cast<std::uint32_t>(required);
    if (sizeQuery) {
        result.result = VirtualRawResult::Success;
        result.packetCount = 0;
        return result;
    }
    if (output.size() < required) {
        result.result = VirtualRawResult::BufferTooSmall;
        return result;
    }
    std::size_t offset = 0;
    for (const auto& packet : packets) {
        std::copy(packet.bytes.begin(), packet.bytes.end(),
                  output.begin() + static_cast<std::ptrdiff_t>(offset));
        const auto next = alignedSize(packet.bytes.size());
        std::fill(output.begin() + static_cast<std::ptrdiff_t>(
                      offset + packet.bytes.size()),
                  output.begin() + static_cast<std::ptrdiff_t>(offset + next),
                  std::byte{0});
        offset += next;
    }
    for (const auto token : tokens) {
        if (m_handles.consume(token) != VirtualRawResult::Success) {
            result.result = VirtualRawResult::InternalFailure;
            return result;
        }
    }
    m_queue.eraseTokens(tokens);
    result.result = VirtualRawResult::Success;
    result.packetCount = static_cast<std::uint32_t>(packets.size());
    return result;
}

void VirtualRawInputContext::beginStopping() {
    std::scoped_lock lock(m_mutex);
    m_stopping = true;
}

void VirtualRawInputContext::reset() {
    std::scoped_lock lock(m_mutex);
    m_stopping = true;
    m_queue.clear();
    m_handles.reset();
    for (auto& registration : m_registrations) registration.reset();
    m_registrationGeneration = 0;
    m_seatId = 0;
    m_processId = 0;
    m_configured = false;
}

std::size_t VirtualRawInputContext::queuedPackets() const {
    std::scoped_lock lock(m_mutex);
    return m_queue.packetCount();
}

std::size_t VirtualRawInputContext::queuedPayloadBytes() const {
    std::scoped_lock lock(m_mutex);
    return m_queue.payloadBytes();
}

std::size_t VirtualRawInputContext::activeHandles() const {
    std::scoped_lock lock(m_mutex);
    return m_handles.activeCount();
}

} // namespace hydra::gatec
