#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::rawprobe {

inline constexpr std::uint32_t kRawInputProbeSchemaVersion = 1;
inline constexpr std::size_t kMaxRawRegistrations = 16;
inline constexpr std::size_t kMaxTraceEvents = 512;
inline constexpr std::size_t kMaxRawPacketBytes = 64 * 1024;
inline constexpr std::size_t kMaxRawBufferPackets = 128;
inline constexpr std::size_t kMaxFixtureBytes = 4 * 1024 * 1024;
inline constexpr std::uint32_t kMaxObserveSeconds = 30;

enum class RawProbeSourceKind : std::uint8_t {
    SyntheticParserFixture = 1,
    ObservedWindowsApi = 2,
    SyntheticOsInputExperiment = 3
};

enum class RawProbeOperation : std::uint8_t {
    Snapshot = 1,
    RegisterKeyboard = 2,
    RegisterMouse = 3,
    ReplaceKeyboardTarget = 4,
    ReplaceMouseTarget = 5,
    RegisterInputSink = 6,
    RegisterDeviceNotify = 7,
    RegisterBackgroundDeviceNotify = 8,
    RemoveKeyboard = 9,
    RemoveMouse = 10,
    DestroyTargetWindow = 11,
    ReplaceDestroyedTarget = 12,
    Cleanup = 13
};

enum class RawProbeResultKind : std::uint8_t {
    Success = 1,
    ApiFailure = 2,
    InvalidContract = 3,
    Truncated = 4,
    Oversized = 5,
    UnsupportedType = 6,
    SizeMismatch = 7,
    BoundsExceeded = 8,
    NotObserved = 9
};

enum class RawMessageKind : std::uint8_t {
    Input = 1,
    DeviceChange = 2
};

enum class RawInputCodeKind : std::uint8_t {
    Foreground = 1,
    Background = 2,
    Unknown = 3
};

enum class RawDeviceChangeKind : std::uint8_t {
    Arrival = 1,
    Removal = 2,
    Unknown = 3
};

struct RawTraceLimits {
    std::uint32_t maximumRegistrations{kMaxRawRegistrations};
    std::uint32_t maximumTraceEvents{kMaxTraceEvents};
    std::uint32_t maximumRawPacketBytes{kMaxRawPacketBytes};
    std::uint32_t maximumRawBufferPackets{kMaxRawBufferPackets};
    std::uint32_t maximumFixtureBytes{kMaxFixtureBytes};

    bool operator==(const RawTraceLimits&) const = default;
};

struct RawEventContext {
    std::uint64_t sequence{0};
    std::uint64_t monotonicTimestampMicros{0};
    std::uint32_t threadId{0};

    bool operator==(const RawEventContext&) const = default;
};

// All HWND/HANDLE/HRAWINPUT fields in this contract are runtime-only
// diagnostic values. They are never stable or persistent identity.
struct RawRegistrationDescriptor {
    std::uint16_t usagePage{0};
    std::uint16_t usage{0};
    std::uint32_t flags{0};
    std::uint64_t targetWindowRuntimeValue{0};

    bool operator==(const RawRegistrationDescriptor&) const = default;
};

struct RawApiResult {
    RawEventContext context;
    RawProbeResultKind kind{RawProbeResultKind::NotObserved};
    bool success{false};
    std::uint64_t resultValue{0};
    std::uint32_t systemError{0};
    std::uint32_t pcbSizeBefore{0};
    std::uint32_t pcbSizeAfter{0};
    std::uint32_t cbSizeHeader{0};
    std::uint32_t reportedSize{0};
    std::uint32_t returnedSize{0};

    bool operator==(const RawApiResult&) const = default;
};

struct RawRegistrationSnapshot {
    RawApiResult api;
    RawApiResult sizeQuery;
    RawApiResult read;
    std::uint32_t reportedDeviceCount{0};
    std::vector<RawRegistrationDescriptor> registrations;

    bool operator==(const RawRegistrationSnapshot&) const = default;
};

struct RawRegistrationEvent {
    RawEventContext context;
    RawProbeOperation operation{RawProbeOperation::Snapshot};
    RawRegistrationDescriptor request;
    RawApiResult call;
    RawRegistrationSnapshot before;
    RawRegistrationSnapshot after;

    bool operator==(const RawRegistrationEvent&) const = default;
};

struct RawHeaderObservation {
    bool available{false};
    std::uint32_t dwType{0};
    std::uint32_t dwSize{0};
    std::uint64_t deviceRuntimeValue{0};
    std::uint64_t wParamRuntimeValue{0};
    std::string devicePath;
    std::string stableDeviceId;

    bool operator==(const RawHeaderObservation&) const = default;
};

