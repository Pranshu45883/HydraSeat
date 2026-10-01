#include "hydra/virtual_xinput_state.hpp"

#include <algorithm>
#include <limits>

namespace hydra::gatec {
namespace {

bool validSourceKind(ControllerSourceKind kind) noexcept {
    return kind == ControllerSourceKind::Synthetic ||
           kind == ControllerSourceKind::ProfileSelected;
}

bool validRuntimeSlot(std::uint8_t slot) noexcept {
    return slot < kVirtualXInputSlotCount || slot == kNoRuntimeXInputSlot;
}

} // namespace

bool validControllerSourceIdentity(
    const ControllerSourceIdentity& source) noexcept {
    return validSourceKind(source.kind) && source.sourceKey != 0 &&
           validRuntimeSlot(source.runtimeXInputSlotHint);
}

bool sameControllerSourceIdentity(
    const ControllerSourceIdentity& left,
    const ControllerSourceIdentity& right) noexcept {
    return left.kind == right.kind && left.sourceKey == right.sourceKey;
}

bool validXInputCapabilities(
    const NormalizedXInputCapabilities& capabilities) noexcept {
    if (capabilities.type != XInputCapabilityType::Gamepad) {
        return false;
    }
    if (!capabilities.vibrationSupported) {
        return capabilities.leftMotorMaximum == 0 &&
               capabilities.rightMotorMaximum == 0;
    }
    return capabilities.leftMotorMaximum != 0 ||
           capabilities.rightMotorMaximum != 0;
}

bool validXInputBattery(const NormalizedXInputBattery& battery) noexcept {
    const auto device = static_cast<std::uint8_t>(battery.deviceType);
    const auto type = static_cast<std::uint8_t>(battery.batteryType);
    const auto level = static_cast<std::uint8_t>(battery.batteryLevel);
    if (device > static_cast<std::uint8_t>(
                     XInputBatteryDeviceType::Headset) ||
        (type > static_cast<std::uint8_t>(XInputBatteryType::Nimh) &&
         type != static_cast<std::uint8_t>(XInputBatteryType::Unknown)) ||
        level > static_cast<std::uint8_t>(XInputBatteryLevel::Full)) {
        return false;
    }
    if (!battery.available) {
        return battery.deviceType == XInputBatteryDeviceType::Gamepad &&
               battery.batteryType == XInputBatteryType::Disconnected &&
               battery.batteryLevel == XInputBatteryLevel::Empty;
    }
    return battery.batteryType != XInputBatteryType::Disconnected;
}

bool VirtualXInputContext::validLogicalSlot(
    std::uint8_t logicalSlot) noexcept {
    return logicalSlot < kVirtualXInputSlotCount;
}

void VirtualXInputContext::clearConnectionState(Slot& slot) noexcept {
    slot.connected = false;
    slot.capabilitiesAvailable = false;
    slot.batteryAvailable = false;
    slot.gamepad = {};
    slot.capabilities = {};
    slot.battery = {};
    slot.vibration = {};
}

VirtualXInputContext::Slot* VirtualXInputContext::findSource(
    const ControllerSourceIdentity& source) noexcept {
    const auto found = std::find_if(
        m_slots.begin(), m_slots.end(), [&](const Slot& slot) {
            return slot.mapped &&
                   sameControllerSourceIdentity(slot.mapping.source, source);
        });
    return found == m_slots.end() ? nullptr : &*found;
}

const VirtualXInputContext::Slot* VirtualXInputContext::findSource(
    const ControllerSourceIdentity& source) const noexcept {
    const auto found = std::find_if(
        m_slots.begin(), m_slots.end(), [&](const Slot& slot) {
            return slot.mapped &&
                   sameControllerSourceIdentity(slot.mapping.source, source);
        });
    return found == m_slots.end() ? nullptr : &*found;
}

bool VirtualXInputContext::sequenceAccepted(
    std::uint64_t sequence) const noexcept {
    return sequence != 0 && sequence > m_lastAppliedSequence;
}

VirtualXInputResult VirtualXInputContext::validateGeneration(
    const Slot& slot, std::uint64_t generation,
    bool allowNewGeneration) noexcept {
    if (generation == 0) {
        return VirtualXInputResult::InvalidArgument;
    }
    if (generation < slot.mapping.sourceGeneration ||
        (generation == slot.mapping.sourceGeneration &&
         slot.requiresNewGeneration)) {
        return VirtualXInputResult::StaleGeneration;
    }
    if (!allowNewGeneration &&
        generation != slot.mapping.sourceGeneration) {
        return VirtualXInputResult::StaleGeneration;
    }
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::mapLogicalSlot(
    std::uint64_t sequence, std::uint8_t logicalSlot,
    const ControllerSourceIdentity& source,
    std::uint64_t sourceGeneration) {
    std::scoped_lock lock(m_mutex);
    if (!validLogicalSlot(logicalSlot)) {
        return VirtualXInputResult::InvalidLogicalSlot;
    }
    if (!validControllerSourceIdentity(source) || sourceGeneration == 0) {
        return VirtualXInputResult::InvalidSource;
    }
    if (!sequenceAccepted(sequence)) {
        return VirtualXInputResult::StaleSequence;
    }
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        if (index != logicalSlot && m_slots[index].mapped &&
            sameControllerSourceIdentity(
                m_slots[index].mapping.source, source)) {
            return VirtualXInputResult::DuplicateSource;
        }
    }

    auto& slot = m_slots[logicalSlot];
    if (slot.mapped &&
        sameControllerSourceIdentity(slot.mapping.source, source)) {
        const auto generationResult =
            validateGeneration(slot, sourceGeneration, true);
        if (generationResult != VirtualXInputResult::Success) {
            return generationResult;
        }
        if (slot.mapping.source == source &&
            slot.mapping.sourceGeneration == sourceGeneration) {
            m_lastAppliedSequence = sequence;
            return VirtualXInputResult::Success;
        }
    }
    if (slot.mapping.mappingGeneration ==
        std::numeric_limits<std::uint64_t>::max()) {
        return VirtualXInputResult::GenerationOverflow;
    }
    const auto nextMappingGeneration =
        slot.mapping.mappingGeneration + 1u;
    slot = {};
    slot.mapped = true;
    slot.mapping.logicalSlot = logicalSlot;
    slot.mapping.source = source;
    slot.mapping.sourceGeneration = sourceGeneration;
    slot.mapping.mappingGeneration = nextMappingGeneration;
    m_lastAppliedSequence = sequence;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::unmapLogicalSlot(
    std::uint64_t sequence, std::uint8_t logicalSlot) {
    std::scoped_lock lock(m_mutex);
    if (!validLogicalSlot(logicalSlot)) {
        return VirtualXInputResult::InvalidLogicalSlot;
    }
    if (!sequenceAccepted(sequence)) {
        return VirtualXInputResult::StaleSequence;
    }
    auto& slot = m_slots[logicalSlot];
    if (slot.mapping.mappingGeneration ==
        std::numeric_limits<std::uint64_t>::max()) {
        return VirtualXInputResult::GenerationOverflow;
    }
    const auto tombstoneGeneration =
        slot.mapping.mappingGeneration + 1u;
    slot = {};
    slot.mapping.logicalSlot = logicalSlot;
    slot.mapping.mappingGeneration = tombstoneGeneration;
    m_lastAppliedSequence = sequence;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::applySourceState(
    std::uint64_t sequence, const ControllerSourceIdentity& source,
    std::uint64_t sourceGeneration,
    const NormalizedXInputGamepad& gamepad) {
    std::scoped_lock lock(m_mutex);
    if (!validControllerSourceIdentity(source)) {
        return VirtualXInputResult::InvalidSource;
    }
    if (!sequenceAccepted(sequence)) {
        return VirtualXInputResult::StaleSequence;
    }
    auto* slot = findSource(source);
    if (slot == nullptr) return VirtualXInputResult::NotMapped;
    if (slot->mapping.source != source) {
        return VirtualXInputResult::InvalidState;
    }
    const auto generationResult =
        validateGeneration(*slot, sourceGeneration, true);
    if (generationResult != VirtualXInputResult::Success) {
        return generationResult;
    }

    const bool generationChanged =
        sourceGeneration != slot->mapping.sourceGeneration;
    if (generationChanged) {
        clearConnectionState(*slot);
        slot->mapping.sourceGeneration = sourceGeneration;
        slot->requiresNewGeneration = false;
    }
    const bool changed = generationChanged || !slot->connected ||
                         slot->gamepad != gamepad;
    slot->connected = true;
    slot->requiresNewGeneration = false;
    slot->gamepad = gamepad;
    if (changed) {
        slot->packetNumber = nextXInputPacketNumber(slot->packetNumber);
    }
    m_lastAppliedSequence = sequence;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::applySourceSnapshot(
    std::uint64_t sequence, const ControllerSourceIdentity& source,
    std::uint64_t sourceGeneration,
    const NormalizedXInputGamepad& gamepad,
    const NormalizedXInputCapabilities* capabilities,
    const NormalizedXInputBattery* battery) {
    std::scoped_lock lock(m_mutex);
    if (!validControllerSourceIdentity(source)) {
        return VirtualXInputResult::InvalidSource;
    }
    if ((capabilities != nullptr && !validXInputCapabilities(*capabilities)) ||
        (battery != nullptr && !validXInputBattery(*battery))) {
        return VirtualXInputResult::InvalidState;
    }
    if (!sequenceAccepted(sequence)) {
        return VirtualXInputResult::StaleSequence;
    }
    auto* slot = findSource(source);
    if (slot == nullptr) return VirtualXInputResult::NotMapped;

    const auto generationResult =
        validateGeneration(*slot, sourceGeneration, true);
    if (generationResult != VirtualXInputResult::Success) {
        return generationResult;
    }

    const bool routingChanged = slot->mapping.source != source;
    if (routingChanged && slot->mapping.mappingGeneration ==
                              std::numeric_limits<std::uint64_t>::max()) {
        return VirtualXInputResult::GenerationOverflow;
    }
    const bool generationChanged =
        sourceGeneration != slot->mapping.sourceGeneration;
    const bool stateChanged = routingChanged || generationChanged ||
                              !slot->connected || slot->gamepad != gamepad;

    if (routingChanged || generationChanged) {
        clearConnectionState(*slot);
    }
    if (routingChanged) {
        ++slot->mapping.mappingGeneration;
        slot->mapping.source = source;
    }
    slot->mapping.sourceGeneration = sourceGeneration;
    slot->connected = true;
    slot->requiresNewGeneration = false;
    slot->gamepad = gamepad;

    if (capabilities != nullptr) {
        slot->capabilities = *capabilities;
        slot->capabilitiesAvailable = true;
    } else {
        slot->capabilities = {};
        slot->capabilitiesAvailable = false;
        slot->vibration = {};
    }
    if (battery != nullptr) {
        slot->battery = *battery;
        slot->batteryAvailable = true;
    } else {
        slot->battery = {};
        slot->batteryAvailable = false;
    }
    if (capabilities != nullptr && !capabilities->vibrationSupported) {
        slot->vibration = {};
    }
    if (stateChanged) {
        slot->packetNumber = nextXInputPacketNumber(slot->packetNumber);
    }
    m_lastAppliedSequence = sequence;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::applySourceCapabilities(
    std::uint64_t sequence, const ControllerSourceIdentity& source,
    std::uint64_t sourceGeneration,
    const NormalizedXInputCapabilities& capabilities) {
    std::scoped_lock lock(m_mutex);
    if (!validControllerSourceIdentity(source)) {
        return VirtualXInputResult::InvalidSource;
    }
    if (!validXInputCapabilities(capabilities)) {
        return VirtualXInputResult::InvalidState;
    }
    if (!sequenceAccepted(sequence)) {
        return VirtualXInputResult::StaleSequence;
    }
    auto* slot = findSource(source);
    if (slot == nullptr) return VirtualXInputResult::NotMapped;
    if (slot->mapping.source != source) {
        return VirtualXInputResult::InvalidState;
    }
    const auto generationResult =
        validateGeneration(*slot, sourceGeneration, false);
    if (generationResult != VirtualXInputResult::Success) {
        return generationResult;
    }
    if (!slot->connected) return VirtualXInputResult::Disconnected;
    slot->capabilities = capabilities;
    slot->capabilitiesAvailable = true;
    m_lastAppliedSequence = sequence;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::applySourceBattery(
    std::uint64_t sequence, const ControllerSourceIdentity& source,
    std::uint64_t sourceGeneration,
    const NormalizedXInputBattery& battery) {
    std::scoped_lock lock(m_mutex);
    if (!validControllerSourceIdentity(source)) {
        return VirtualXInputResult::InvalidSource;
    }
    if (!validXInputBattery(battery)) {
        return VirtualXInputResult::InvalidState;
    }
    if (!sequenceAccepted(sequence)) {
        return VirtualXInputResult::StaleSequence;
    }
    auto* slot = findSource(source);
    if (slot == nullptr) return VirtualXInputResult::NotMapped;
    if (slot->mapping.source != source) {
        return VirtualXInputResult::InvalidState;
    }
    const auto generationResult =
        validateGeneration(*slot, sourceGeneration, false);
    if (generationResult != VirtualXInputResult::Success) {
        return generationResult;
    }
    if (!slot->connected) return VirtualXInputResult::Disconnected;
    slot->battery = battery;
    slot->batteryAvailable = true;
    m_lastAppliedSequence = sequence;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::disconnectSource(
    std::uint64_t sequence, const ControllerSourceIdentity& source,
    std::uint64_t sourceGeneration) {
    std::scoped_lock lock(m_mutex);
    if (!validControllerSourceIdentity(source)) {
        return VirtualXInputResult::InvalidSource;
    }
    if (!sequenceAccepted(sequence)) {
        return VirtualXInputResult::StaleSequence;
    }
    auto* slot = findSource(source);
    if (slot == nullptr) return VirtualXInputResult::NotMapped;
    const auto generationResult =
        validateGeneration(*slot, sourceGeneration, false);
    if (generationResult != VirtualXInputResult::Success) {
        return generationResult;
    }
    if (!slot->connected) return VirtualXInputResult::Disconnected;
    clearConnectionState(*slot);
    slot->requiresNewGeneration = true;
    slot->packetNumber = nextXInputPacketNumber(slot->packetNumber);
    m_lastAppliedSequence = sequence;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::routeVibration(
    std::uint64_t sequence, const VirtualXInputVibrationRequest& request,
    VirtualXInputVibrationRoute& route) {
    route = {};
    std::scoped_lock lock(m_mutex);
    if (!validLogicalSlot(request.logicalSlot)) {
        return VirtualXInputResult::InvalidLogicalSlot;
    }
    if (!sequenceAccepted(sequence)) {
        return VirtualXInputResult::StaleSequence;
    }
    auto& slot = m_slots[request.logicalSlot];
    if (!slot.mapped) return VirtualXInputResult::NotMapped;
    if (!slot.connected) return VirtualXInputResult::Disconnected;
    if (request.expectedMappingGeneration !=
        slot.mapping.mappingGeneration) {
        return VirtualXInputResult::MappingGenerationMismatch;
    }
    if (request.expectedSourceGeneration !=
        slot.mapping.sourceGeneration) {
        return VirtualXInputResult::StaleGeneration;
    }
    if (!slot.capabilitiesAvailable ||
        !slot.capabilities.vibrationSupported) {
        return VirtualXInputResult::InvalidState;
    }
    if (slot.vibration.routeCount ==
        std::numeric_limits<std::uint64_t>::max()) {
        return VirtualXInputResult::GenerationOverflow;
    }
    route.logicalSlot = request.logicalSlot;
    route.source = slot.mapping.source;
    route.sourceGeneration = slot.mapping.sourceGeneration;
    route.mappingGeneration = slot.mapping.mappingGeneration;
    route.commandSequence = sequence;
    route.routeCount = slot.vibration.routeCount + 1u;
    route.leftMotor = request.leftMotor;
    route.rightMotor = request.rightMotor;
    slot.vibration = route;
    m_lastAppliedSequence = sequence;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::getMapping(
    std::uint8_t logicalSlot, VirtualXInputMapping& mapping) const {
    mapping = {};
    std::scoped_lock lock(m_mutex);
    if (!validLogicalSlot(logicalSlot)) {
        return VirtualXInputResult::InvalidLogicalSlot;
    }
    const auto& slot = m_slots[logicalSlot];
    if (!slot.mapped) return VirtualXInputResult::NotMapped;
    mapping = slot.mapping;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::getState(
    std::uint8_t logicalSlot, VirtualXInputState& state) const {
    state = {};
    std::scoped_lock lock(m_mutex);
    if (!validLogicalSlot(logicalSlot)) {
        return VirtualXInputResult::InvalidLogicalSlot;
    }
    const auto& slot = m_slots[logicalSlot];
    if (!slot.mapped) return VirtualXInputResult::NotMapped;
    state.mapping = slot.mapping;
    state.packetNumber = slot.packetNumber;
    if (!slot.connected) return VirtualXInputResult::Disconnected;
    state.connected = true;
    state.gamepad = slot.gamepad;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::getCapabilities(
    std::uint8_t logicalSlot,
    VirtualXInputCapabilities& capabilities) const {
    capabilities = {};
    std::scoped_lock lock(m_mutex);
    if (!validLogicalSlot(logicalSlot)) {
        return VirtualXInputResult::InvalidLogicalSlot;
    }
    const auto& slot = m_slots[logicalSlot];
    if (!slot.mapped) return VirtualXInputResult::NotMapped;
    capabilities.mapping = slot.mapping;
    if (!slot.connected || !slot.capabilitiesAvailable) {
        return VirtualXInputResult::Disconnected;
    }
    capabilities.capabilities = slot.capabilities;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::getBattery(
    std::uint8_t logicalSlot, VirtualXInputBattery& battery) const {
    battery = {};
    std::scoped_lock lock(m_mutex);
    if (!validLogicalSlot(logicalSlot)) {
        return VirtualXInputResult::InvalidLogicalSlot;
    }
    const auto& slot = m_slots[logicalSlot];
    if (!slot.mapped) return VirtualXInputResult::NotMapped;
    battery.mapping = slot.mapping;
    if (!slot.connected || !slot.batteryAvailable) {
        return VirtualXInputResult::Disconnected;
    }
    battery.battery = slot.battery;
    return VirtualXInputResult::Success;
}

VirtualXInputResult VirtualXInputContext::getLastVibration(
    std::uint8_t logicalSlot,
    VirtualXInputVibrationRoute& route) const {
    route = {};
    std::scoped_lock lock(m_mutex);
    if (!validLogicalSlot(logicalSlot)) {
        return VirtualXInputResult::InvalidLogicalSlot;
    }
    const auto& slot = m_slots[logicalSlot];
    if (!slot.mapped) return VirtualXInputResult::NotMapped;
    if (!slot.connected || slot.vibration.routeCount == 0) {
        return VirtualXInputResult::Disconnected;
    }
    route = slot.vibration;
    return VirtualXInputResult::Success;
}

void VirtualXInputContext::reset() noexcept {
    std::scoped_lock lock(m_mutex);
    m_slots = {};
    m_lastAppliedSequence = 0;
}

std::uint64_t VirtualXInputContext::lastAppliedSequence() const noexcept {
    std::scoped_lock lock(m_mutex);
    return m_lastAppliedSequence;
}

} // namespace hydra::gatec
