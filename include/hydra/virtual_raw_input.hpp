#pragma once

#include "hydra/gate_c_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace hydra::gatec {

inline constexpr std::uint16_t kRawUsagePageGenericDesktop = 0x0001;
inline constexpr std::uint16_t kRawUsageMouse = 0x0002;
inline constexpr std::uint16_t kRawUsageKeyboard = 0x0006;
inline constexpr std::uint32_t kRawTypeMouse = 0;
inline constexpr std::uint32_t kRawTypeKeyboard = 1;
inline constexpr std::uint32_t kRawRidevRemove = 0x00000001u;
inline constexpr std::uint32_t kRawRidevInputSink = 0x00000100u;
inline constexpr std::uint32_t kRawRidevDeviceNotify = 0x00002000u;
inline constexpr std::uint32_t kRawRidInput = 0x10000003u;
inline constexpr std::uint32_t kRawRidHeader = 0x10000005u;
inline constexpr std::uint32_t kRawRimInput = 0;
inline constexpr std::uint32_t kRawRimInputSink = 1;
inline constexpr std::uint32_t kRawWmKeyDown = 0x0100u;
inline constexpr std::uint32_t kRawWmKeyUp = 0x0101u;
inline constexpr std::uint16_t kRawKeyboardBreak = 0x0001u;
inline constexpr std::uint16_t kRawMouseWheel = 0x0400u;
inline constexpr std::size_t kVirtualRawMaximumRegistrations = 2;
inline constexpr std::size_t kVirtualRawMaximumRegistrationOperations = 16;
inline constexpr std::size_t kVirtualRawMaximumHandles = 128;
inline constexpr std::size_t kVirtualRawMaximumPackets = 128;
inline constexpr std::size_t kVirtualRawMaximumPayloadBytes = 64 * 1024;
inline constexpr std::size_t kVirtualRawBufferAlignment = 8;

enum class RawArchitecture : std::uint16_t {
    X86 = 32,
    X64 = 64
};

enum class VirtualRawResult : std::uint32_t {
    Success = 0,
    InvalidArgument = 1,
    UnsupportedUsage = 2,
    InvalidFlags = 3,
    InvalidTarget = 4,
    QueueFull = 5,
    HandleTableFull = 6,
    UnknownToken = 7,
    StaleToken = 8,
    ConsumedToken = 9,
    RegistrationChanged = 10,
    SessionStopping = 11,
    BufferTooSmall = 12,
    UnsupportedCommand = 13,
    MalformedPacket = 14,
    GenerationExhausted = 15,
    InternalFailure = 16,
    RegistrationMissing = 17
};

struct RawUsageKey {
    std::uint16_t usagePage{0};
    std::uint16_t usage{0};

    bool operator==(const RawUsageKey&) const = default;
};

struct VirtualRawRegistration {
    RawUsageKey key;
    std::uint32_t requestedFlags{0};
    std::uint32_t observableFlags{0};
    std::uint64_t targetWindowRuntimeValue{0};
    bool targetWindowValidatedAtRegistration{false};
    bool deviceNotificationRequested{false};
    std::uint64_t generation{0};

    bool operator==(const VirtualRawRegistration&) const = default;
};

struct VirtualRawRegistrationRequest {
    RawUsageKey key;
    std::uint32_t flags{0};
    std::uint64_t targetWindowRuntimeValue{0};
    bool targetWindowCurrentProcess{false};
};

struct VirtualRawPacket {
    std::uint64_t sequence{0};
    std::uint32_t seatId{0};
    InputKind inputKind{InputKind::Keyboard};
    RawUsageKey usage;
    std::uint64_t creationTimestampMicros{0};
    std::uint64_t registrationGeneration{0};
    std::uint64_t targetWindowRuntimeValue{0};
    std::uint32_t deliveryCode{kRawRimInput};
    std::vector<std::byte> bytes;
    std::uint64_t syntheticToken{0};

    bool operator==(const VirtualRawPacket&) const = default;
};

bool validVirtualRawPacket(const VirtualRawPacket& packet,
                           RawArchitecture architecture) noexcept;

enum class SyntheticRawHandleState : std::uint8_t {
    Free,
    Allocated,
    Delivered,
    Consumed,
    Expired,
    Retired
};

struct SyntheticRawHandleAllocation {
    VirtualRawResult result{VirtualRawResult::InternalFailure};
    std::uint64_t token{0};
};

class SyntheticRawHandleTable {
public:
    SyntheticRawHandleTable(RawArchitecture architecture,
                            std::uint16_t contextDiscriminator);

    SyntheticRawHandleAllocation allocate(VirtualRawPacket packet);
    VirtualRawResult markDelivered(std::uint64_t token);
    VirtualRawResult resolve(std::uint64_t token,
                             VirtualRawPacket& packet) const;
    VirtualRawResult consume(std::uint64_t token);
    VirtualRawResult expire(std::uint64_t token);
    void expireUsage(const RawUsageKey& key);
    void reset();