struct RawDataQueryEvent {
    RawEventContext context;
    std::uint32_t uiCommand{0};
    RawApiResult query;
    RawApiResult read;
    RawHeaderObservation header;
    std::uint32_t totalPayloadBytes{0};

    bool operator==(const RawDataQueryEvent&) const = default;
};

struct RawBufferBlock {
    std::uint32_t offset{0};
    std::uint32_t alignedNextOffset{0};
    RawHeaderObservation header;

    bool operator==(const RawBufferBlock&) const = default;
};

struct RawBufferQueryEvent {
    RawEventContext context;
    std::uint32_t requestedBufferBytes{0};
    RawApiResult call;
    std::uint32_t returnedRawInputCount{0};
    std::vector<RawBufferBlock> blocks;

    bool operator==(const RawBufferQueryEvent&) const = default;
};

struct RawMessageEvent {
    RawEventContext context;
    RawMessageKind messageKind{RawMessageKind::Input};
    std::uint32_t messageId{0};
    std::uint32_t messageTimeMilliseconds{0};
    std::uint64_t windowRuntimeValue{0};
    std::uint64_t wParamRuntimeValue{0};
    std::uint64_t lParamRuntimeValue{0};
    RawInputCodeKind inputCode{RawInputCodeKind::Unknown};
    RawDeviceChangeKind deviceChange{RawDeviceChangeKind::Unknown};
    RawDataQueryEvent headerQuery;
    RawDataQueryEvent inputQueryAndRead;

    bool operator==(const RawMessageEvent&) const = default;
};

struct RawProbeObservation {
    std::string name;
    RawProbeResultKind result{RawProbeResultKind::NotObserved};
    std::string detail;

    bool operator==(const RawProbeObservation&) const = default;
};

struct RawInputProbeTrace {
    std::uint32_t schemaVersion{kRawInputProbeSchemaVersion};
    std::string platform{"windows"};
    RawProbeSourceKind sourceKind{RawProbeSourceKind::SyntheticParserFixture};
    std::uint16_t architectureBits{0};
    std::uint32_t processId{0};
    std::uint32_t threadId{0};
    std::uint32_t rawInputHeaderBytes{0};
    std::uint32_t rawInputBytes{0};
    std::uint32_t rawInputBufferAlignmentBytes{0};
    RawTraceLimits limits;
    bool physicalInputObserved{false};
    bool deviceChangeObserved{false};
    bool traceOverflowed{false};
    std::vector<RawRegistrationEvent> registrationEvents;
    std::vector<RawMessageEvent> messageEvents;
    std::vector<RawDataQueryEvent> dataEvents;
    std::vector<RawBufferQueryEvent> bufferEvents;
    std::vector<RawProbeObservation> observations;

    bool operator==(const RawInputProbeTrace&) const = default;
};

struct RawTraceParseResult {
    std::optional<RawInputProbeTrace> trace;
    std::string error;

    explicit operator bool() const noexcept { return trace.has_value(); }
};

struct RawDataContractInput {
    std::uint32_t headerBytes{0};
    std::uint32_t maximumPacketBytes{kMaxRawPacketBytes};
    std::uint32_t queryReturnValue{0};
    std::uint32_t querySizeAfter{0};
    std::uint32_t readReturnValue{0};
    std::uint32_t readSizeAfter{0};
    std::uint32_t suppliedBufferBytes{0};
    std::uint32_t rawDwType{0};
    std::uint32_t rawDwSize{0};
};

struct RawRegistrationContractInput {
    RawRegistrationDescriptor request;
    std::uint32_t cbSize{0};
    std::uint32_t nativeCbSize{0};
    bool controlledTargetRequired{true};
};

struct RawBufferParseResult {
    RawProbeResultKind kind{RawProbeResultKind::InvalidContract};
    std::vector<RawBufferBlock> blocks;
    std::string error;

    explicit operator bool() const noexcept {
        return kind == RawProbeResultKind::Success;
    }
};

std::string serializeRawInputProbeTrace(const RawInputProbeTrace& trace);
RawTraceParseResult parseRawInputProbeTrace(std::string_view json);
RawProbeResultKind validateRawDataContract(
    const RawDataContractInput& input) noexcept;
RawProbeResultKind validateRawRegistrationContract(
    const RawRegistrationContractInput& input) noexcept;
RawBufferParseResult parseRawInputBufferLayout(
    std::span<const std::byte> bytes,
    std::uint32_t returnedPacketCount,
    std::uint16_t architectureBits,
    std::uint32_t headerBytes,
    std::uint32_t alignmentBytes = 0);

std::string_view rawProbeSourceKindName(RawProbeSourceKind value) noexcept;
std::string_view rawProbeOperationName(RawProbeOperation value) noexcept;
std::string_view rawProbeResultKindName(RawProbeResultKind value) noexcept;

} // namespace hydra::rawprobe
