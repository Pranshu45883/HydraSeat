#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::hostipc {

constexpr std::uint32_t kHostProtocolMagic = 0x31505348u; // "HSP1"
constexpr std::uint16_t kHostProtocolVersion = 2u;
constexpr std::size_t kHostProtocolHeaderBytes = 24u;
constexpr std::size_t kHostProtocolMaxPayloadBytes = 64u * 1024u;
constexpr std::size_t kHostProtocolMaxDiagnosticBytes = 2048u;
constexpr std::size_t kHostProtocolMaxControllerIdBytes = 512u;
constexpr std::size_t kHostProtocolMaxAudioEndpointIdBytes = 2048u;
constexpr std::size_t kHostProtocolMaxLaunchTitleBytes = 512u;
constexpr std::size_t kHostProtocolMaxLaunchPathBytes = 32768u;
constexpr std::size_t kHostProtocolMaxLaunchArgumentsBytes = 16384u;
constexpr std::size_t kHostSeatCount = 2u;

enum class MessageType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    GetSnapshot = 3,
    Snapshot = 4,
    Ping = 5,
    Pong = 6,
    Error = 7,
    AcquireUiLease = 8,
    AcquireUiLeaseResult = 9,
    ReleaseUiLease = 10,
    ReleaseUiLeaseResult = 11,
    PairController = 12,
    PairControllerResult = 13,
    RouteAudio = 14,
    RouteAudioResult = 15,
    ResetAudio = 16,
    ResetAudioResult = 17,
    LaunchGame = 18,
    LaunchGameResult = 19,
    StopGame = 20,
    StopGameResult = 21,
};

enum class ClientRole : std::uint8_t {
    ReadOnly = 0,
    Control = 1,
};

enum class ErrorCode : std::uint16_t {
    None = 0,
    Malformed = 1,
    VersionMismatch = 2,
    PermissionDenied = 3,
    Unsupported = 4,
    InternalError = 5,
    InvalidState = 6,
};

enum class AudioMutationStatus : std::uint16_t {
    Success = 0,
    InvalidProcess = 1,
    ProcessNotFound = 2,
    AudioSessionNotFound = 3,
    EndpointNotFound = 4,
    EndpointUnavailable = 5,
    IdentityMismatch = 6,
    RoutingFailed = 7,
    OsApiError = 8,
};

struct Frame {
    MessageType type{MessageType::Error};
    std::uint64_t correlationId{0};
    std::vector<std::byte> payload;
};

struct Hello {
    ClientRole role{ClientRole::ReadOnly};

    bool operator==(const Hello&) const = default;
};

struct HelloAck {
    ClientRole role{ClientRole::ReadOnly};
    std::uint16_t protocolVersion{kHostProtocolVersion};
    std::uint8_t seatCount{static_cast<std::uint8_t>(kHostSeatCount)};

    bool operator==(const HelloAck&) const = default;
};

struct SeatSnapshot {
    std::uint32_t seatId{0};
    std::uint64_t generation{0};
    bool active{false};
    bool uiLeaseActive{false};
    bool gameLeaseActive{false};
    bool processOwned{false};
    bool windowOwned{false};
    bool controllerBound{false};
    std::uint32_t processId{0};
    std::uint64_t processCreationIdentity{0};
    std::uint64_t targetHwnd{0};

    bool operator==(const SeatSnapshot&) const = default;
};

struct HostSnapshot {
    std::uint64_t authorityRevision{0};
    std::array<SeatSnapshot, kHostSeatCount> seats{};

    bool operator==(const HostSnapshot&) const = default;
};

struct SeatRequest {
    std::uint32_t seatId{0};

    bool operator==(const SeatRequest&) const = default;
};

struct ControllerPairRequest {
    std::uint32_t seatId{0};
    std::uint8_t runtimeXInputSlot{0};
    std::string persistentControllerId;

    bool operator==(const ControllerPairRequest&) const = default;
};

struct LaunchGameRequest {
    std::uint32_t seatId{0};
    std::string titleUtf8;
    std::string executablePathUtf8;
    std::string launchArgumentsUtf8;
    std::string workingDirectoryUtf8;

    bool operator==(const LaunchGameRequest&) const = default;
};

struct ProcessRequest {
    std::uint32_t processId{0};
    std::uint64_t creationIdentity{0};

    bool operator==(const ProcessRequest&) const = default;
};

struct AudioRouteRequest {
    ProcessRequest process;
    std::string endpointId;

    bool operator==(const AudioRouteRequest&) const = default;
};

struct AudioMutationResult {
    AudioMutationStatus status{AudioMutationStatus::OsApiError};

    bool operator==(const AudioMutationResult&) const = default;
};

struct ErrorPayload {
    ErrorCode code{ErrorCode::InternalError};
    std::string diagnostic;

    bool operator==(const ErrorPayload&) const = default;
};

struct DecodeResult {
    ErrorCode error{ErrorCode::None};
    std::string diagnostic;
};

std::string_view messageTypeName(MessageType type) noexcept;
std::string_view errorCodeName(ErrorCode code) noexcept;

std::vector<std::byte> encodeFrame(const Frame& frame);
std::optional<Frame> decodeFrame(
    std::span<const std::byte> bytes,
    DecodeResult* result = nullptr);

std::vector<std::byte> encodeHello(const Hello& value);
std::optional<Hello> decodeHello(std::span<const std::byte> payload);

std::vector<std::byte> encodeHelloAck(const HelloAck& value);
std::optional<HelloAck> decodeHelloAck(std::span<const std::byte> payload);

std::vector<std::byte> encodeSnapshot(const HostSnapshot& snapshot);
std::optional<HostSnapshot> decodeSnapshot(std::span<const std::byte> payload);

std::vector<std::byte> encodeSeatRequest(const SeatRequest& request);
std::optional<SeatRequest> decodeSeatRequest(std::span<const std::byte> payload);

std::vector<std::byte> encodeControllerPairRequest(
    const ControllerPairRequest& request);
std::optional<ControllerPairRequest> decodeControllerPairRequest(
    std::span<const std::byte> payload);

std::vector<std::byte> encodeLaunchGameRequest(
    const LaunchGameRequest& request);
std::optional<LaunchGameRequest> decodeLaunchGameRequest(
    std::span<const std::byte> payload);

std::vector<std::byte> encodeProcessRequest(const ProcessRequest& request);
std::optional<ProcessRequest> decodeProcessRequest(
    std::span<const std::byte> payload);

std::vector<std::byte> encodeAudioRouteRequest(const AudioRouteRequest& request);
std::optional<AudioRouteRequest> decodeAudioRouteRequest(
    std::span<const std::byte> payload);

std::vector<std::byte> encodeAudioMutationResult(
    const AudioMutationResult& result);
std::optional<AudioMutationResult> decodeAudioMutationResult(
    std::span<const std::byte> payload);

std::vector<std::byte> encodePing(std::uint64_t nonce);
std::optional<std::uint64_t> decodePing(std::span<const std::byte> payload);

std::vector<std::byte> encodeError(const ErrorPayload& error);
std::optional<ErrorPayload> decodeError(std::span<const std::byte> payload);

bool isMutatingRequest(MessageType type) noexcept;
MessageType responseTypeFor(MessageType request) noexcept;

} // namespace hydra::hostipc