    std::size_t activeCount() const noexcept;
    std::uint64_t maximumGeneration() const noexcept;

private:
    struct Slot {
        SyntheticRawHandleState state{SyntheticRawHandleState::Free};
        std::uint32_t generation{1};
        std::optional<VirtualRawPacket> packet;
    };

    std::uint64_t encode(std::size_t slot,
                         std::uint32_t generation) const noexcept;
    VirtualRawResult decode(std::uint64_t token, std::size_t& slot,
                            std::uint32_t& generation) const noexcept;
    void releaseSlot(Slot& slot, SyntheticRawHandleState terminal);

    RawArchitecture m_architecture;
    std::uint16_t m_contextDiscriminator;
    std::array<Slot, kVirtualRawMaximumHandles> m_slots{};
};

class VirtualRawInputQueue {
public:
    VirtualRawResult enqueue(std::uint64_t token, std::size_t payloadBytes);
    bool erase(std::uint64_t token) noexcept;
    void eraseTokens(std::span<const std::uint64_t> tokens) noexcept;
    void clear() noexcept;

    const std::deque<std::uint64_t>& tokens() const noexcept {
        return m_tokens;
    }
    std::size_t packetCount() const noexcept { return m_tokens.size(); }
    std::size_t payloadBytes() const noexcept { return m_payloadBytes; }

private:
    struct SizeRecord {
        std::uint64_t token{0};
        std::size_t bytes{0};
    };
    std::deque<std::uint64_t> m_tokens;
    std::deque<SizeRecord> m_sizes;
    std::size_t m_payloadBytes{0};
};

struct VirtualRawDelivery {
    VirtualRawResult result{VirtualRawResult::InternalFailure};
    std::uint64_t token{0};
    std::uint64_t targetWindowRuntimeValue{0};
    std::uint64_t registrationGeneration{0};
    std::uint32_t messageWParam{kRawRimInput};
};

struct VirtualRawDataResult {
    VirtualRawResult result{VirtualRawResult::InternalFailure};
    std::uint32_t returnValue{0xffffffffu};
    std::uint32_t sizeAfter{0};
};

struct VirtualRawBufferResult {
    VirtualRawResult result{VirtualRawResult::InternalFailure};
    std::uint32_t packetCount{0xffffffffu};
    std::uint32_t sizeAfter{0};
};

class VirtualRawInputContext {
public:
    VirtualRawInputContext(RawArchitecture architecture,
                           std::uint16_t contextDiscriminator);

    VirtualRawResult configure(std::uint32_t seatId,
                               std::uint32_t processId);
    VirtualRawResult registerDevices(
        std::span<const VirtualRawRegistrationRequest> requests);
    std::vector<VirtualRawRegistration> registrations() const;

    VirtualRawDelivery enqueueInput(std::uint64_t sequence,
                                    const InputEventMessage& input);
    VirtualRawResult completeDelivery(std::uint64_t token,
                                      bool targetCurrentlyValid,
                                      bool postSucceeded);
    VirtualRawDataResult readData(std::uint64_t token,
                                 std::uint32_t command,
                                 std::uint32_t cbSizeHeader,
                                 std::span<std::byte> output,
                                 bool sizeQuery);
    VirtualRawBufferResult readBuffer(std::uint32_t cbSizeHeader,
                                     std::span<std::byte> output,
                                     bool sizeQuery);

    void beginStopping();
    void reset();
    std::size_t queuedPackets() const;
    std::size_t queuedPayloadBytes() const;
    std::size_t activeHandles() const;

    RawArchitecture architecture() const noexcept { return m_architecture; }
    std::uint32_t rawInputHeaderBytes() const noexcept;
    std::uint32_t rawInputBytes() const noexcept;

private:
    static bool supportedUsage(const RawUsageKey& key) noexcept;
    static std::size_t usageIndex(const RawUsageKey& key) noexcept;
    static std::uint32_t observableFlags(std::uint32_t requested) noexcept;
    VirtualRawPacket serializePacket(std::uint64_t sequence,
                                     const InputEventMessage& input,
                                     const VirtualRawRegistration& registration)
        const;
    bool validatePacket(const VirtualRawPacket& packet) const noexcept;
    void expireUsageLocked(const RawUsageKey& key);

    RawArchitecture m_architecture;
    mutable std::mutex m_mutex;
    std::uint32_t m_seatId{0};
    std::uint32_t m_processId{0};
    std::uint64_t m_registrationGeneration{0};
    bool m_configured{false};
    bool m_stopping{false};
    std::array<std::optional<VirtualRawRegistration>,
               kVirtualRawMaximumRegistrations> m_registrations{};
    SyntheticRawHandleTable m_handles;
    VirtualRawInputQueue m_queue;
};

} // namespace hydra::gatec
