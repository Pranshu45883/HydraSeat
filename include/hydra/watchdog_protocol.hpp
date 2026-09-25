#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::watchdog {

inline constexpr std::uint32_t kWatchdogProtocolMagic = 0x31445748u; // "HWD1".
inline constexpr std::uint16_t kWatchdogProtocolVersion = 1;
inline constexpr std::size_t kWatchdogFrameHeaderBytes = 24;
inline constexpr std::size_t kWatchdogMaxPayloadBytes = 4096;
inline constexpr std::size_t kWatchdogMaxFrameBytes =
    kWatchdogFrameHeaderBytes + kWatchdogMaxPayloadBytes;
inline constexpr std::size_t kWatchdogMaxRollbackActions = 32;
inline constexpr std::uint32_t kWatchdogMinLeaseTimeoutMs = 100;
inline constexpr std::uint32_t kWatchdogMaxLeaseTimeoutMs = 60'000;
inline constexpr std::uint32_t kWatchdogMinActionTimeoutMs = 10;
inline constexpr std::uint32_t kWatchdogMaxActionTimeoutMs = 10'000;
inline constexpr std::uint32_t kWatchdogMaxRollbackTimeoutMs = 60'000;

using SessionId = std::array<std::uint8_t, 16>;

struct ProcessIdentity {
    std::uint32_t processId{0};
    std::uint64_t creationTime100ns{0};

    bool operator==(const ProcessIdentity&) const = default;
};

enum class RollbackActionKind : std::uint16_t {
    TerminateOwnedProcess = 1,
    CloseOwnedSession = 2,
    ClearOptionalBackendState = 3,
    ReleaseOverlayState = 4,
    RestoreSnapshotState = 5,
    WriteSafeModeResult = 6
};

struct RollbackActionDescriptor {
    std::uint32_t actionId{0};
    RollbackActionKind kind{RollbackActionKind::TerminateOwnedProcess};
    std::uint32_t activationOrdinal{0};
    std::uint32_t timeoutMilliseconds{0};
    std::uint64_t generation{0};
    std::uint64_t resourceId{0};
    ProcessIdentity process{};

    bool operator==(const RollbackActionDescriptor&) const = default;
};

struct WatchdogLease {
    SessionId sessionId{};
    std::uint64_t generation{0};
    std::uint32_t timeoutMilliseconds{0};

    bool operator==(const WatchdogLease&) const = default;
};

struct RollbackPlanManifest {
    WatchdogLease lease{};
    std::uint32_t rollbackTimeoutMilliseconds{0};
    std::vector<RollbackActionDescriptor> actions;

    bool operator==(const RollbackPlanManifest&) const = default;
};

enum class WatchdogMessageType : std::uint16_t {
    RegisterPlan = 1,
    RenewLease = 2,
    Disarm = 3,
    Status = 4
};

enum class WatchdogRunState : std::uint16_t {
    WaitingForPlan = 1,
    Armed = 2,
    RollingBack = 3,
    Disarmed = 4,
    RollbackComplete = 5,
    RecoveryRequired = 6
};

enum class WatchdogTriggerReason : std::uint16_t {
    None = 0,
    HostExited = 1,
    LeaseExpired = 2,
    ControlChannelClosed = 3,
    ProtocolViolation = 4,
    RollbackFailure = 5,
    CleanDisarm = 6
};

struct WatchdogStatus {
    SessionId sessionId{};
    std::uint64_t generation{0};
    WatchdogRunState state{WatchdogRunState::WaitingForPlan};
    WatchdogTriggerReason reason{WatchdogTriggerReason::None};
    std::uint16_t completedActions{0};
    std::uint16_t totalActions{0};
    std::uint32_t failedActionId{0};
    std::uint32_t systemError{0};

    bool operator==(const WatchdogStatus&) const = default;
};

struct DecodedWatchdogFrame {
    WatchdogMessageType type{WatchdogMessageType::Status};
    std::uint64_t sequence{0};
    std::vector<std::byte> payload;
};

struct WatchdogFrameDecodeResult {
    std::optional<DecodedWatchdogFrame> frame;
    std::string error;

    explicit operator bool() const noexcept { return frame.has_value(); }
};

bool isZeroSessionId(const SessionId& sessionId) noexcept;
bool isKnownRollbackActionKind(RollbackActionKind kind) noexcept;

bool validateRollbackPlan(const RollbackPlanManifest& manifest,
                          std::string* error = nullptr);

std::vector<std::byte> encodeWatchdogFrame(
    WatchdogMessageType type,
    std::uint64_t sequence,
    std::span<const std::byte> payload);
WatchdogFrameDecodeResult decodeWatchdogFrame(
    std::span<const std::byte> frameBytes);

std::vector<std::byte> encodeRegisterPlan(
    std::uint64_t sequence,
    const RollbackPlanManifest& manifest);
bool decodeRegisterPlan(const DecodedWatchdogFrame& frame,
                        RollbackPlanManifest& manifest,
                        std::string* error = nullptr);

std::vector<std::byte> encodeLeaseRenewal(
    std::uint64_t sequence,
    const WatchdogLease& lease);
bool decodeLeaseRenewal(const DecodedWatchdogFrame& frame,
                        WatchdogLease& lease,
                        std::string* error = nullptr);

std::vector<std::byte> encodeDisarm(
    std::uint64_t sequence,
    const WatchdogLease& lease);
bool decodeDisarm(const DecodedWatchdogFrame& frame,
                  WatchdogLease& lease,
                  std::string* error = nullptr);

std::vector<std::byte> encodeWatchdogStatus(
    std::uint64_t sequence,
    const WatchdogStatus& status);
bool decodeWatchdogStatus(const DecodedWatchdogFrame& frame,
                          WatchdogStatus& status,
                          std::string* error = nullptr);

std::string_view watchdogRunStateName(WatchdogRunState state) noexcept;
std::string_view watchdogTriggerReasonName(
    WatchdogTriggerReason reason) noexcept;

} // namespace hydra::watchdog
