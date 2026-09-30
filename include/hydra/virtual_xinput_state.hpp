#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace hydra::gatec {

inline constexpr std::size_t kVirtualXInputSlotCount = 4;
inline constexpr std::uint8_t kNoRuntimeXInputSlot = 0xffu;

enum class ControllerSourceKind : std::uint8_t {
    Synthetic = 1,
    ProfileSelected = 2
};

struct ControllerSourceIdentity {
    ControllerSourceKind kind{ControllerSourceKind::Synthetic};
    std::uint8_t runtimeXInputSlotHint{kNoRuntimeXInputSlot};
    std::uint64_t sourceKey{0};

    bool operator==(const ControllerSourceIdentity&) const = default;
};

struct NormalizedXInputGamepad {
    std::uint16_t buttons{0};
    std::uint8_t leftTrigger{0};
    std::uint8_t rightTrigger{0};
    std::int16_t thumbLX{0};
    std::int16_t thumbLY{0};
    std::int16_t thumbRX{0};
    std::int16_t thumbRY{0};

    bool operator==(const NormalizedXInputGamepad&) const = default;
};

enum class XInputCapabilityType : std::uint8_t {
    Gamepad = 1
};

struct NormalizedXInputCapabilities {
    XInputCapabilityType type{XInputCapabilityType::Gamepad};
    std::uint8_t subtype{0};
    std::uint16_t flags{0};
    NormalizedXInputGamepad gamepad{};
    bool vibrationSupported{false};
    std::uint16_t leftMotorMaximum{0};
    std::uint16_t rightMotorMaximum{0};

    bool operator==(const NormalizedXInputCapabilities&) const = default;
};

enum class XInputBatteryDeviceType : std::uint8_t {
    Gamepad = 0,
    Headset = 1
};

enum class XInputBatteryType : std::uint8_t {
    Disconnected = 0,
    Wired = 1,
    Alkaline = 2,
    Nimh = 3,
    Unknown = 0xffu
};

enum class XInputBatteryLevel : std::uint8_t {
    Empty = 0,
    Low = 1,
    Medium = 2,
    Full = 3
};

struct NormalizedXInputBattery {
    bool available{false};
    XInputBatteryDeviceType deviceType{XInputBatteryDeviceType::Gamepad};
    XInputBatteryType batteryType{XInputBatteryType::Disconnected};
    XInputBatteryLevel batteryLevel{XInputBatteryLevel::Empty};

    bool operator==(const NormalizedXInputBattery&) const = default;
};

struct VirtualXInputMapping {
    std::uint8_t logicalSlot{0};
    ControllerSourceIdentity source{};
    std::uint64_t sourceGeneration{0};
    std::uint64_t mappingGeneration{0};

    bool operator==(const VirtualXInputMapping&) const = default;
};

struct VirtualXInputState {
    VirtualXInputMapping mapping{};
    bool connected{false};
    std::uint32_t packetNumber{0};
    NormalizedXInputGamepad gamepad{};

    bool operator==(const VirtualXInputState&) const = default;
};

struct VirtualXInputCapabilities {
    VirtualXInputMapping mapping{};
    NormalizedXInputCapabilities capabilities{};

    bool operator==(const VirtualXInputCapabilities&) const = default;
};

struct VirtualXInputBattery {
    VirtualXInputMapping mapping{};
    NormalizedXInputBattery battery{};

    bool operator==(const VirtualXInputBattery&) const = default;
};

struct VirtualXInputVibrationRequest {
    std::uint8_t logicalSlot{0};
    std::uint16_t leftMotor{0};
    std::uint16_t rightMotor{0};
    std::uint64_t expectedMappingGeneration{0};
    std::uint64_t expectedSourceGeneration{0};
};

struct VirtualXInputVibrationRoute {
    std::uint8_t logicalSlot{0};
    ControllerSourceIdentity source{};
    std::uint64_t sourceGeneration{0};
    std::uint64_t mappingGeneration{0};
    std::uint64_t commandSequence{0};
    std::uint64_t routeCount{0};
    std::uint16_t leftMotor{0};
    std::uint16_t rightMotor{0};

    bool operator==(const VirtualXInputVibrationRoute&) const = default;
};

enum class VirtualXInputResult : std::uint32_t {
    Success = 0,
    InvalidArgument = 1,
    InvalidLogicalSlot = 2,
    InvalidSource = 3,
    DuplicateSource = 4,
    NotMapped = 5,
    Disconnected = 6,
    StaleSequence = 7,
    StaleGeneration = 8,
    MappingGenerationMismatch = 9,
    InvalidState = 10,
    GenerationOverflow = 11
};

constexpr std::uint32_t nextXInputPacketNumber(
    std::uint32_t current) noexcept {
    return current + 1u;
}

bool validControllerSourceIdentity(
    const ControllerSourceIdentity& source) noexcept;
bool sameControllerSourceIdentity(
    const ControllerSourceIdentity& left,
    const ControllerSourceIdentity& right) noexcept;
bool validXInputCapabilities(
    const NormalizedXInputCapabilities& capabilities) noexcept;
bool validXInputBattery(const NormalizedXInputBattery& battery) noexcept;

// One context belongs to exactly one controlled adapter/process. It owns four
// bounded logical slots and never polls or mutates a physical controller.
// Callers may use the returned vibration route at a separate source-backend
// boundary; this class performs no XInputGetState/XInputSetState calls.
class VirtualXInputContext {
public:
    VirtualXInputResult mapLogicalSlot(
        std::uint64_t sequence, std::uint8_t logicalSlot,
        const ControllerSourceIdentity& source,
        std::uint64_t sourceGeneration);
    VirtualXInputResult unmapLogicalSlot(
        std::uint64_t sequence, std::uint8_t logicalSlot);
    VirtualXInputResult applySourceState(
        std::uint64_t sequence, const ControllerSourceIdentity& source,
        std::uint64_t sourceGeneration,
        const NormalizedXInputGamepad& gamepad);
    VirtualXInputResult applySourceSnapshot(
        std::uint64_t sequence, const ControllerSourceIdentity& source,
        std::uint64_t sourceGeneration,
        const NormalizedXInputGamepad& gamepad,
        const NormalizedXInputCapabilities* capabilities,
        const NormalizedXInputBattery* battery);
    VirtualXInputResult applySourceCapabilities(
        std::uint64_t sequence, const ControllerSourceIdentity& source,
        std::uint64_t sourceGeneration,
        const NormalizedXInputCapabilities& capabilities);
    VirtualXInputResult applySourceBattery(
        std::uint64_t sequence, const ControllerSourceIdentity& source,
        std::uint64_t sourceGeneration,
        const NormalizedXInputBattery& battery);
    VirtualXInputResult disconnectSource(
        std::uint64_t sequence, const ControllerSourceIdentity& source,
        std::uint64_t sourceGeneration);
    VirtualXInputResult routeVibration(
        std::uint64_t sequence,
        const VirtualXInputVibrationRequest& request,
        VirtualXInputVibrationRoute& route);

    VirtualXInputResult getMapping(
        std::uint8_t logicalSlot, VirtualXInputMapping& mapping) const;
    VirtualXInputResult getState(
        std::uint8_t logicalSlot, VirtualXInputState& state) const;
    VirtualXInputResult getCapabilities(
        std::uint8_t logicalSlot,
        VirtualXInputCapabilities& capabilities) const;
    VirtualXInputResult getBattery(
        std::uint8_t logicalSlot, VirtualXInputBattery& battery) const;
    VirtualXInputResult getLastVibration(
        std::uint8_t logicalSlot,
        VirtualXInputVibrationRoute& route) const;

    void reset() noexcept;
    std::uint64_t lastAppliedSequence() const noexcept;

private:
    struct Slot {
        bool mapped{false};
        bool connected{false};
        bool requiresNewGeneration{false};
        bool capabilitiesAvailable{false};
        bool batteryAvailable{false};
        VirtualXInputMapping mapping{};
        std::uint32_t packetNumber{0};
        NormalizedXInputGamepad gamepad{};
        NormalizedXInputCapabilities capabilities{};
        NormalizedXInputBattery battery{};
        VirtualXInputVibrationRoute vibration{};
    };

    static bool validLogicalSlot(std::uint8_t logicalSlot) noexcept;
    static void clearConnectionState(Slot& slot) noexcept;
    Slot* findSource(const ControllerSourceIdentity& source) noexcept;
    const Slot* findSource(
        const ControllerSourceIdentity& source) const noexcept;
    bool sequenceAccepted(std::uint64_t sequence) const noexcept;
    static VirtualXInputResult validateGeneration(
        const Slot& slot, std::uint64_t generation,
        bool allowNewGeneration) noexcept;

    mutable std::mutex m_mutex;
    std::array<Slot, kVirtualXInputSlotCount> m_slots{};
    std::uint64_t m_lastAppliedSequence{0};
};

} // namespace hydra::gatec
