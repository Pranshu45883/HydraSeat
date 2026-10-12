#include "hydra/host_transport.hpp"

#include "hydra/game_launcher.hpp"
#include "hydra/input_router.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "hydra/windows_audio_router.hpp"
#include <windows.h>
#endif

namespace hydra::hostipc {
namespace {

AudioMutationStatus toProtocolAudioStatus(
    runtime::AudioRouteStatus status) noexcept {
    switch (status) {
    case runtime::AudioRouteStatus::Success:
        return AudioMutationStatus::Success;
    case runtime::AudioRouteStatus::InvalidProcess:
        return AudioMutationStatus::InvalidProcess;
    case runtime::AudioRouteStatus::ProcessNotFound:
        return AudioMutationStatus::ProcessNotFound;
    case runtime::AudioRouteStatus::AudioSessionNotFound:
        return AudioMutationStatus::AudioSessionNotFound;
    case runtime::AudioRouteStatus::EndpointNotFound:
        return AudioMutationStatus::EndpointNotFound;
    case runtime::AudioRouteStatus::EndpointUnavailable:
        return AudioMutationStatus::EndpointUnavailable;
    case runtime::AudioRouteStatus::IdentityMismatch:
        return AudioMutationStatus::IdentityMismatch;
    case runtime::AudioRouteStatus::RoutingFailed:
        return AudioMutationStatus::RoutingFailed;
    case runtime::AudioRouteStatus::OsApiError:
        return AudioMutationStatus::OsApiError;
    }
    return AudioMutationStatus::OsApiError;
}

HardwareDeviceKind toProtocolHardwareKind(DeviceType type) noexcept {
    switch (type) {
    case DeviceType::Display:
        return HardwareDeviceKind::Display;
    case DeviceType::Keyboard:
        return HardwareDeviceKind::Keyboard;
    case DeviceType::Mouse:
        return HardwareDeviceKind::Mouse;
    case DeviceType::Controller:
        return HardwareDeviceKind::Controller;
    }
    return HardwareDeviceKind::Keyboard;
}

#if defined(_WIN32)
std::optional<std::wstring> utf8ToWide(std::string_view value) {
    if (value.empty()) return std::wstring{};
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int sourceLength = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength, nullptr, 0);
    if (required <= 0) return std::nullopt;

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        sourceLength,
        result.data(),
        required);
    if (written != required) return std::nullopt;
    return result;
}

std::optional<std::string> wideToUtf8(std::wstring_view value) {
    if (value.empty()) return std::string{};
    if (value.size() >
        static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int sourceLength = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return std::nullopt;

    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength,
        result.data(), required, nullptr, nullptr);
    if (written != required) return std::nullopt;
    return result;
}

#else
std::optional<std::wstring> utf8ToWide(std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if (ch > 0x7fu) return std::nullopt;
        result.push_back(static_cast<wchar_t>(ch));
    }
    return result;
}

std::optional<std::string> wideToUtf8(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch < 0 || ch > 0x7f) return std::nullopt;
        result.push_back(static_cast<char>(ch));
    }
    return result;
}
#endif

std::optional<HardwareInventory> toProtocolHardwareInventory(
    std::vector<DeviceInfo> devices) {
    if (devices.size() > kHostProtocolMaxHardwareDevices) return std::nullopt;

    HardwareInventory result;
    result.devices.reserve(devices.size());
    for (const auto& device : devices) {
        const auto stableId = wideToUtf8(device.id);
        const auto displayName = wideToUtf8(device.name);
        if (!stableId || stableId->empty() || !displayName) {
            return std::nullopt;
        }
        result.devices.push_back(HardwareDeviceRecord{
            toProtocolHardwareKind(device.type),
            *stableId,
            *displayName,
        });
    }
    return result;
}

std::optional<SeatHardwareAssignment> toProtocolSeatHardware(
    const runtime::SeatHardwareConfiguration& configuration) {
    const auto displayId = wideToUtf8(configuration.displayId);
    const auto keyboardId = wideToUtf8(configuration.keyboardId);
    const auto mouseId = wideToUtf8(configuration.mouseId);
    const auto controllerId = wideToUtf8(configuration.controllerId);
    if (!displayId || !keyboardId || !mouseId || !controllerId) {
        return std::nullopt;
    }
    return SeatHardwareAssignment{
        configuration.seatId,
        *displayId,
        *keyboardId,
        *mouseId,
        *controllerId,
    };
}

std::optional<runtime::SeatHardwareConfiguration> toRuntimeSeatHardware(
    const SeatHardwareAssignment& assignment) {
    const auto displayId = utf8ToWide(assignment.displayIdUtf8);
    const auto keyboardId = utf8ToWide(assignment.keyboardIdUtf8);
    const auto mouseId = utf8ToWide(assignment.mouseIdUtf8);
    const auto controllerId = utf8ToWide(assignment.controllerIdUtf8);
    if (!displayId || !keyboardId || !mouseId || !controllerId) {
        return std::nullopt;
    }
    return runtime::SeatHardwareConfiguration{
        assignment.seatId,
        *displayId,
        *keyboardId,
        *mouseId,
        *controllerId,
    };
}

} // namespace

HostConnectionSession::HostConnectionSession(
    runtime::RuntimeHost& host,
    runtime::AudioRouter* audioRouter,
    GameLauncher* gameLauncher) noexcept
    : host_(host),
      audioRouter_(audioRouter),
      gameLauncher_(gameLauncher) {}

HostConnectionSession::~HostConnectionSession() {
    for (auto& lease : uiLeases_) {
        if (lease) {
            (void)host_.releaseUiLease(*lease);
            lease.reset();
        }
    }
}

runtime::ActivationToken* HostConnectionSession::uiLease(
    std::uint32_t seatId) noexcept {
    if (seatId == 0 || seatId > uiLeases_.size()) return nullptr;
    auto& lease = uiLeases_[seatId - 1u];
    return lease ? &*lease : nullptr;
}

const runtime::ActivationToken* HostConnectionSession::uiLease(
    std::uint32_t seatId) const noexcept {
    if (seatId == 0 || seatId > uiLeases_.size()) return nullptr;
    const auto& lease = uiLeases_[seatId - 1u];
    return lease ? &*lease : nullptr;
}

Frame HostConnectionSession::error(
    std::uint64_t correlationId,
    ErrorCode code,
    std::string diagnostic) const {
    Frame response;
    response.type = MessageType::Error;
    response.correlationId = correlationId;
    response.payload = encodeError(ErrorPayload{code, std::move(diagnostic)});
    return response;
}

Frame HostConnectionSession::handle(const Frame& request) {
    if (request.correlationId == 0) {
        return error(1, ErrorCode::Malformed, "zero correlation is not permitted");
    }

    if (!helloComplete_) {
        if (request.type != MessageType::Hello) {
            return error(
                request.correlationId,
                ErrorCode::PermissionDenied,
                "hello handshake required before host requests");
        }
        const auto hello = decodeHello(request.payload);
        if (!hello) {
            return error(
                request.correlationId,
                ErrorCode::Malformed,
                "invalid hello payload");
        }
        role_ = hello->role;
        helloComplete_ = true;
        Frame response;
        response.type = MessageType::HelloAck;
        response.correlationId = request.correlationId;
        response.payload = encodeHelloAck(
            HelloAck{role_, kHostProtocolVersion,
                     static_cast<std::uint8_t>(kHostSeatCount)});
        return response;
    }

    if (isMutatingRequest(request.type) && role_ != ClientRole::Control) {
        return error(
            request.correlationId,
            ErrorCode::PermissionDenied,
            "control role is required for Seat mutation");
    }

    // Games may exit without an explicit Stop request. Reconcile completed Job
    // Objects before serving state so the UI never remains stuck on Running and
    // a Seat can be launched again after a normal game exit.
    if (gameLauncher_ != nullptr) {
        gameLauncher_->reapExitedGames();
    }

    switch (request.type) {
    case MessageType::Hello:
        return error(
            request.correlationId,
            ErrorCode::Malformed,
            "hello handshake already completed");

    case MessageType::GetSnapshot: {
        if (!request.payload.empty()) {
            return error(
                request.correlationId,
                ErrorCode::Malformed,
                "snapshot request payload must be empty");
        }
        Frame response;
        response.type = MessageType::Snapshot;
        response.correlationId = request.correlationId;
        response.payload = encodeSnapshot(host_.snapshot());
        return response;
    }

    case MessageType::GetHardwareInventory: {
        if (!request.payload.empty()) {
            return error(
                request.correlationId,
                ErrorCode::Malformed,
                "hardware inventory request payload must be empty");
        }
        const auto inventory =
            toProtocolHardwareInventory(host_.hardwareInventory());
        if (!inventory) {
            return error(
                request.correlationId,
                ErrorCode::InternalError,
                "hardware inventory could not be encoded safely");
        }
        Frame response;
        response.type = MessageType::HardwareInventory;
        response.correlationId = request.correlationId;
        response.payload = encodeHardwareInventory(*inventory);
        if (response.payload.empty() && !inventory->devices.empty()) {
            return error(
                request.correlationId,
                ErrorCode::InternalError,
                "hardware inventory exceeded protocol bounds");
        }
        return response;
    }

    case MessageType::GetSeatHardware: {
        const auto seatRequest = decodeSeatRequest(request.payload);
        if (!seatRequest) {
            return error(
                request.correlationId,
                ErrorCode::Malformed,
                "invalid Seat hardware query payload");
        }
        const auto configuration =
            host_.seatHardwareConfiguration(seatRequest->seatId);
        if (!configuration) {
            return error(
                request.correlationId,
                ErrorCode::InvalidState,
                "Seat hardware configuration is unavailable");
        }
        const auto protocolConfiguration =
            toProtocolSeatHardware(*configuration);
        if (!protocolConfiguration) {
            return error(
                request.correlationId,
                ErrorCode::InternalError,
                "Seat hardware configuration could not be encoded");
        }
        Frame response;
        response.type = MessageType::SeatHardware;
        response.correlationId = request.correlationId;
        response.payload =
            encodeSeatHardwareAssignment(*protocolConfiguration);
        return response;
    }

    case MessageType::Ping: {
        const auto nonce = decodePing(request.payload);
        if (!nonce) {
            return error(
                request.correlationId,
                ErrorCode::Malformed,
                "invalid ping payload");
        }
        Frame response;
        response.type = MessageType::Pong;
        response.correlationId = request.correlationId;
        response.payload = encodePing(*nonce);
        return response;
    }

    case MessageType::AcquireUiLease: {
        const auto requestValue = decodeSeatRequest(request.payload);
        if (!requestValue) {
            return error(
                request.correlationId, ErrorCode::Malformed,
                "invalid UI lease request payload");
        }
        if (uiLease(requestValue->seatId) != nullptr) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "this connection already owns the Seat UI lease");
        }
        const auto lease = host_.acquireUiLease(requestValue->seatId);
        if (!lease.valid()) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "Seat UI lease is already owned or unavailable");
        }
        uiLeases_[requestValue->seatId - 1u] = lease;

        Frame response;
        response.type = MessageType::AcquireUiLeaseResult;
        response.correlationId = request.correlationId;
        response.payload = encodeSnapshot(host_.snapshot());
        return response;
    }

    case MessageType::ReleaseUiLease: {
        const auto requestValue = decodeSeatRequest(request.payload);
        if (!requestValue) {
            return error(
                request.correlationId, ErrorCode::Malformed,
                "invalid UI lease release payload");
        }
        auto* lease = uiLease(requestValue->seatId);
        if (lease == nullptr) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "this connection does not own the Seat UI lease");
        }
        const auto token = *lease;
        if (!host_.releaseUiLease(token)) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "Seat UI lease became stale before release");
        }
        uiLeases_[requestValue->seatId - 1u].reset();

        Frame response;
        response.type = MessageType::ReleaseUiLeaseResult;
        response.correlationId = request.correlationId;
        response.payload = encodeSnapshot(host_.snapshot());
        return response;
    }

    case MessageType::AssignSeatHardware: {
        const auto assignment =
            decodeSeatHardwareAssignment(request.payload);
        if (!assignment) {
            return error(
                request.correlationId,
                ErrorCode::Malformed,
                "invalid Seat hardware assignment payload");
        }
        const auto* lease = uiLease(assignment->seatId);
        if (lease == nullptr) {
            return error(
                request.correlationId,
                ErrorCode::InvalidState,
                "hardware assignment requires this connection's Seat UI lease");
        }
        const auto runtimeAssignment =
            toRuntimeSeatHardware(*assignment);
        if (!runtimeAssignment) {
            return error(
                request.correlationId,
                ErrorCode::Malformed,
                "Seat hardware identifiers are not valid UTF-8");
        }
        std::string configurationError;
        if (!host_.configureSeatHardware(
                *lease,
                *runtimeAssignment,
                &configurationError)) {
            return error(
                request.correlationId,
                ErrorCode::InvalidState,
                configurationError.empty()
                    ? "current hardware inventory or cross-Seat ownership rejected the assignment"
                    : configurationError);
        }
        const auto current =
            host_.seatHardwareConfiguration(assignment->seatId);
        if (!current) {
            return error(
                request.correlationId,
                ErrorCode::InternalError,
                "Seat hardware assignment disappeared after commit");
        }
        const auto protocolCurrent = toProtocolSeatHardware(*current);
        if (!protocolCurrent) {
            return error(
                request.correlationId,
                ErrorCode::InternalError,
                "Seat hardware assignment could not be encoded");
        }

        Frame response;
        response.type = MessageType::AssignSeatHardwareResult;
        response.correlationId = request.correlationId;
        response.payload = encodeSeatHardwareAssignment(*protocolCurrent);
        return response;
    }

    case MessageType::PairController: {
        const auto pair = decodeControllerPairRequest(request.payload);
        if (!pair) {
            return error(
                request.correlationId, ErrorCode::Malformed,
                "invalid controller pairing payload");
        }
        const auto* lease = uiLease(pair->seatId);
        if (lease == nullptr) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "controller pairing requires this connection's Seat UI lease");
        }
        if (!host_.pairController(
                *lease, pair->persistentControllerId,
                pair->runtimeXInputSlot)) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "current controller inventory rejected the pairing request");
        }

        Frame response;
        response.type = MessageType::PairControllerResult;
        response.correlationId = request.correlationId;
        response.payload = encodeSnapshot(host_.snapshot());
        return response;
    }

    case MessageType::RouteAudio: {
        const auto route = decodeAudioRouteRequest(request.payload);
        if (!route) {
            return error(
                request.correlationId, ErrorCode::Malformed,
                "invalid audio route payload");
        }
        const runtime::ProcessIdentity process{
            route->process.processId,
            route->process.creationIdentity};
        const auto seatId = host_.seatForProcess(process);
        const auto* lease = seatId ? uiLease(*seatId) : nullptr;
        if (lease == nullptr) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "audio routing requires this connection's UI lease for the exact owning Seat");
        }
        if (audioRouter_ == nullptr) {
            return error(
                request.correlationId, ErrorCode::Unsupported,
                "native audio routing backend is unavailable");
        }

        const auto endpointId = utf8ToWide(route->endpointId);
        if (!endpointId || endpointId->empty()) {
            return error(
                request.correlationId,
                ErrorCode::Malformed,
                "audio endpoint identifier is not valid UTF-8");
        }
        const runtime::AudioEndpointIdentity endpoint{
            *endpointId,
            std::nullopt};
        const auto status = host_.routeAudio(
            *lease, process, endpoint, *audioRouter_);

        Frame response;
        response.type = MessageType::RouteAudioResult;
        response.correlationId = request.correlationId;
        response.payload = encodeAudioMutationResult(
            AudioMutationResult{toProtocolAudioStatus(status)});
        return response;
    }

    case MessageType::ResetAudio: {
        const auto reset = decodeProcessRequest(request.payload);
        if (!reset) {
            return error(
                request.correlationId, ErrorCode::Malformed,
                "invalid audio reset payload");
        }
        const runtime::ProcessIdentity process{
            reset->processId,
            reset->creationIdentity};
        const auto seatId = host_.seatForProcess(process);
        const auto* lease = seatId ? uiLease(*seatId) : nullptr;
        if (lease == nullptr) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "audio reset requires this connection's UI lease for the exact owning Seat");
        }
        if (audioRouter_ == nullptr) {
            return error(
                request.correlationId, ErrorCode::Unsupported,
                "native audio routing backend is unavailable");
        }

        const auto status = host_.resetAudio(
            *lease, process, *audioRouter_);

        Frame response;
        response.type = MessageType::ResetAudioResult;
        response.correlationId = request.correlationId;
        response.payload = encodeAudioMutationResult(
            AudioMutationResult{toProtocolAudioStatus(status)});
        return response;
    }

    case MessageType::LaunchGame: {
        const auto launch = decodeLaunchGameRequest(request.payload);
        if (!launch) {
            return error(
                request.correlationId, ErrorCode::Malformed,
                "invalid game launch payload");
        }
        if (uiLease(launch->seatId) == nullptr) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "game launch requires this connection's Seat UI lease");
        }
        if (gameLauncher_ == nullptr) {
            return error(
                request.correlationId, ErrorCode::Unsupported,
                "host game launcher is unavailable");
        }
#if defined(_WIN32)
        const auto title = utf8ToWide(launch->titleUtf8);
        const auto executable = utf8ToWide(launch->executablePathUtf8);
        const auto arguments = utf8ToWide(launch->launchArgumentsUtf8);
        const auto workingDirectory = utf8ToWide(launch->workingDirectoryUtf8);
        if (!title || !executable || !arguments || !workingDirectory) {
            return error(
                request.correlationId, ErrorCode::Malformed,
                "launch request contains invalid UTF-8");
        }

        GameProfile profile;
        profile.title = *title;
        profile.platform = GamePlatform::CustomExecutable;
        profile.executablePath = *executable;
        profile.launchArguments = *arguments;
        profile.workingDirectory = *workingDirectory;

        WorkspaceConfig workspace{};
        workspace.workspaceId = launch->seatId;
        if (!gameLauncher_->launchGameForWorkspace(profile, workspace)) {
            const auto diagnostic = gameLauncher_->lastError();
            return error(
                request.correlationId,
                ErrorCode::InvalidState,
                diagnostic.empty()
                    ? "game launch failed or Seat game authority is unavailable"
                    : diagnostic);
        }

        Frame response;
        response.type = MessageType::LaunchGameResult;
        response.correlationId = request.correlationId;
        response.payload = encodeSnapshot(host_.snapshot());
        return response;
#else
        return error(
            request.correlationId, ErrorCode::Unsupported,
            "game launch is available only on Windows");
#endif
    }

    case MessageType::StopGame: {
        const auto stop = decodeSeatRequest(request.payload);
        if (!stop) {
            return error(
                request.correlationId, ErrorCode::Malformed,
                "invalid game stop payload");
        }
        if (uiLease(stop->seatId) == nullptr) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "game stop requires this connection's Seat UI lease");
        }
        if (gameLauncher_ == nullptr) {
            return error(
                request.correlationId, ErrorCode::Unsupported,
                "host game launcher is unavailable");
        }
        if (!gameLauncher_->stopWorkspaceGame(stop->seatId)) {
            return error(
                request.correlationId, ErrorCode::InvalidState,
                "no owned Seat game could be stopped safely");
        }

        Frame response;
        response.type = MessageType::StopGameResult;
        response.correlationId = request.correlationId;
        response.payload = encodeSnapshot(host_.snapshot());
        return response;
    }

    default:
        return error(
            request.correlationId,
            ErrorCode::Unsupported,
            "request direction is not enabled by host protocol v3");
    }
}

namespace {

void setError(std::string* output, std::string message) {
    if (output) *output = std::move(message);
}

#if defined(_WIN32)

class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_(value) {}
    ~ScopedHandle() {
        if (valid()) CloseHandle(value_);
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept
        : value_(other.release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this == &other) return *this;
        if (valid()) CloseHandle(value_);
        value_ = other.release();
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
    HANDLE release() noexcept {
        HANDLE value = value_;
        value_ = INVALID_HANDLE_VALUE;
        return value;
    }
    bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_;
};

std::string windowsError(const char* prefix, DWORD code = GetLastError()) {
    return std::string(prefix) + " (win32=" + std::to_string(code) + ")";
}

bool waitOverlapped(
    HANDLE handle,
    OVERLAPPED& overlapped,
    std::uint32_t timeoutMs,
    DWORD& transferred) noexcept {
    const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        return GetOverlappedResult(
                   handle, &overlapped, &transferred, FALSE) != FALSE;
    }

    if (waitResult == WAIT_TIMEOUT) {
        // An OVERLAPPED and its event must remain alive until cancellation has
        // completed. Returning immediately after CancelIoEx risks a kernel I/O
        // completion writing through stack/event storage that has gone away.
        (void)CancelIoEx(handle, &overlapped);
        DWORD ignored = 0;
        (void)GetOverlappedResult(handle, &overlapped, &ignored, TRUE);
        SetLastError(ERROR_TIMEOUT);
        return false;
    }

    // WAIT_FAILED already leaves the useful Win32 error in GetLastError().
    return false;
}

bool readExact(
    HANDLE handle,
    std::byte* data,
    std::size_t size,
    std::uint32_t timeoutMs) noexcept {
    std::size_t total = 0;
    while (total < size) {
        ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) return false;

        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred = 0;
        const DWORD remaining = static_cast<DWORD>(size - total);
        const BOOL started = ReadFile(
            handle, data + total, remaining, &transferred, &overlapped);
        if (!started) {
            const DWORD code = GetLastError();
            if (code != ERROR_IO_PENDING) return false;
            if (!waitOverlapped(handle, overlapped, timeoutMs, transferred)) {
                return false;
            }
        }
        if (transferred == 0) return false;
        total += transferred;
    }
    return true;
}

bool writeExact(
    HANDLE handle,
    const std::byte* data,
    std::size_t size,
    std::uint32_t timeoutMs) noexcept {
    std::size_t total = 0;
    while (total < size) {
        ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) return false;

        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred = 0;
        const DWORD remaining = static_cast<DWORD>(size - total);
        const BOOL started = WriteFile(
            handle, data + total, remaining, &transferred, &overlapped);
        if (!started) {
            const DWORD code = GetLastError();
            if (code != ERROR_IO_PENDING) return false;
            if (!waitOverlapped(handle, overlapped, timeoutMs, transferred)) {
                return false;
            }
        }
        if (transferred == 0) return false;
        total += transferred;
    }
    return true;
}

std::uint32_t payloadSizeFromHeader(
    std::span<const std::byte> header) noexcept {
    if (header.size() != kHostProtocolHeaderBytes) return UINT32_MAX;
    std::uint32_t size = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        size |= static_cast<std::uint32_t>(
            std::to_integer<unsigned int>(header[16 + index])) << (index * 8u);
    }
    return size;
}

std::optional<Frame> readFrame(
    HANDLE handle,
    std::uint32_t timeoutMs,
    std::string* error) {
    std::vector<std::byte> bytes(kHostProtocolHeaderBytes);
    if (!readExact(handle, bytes.data(), bytes.size(), timeoutMs)) {
        setError(error, windowsError("read host frame header failed"));
        return std::nullopt;
    }

    const auto payloadSize = payloadSizeFromHeader(bytes);
    if (payloadSize > kHostProtocolMaxPayloadBytes) {
        setError(error, "host frame exceeds protocol payload bound");
        return std::nullopt;
    }

    const auto headerSize = bytes.size();
    bytes.resize(headerSize + payloadSize);
    if (payloadSize != 0 &&
        !readExact(handle, bytes.data() + headerSize, payloadSize, timeoutMs)) {
        setError(error, windowsError("read host frame payload failed"));
        return std::nullopt;
    }

    DecodeResult decoded;
    const auto frame = decodeFrame(bytes, &decoded);
    if (!frame) {
        setError(error, "host frame decode failed: " + decoded.diagnostic);
    }
    return frame;
}

bool writeFrame(
    HANDLE handle,
    const Frame& frame,
    std::uint32_t timeoutMs,
    std::string* error) {
    const auto bytes = encodeFrame(frame);
    if (bytes.empty()) {
        setError(error, "host frame encode failed");
        return false;
    }
    if (!writeExact(handle, bytes.data(), bytes.size(), timeoutMs)) {
        setError(error, windowsError("write host frame failed"));
        return false;
    }
    return true;
}

bool connectServerPipe(
    HANDLE pipe,
    std::uint32_t timeoutMs,
    std::string* error) {
    ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        setError(error, windowsError("create pipe connect event failed"));
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &overlapped)) return true;

    const DWORD code = GetLastError();
    if (code == ERROR_PIPE_CONNECTED) return true;
    if (code != ERROR_IO_PENDING) {
        setError(error, windowsError("ConnectNamedPipe failed", code));
        return false;
    }

    DWORD transferred = 0;
    if (!waitOverlapped(pipe, overlapped, timeoutMs, transferred)) {
        setError(error, "timeout waiting for host client");
        return false;
    }
    return true;
}

HANDLE openClientPipe(std::uint32_t timeoutMs, std::string* error) {
    const auto endpoint = currentHostPipeName();
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        HANDLE pipe = CreateFileW(
            endpoint.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;

        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PIPE_BUSY) {
            setError(error, windowsError("open host pipe failed", code));
            return INVALID_HANDLE_VALUE;
        }
        if (timeoutMs == 0 || GetTickCount64() - start >= timeoutMs) {
            setError(error, "timeout opening host pipe");
            return INVALID_HANDLE_VALUE;
        }
        Sleep(1);
    }
}

#endif

} // namespace

std::wstring currentHostPipeName() {
#if defined(_WIN32)
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
        return {};
    }
    return L"\\\\.\\pipe\\HydraSeat.Host.v3." + std::to_wstring(sessionId);
#else
    return {};
#endif
}

class HostPipeClient::Impl final {
public:
#if defined(_WIN32)
    HANDLE handle{INVALID_HANDLE_VALUE};
#endif
    std::uint64_t nextCorrelation{1};

    ~Impl() {
        close();
    }

    void close() noexcept {
#if defined(_WIN32)
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
#endif
        nextCorrelation = 1;
    }

    bool connected() const noexcept {
#if defined(_WIN32)
        return handle != INVALID_HANDLE_VALUE && handle != nullptr;
#else
        return false;
#endif
    }

    std::optional<Frame> transact(
        MessageType type,
        std::vector<std::byte> payload,
        std::uint32_t timeoutMs,
        std::string* error) {
#if defined(_WIN32)
        if (!connected()) {
            setError(error, "host pipe client is not connected");
            return std::nullopt;
        }
        if (nextCorrelation == 0) {
            setError(error, "host correlation space exhausted");
            return std::nullopt;
        }

        const std::uint64_t correlation = nextCorrelation++;
        Frame request{type, correlation, std::move(payload)};
        if (!writeFrame(handle, request, timeoutMs, error)) {
            close();
            return std::nullopt;
        }

        auto response = readFrame(handle, timeoutMs, error);
        if (!response) {
            close();
            return std::nullopt;
        }
        if (response->correlationId != correlation) {
            setError(error, "host response correlation mismatch");
            close();
            return std::nullopt;
        }
        return response;
#else
        (void)type;
        (void)payload;
        (void)timeoutMs;
        setError(error, "host pipe transport is available only on Windows");
        return std::nullopt;
#endif
    }
};

HostPipeClient::HostPipeClient() : impl_(std::make_unique<Impl>()) {}
HostPipeClient::~HostPipeClient() {
    close();
}
HostPipeClient::HostPipeClient(HostPipeClient&&) noexcept = default;
HostPipeClient& HostPipeClient::operator=(HostPipeClient&&) noexcept = default;

bool HostPipeClient::connect(
    ClientRole role,
    std::uint32_t timeoutMs,
    std::string* error) {
    close();
#if defined(_WIN32)
    if (currentHostPipeName().empty()) {
        setError(error, "unable to resolve current Windows session");
        return false;
    }

    impl_->handle = openClientPipe(timeoutMs, error);
    if (!impl_->connected()) return false;

    const auto response = impl_->transact(
        MessageType::Hello, encodeHello(Hello{role}), timeoutMs, error);
    if (!response || response->type != MessageType::HelloAck) {
        if (response && response->type == MessageType::Error) {
            const auto protocolError = decodeError(response->payload);
            setError(
                error,
                protocolError ? protocolError->diagnostic
                              : "host rejected hello handshake");
        } else if (response) {
            setError(error, "unexpected host hello response");
        }
        close();
        return false;
    }

    const auto ack = decodeHelloAck(response->payload);
    if (!ack || ack->role != role ||
        ack->protocolVersion != kHostProtocolVersion ||
        ack->seatCount != kHostSeatCount) {
        setError(error, "invalid host hello acknowledgement");
        close();
        return false;
    }
    return true;
#else
    (void)role;
    (void)timeoutMs;
    setError(error, "host pipe transport is available only on Windows");
    return false;
#endif
}

void HostPipeClient::close() noexcept {
    if (impl_) impl_->close();
}

bool HostPipeClient::connected() const noexcept {
    return impl_ && impl_->connected();
}

std::optional<HostSnapshot> HostPipeClient::getSnapshot(
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto response = impl_->transact(
        MessageType::GetSnapshot, {}, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(
            error,
            protocolError ? protocolError->diagnostic
                          : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::Snapshot) {
        setError(error, "unexpected host snapshot response");
        return std::nullopt;
    }
    const auto snapshot = decodeSnapshot(response->payload);
    if (!snapshot) setError(error, "invalid host snapshot payload");
    return snapshot;
}

std::optional<HostSnapshot> HostPipeClient::acquireUiLease(
    std::uint32_t seatId,
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto payload = encodeSeatRequest(SeatRequest{seatId});
    if (payload.empty()) {
        setError(error, "invalid Seat id for UI lease");
        return std::nullopt;
    }
    const auto response = impl_->transact(
        MessageType::AcquireUiLease, payload, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(error, protocolError ? protocolError->diagnostic
                                      : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::AcquireUiLeaseResult) {
        setError(error, "unexpected UI lease acquire response");
        return std::nullopt;
    }
    const auto snapshot = decodeSnapshot(response->payload);
    if (!snapshot) setError(error, "invalid UI lease acquire snapshot");
    return snapshot;
}

std::optional<HostSnapshot> HostPipeClient::releaseUiLease(
    std::uint32_t seatId,
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto payload = encodeSeatRequest(SeatRequest{seatId});
    if (payload.empty()) {
        setError(error, "invalid Seat id for UI lease release");
        return std::nullopt;
    }
    const auto response = impl_->transact(
        MessageType::ReleaseUiLease, payload, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(error, protocolError ? protocolError->diagnostic
                                      : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::ReleaseUiLeaseResult) {
        setError(error, "unexpected UI lease release response");
        return std::nullopt;
    }
    const auto snapshot = decodeSnapshot(response->payload);
    if (!snapshot) setError(error, "invalid UI lease release snapshot");
    return snapshot;
}

std::optional<HardwareInventory> HostPipeClient::getHardwareInventory(
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto response = impl_->transact(
        MessageType::GetHardwareInventory, {}, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(error, protocolError ? protocolError->diagnostic
                                      : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::HardwareInventory) {
        setError(error, "unexpected hardware inventory response");
        return std::nullopt;
    }
    const auto inventory = decodeHardwareInventory(response->payload);
    if (!inventory) setError(error, "invalid hardware inventory payload");
    return inventory;
}

std::optional<SeatHardwareAssignment> HostPipeClient::getSeatHardware(
    std::uint32_t seatId,
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto payload = encodeSeatRequest(SeatRequest{seatId});
    if (payload.empty()) {
        setError(error, "invalid Seat id for hardware query");
        return std::nullopt;
    }
    const auto response = impl_->transact(
        MessageType::GetSeatHardware, payload, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(error, protocolError ? protocolError->diagnostic
                                      : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::SeatHardware) {
        setError(error, "unexpected Seat hardware response");
        return std::nullopt;
    }
    const auto assignment = decodeSeatHardwareAssignment(response->payload);
    if (!assignment) setError(error, "invalid Seat hardware payload");
    return assignment;
}

std::optional<SeatHardwareAssignment> HostPipeClient::assignSeatHardware(
    const SeatHardwareAssignment& assignment,
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto payload = encodeSeatHardwareAssignment(assignment);
    if (payload.empty()) {
        setError(error, "invalid Seat hardware assignment");
        return std::nullopt;
    }
    const auto response = impl_->transact(
        MessageType::AssignSeatHardware, payload, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(error, protocolError ? protocolError->diagnostic
                                      : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::AssignSeatHardwareResult) {
        setError(error, "unexpected Seat hardware assignment response");
        return std::nullopt;
    }
    const auto current = decodeSeatHardwareAssignment(response->payload);
    if (!current) setError(error, "invalid Seat hardware assignment payload");
    return current;
}

std::optional<HostSnapshot> HostPipeClient::pairController(
    std::uint32_t seatId,
    const std::string& persistentControllerId,
    std::uint8_t runtimeXInputSlot,
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto payload = encodeControllerPairRequest(
        ControllerPairRequest{
            seatId, runtimeXInputSlot, persistentControllerId});
    if (payload.empty()) {
        setError(error, "invalid controller pairing request");
        return std::nullopt;
    }
    const auto response = impl_->transact(
        MessageType::PairController, payload, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(error, protocolError ? protocolError->diagnostic
                                      : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::PairControllerResult) {
        setError(error, "unexpected controller pairing response");
        return std::nullopt;
    }
    const auto snapshot = decodeSnapshot(response->payload);
    if (!snapshot) setError(error, "invalid controller pairing snapshot");
    return snapshot;
}

std::optional<AudioMutationStatus> HostPipeClient::routeAudio(
    std::uint32_t processId,
    std::uint64_t creationIdentity,
    const std::string& endpointId,
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto payload = encodeAudioRouteRequest(AudioRouteRequest{
        ProcessRequest{processId, creationIdentity},
        endpointId});
    if (payload.empty()) {
        setError(error, "invalid audio route request");
        return std::nullopt;
    }
    const auto response = impl_->transact(
        MessageType::RouteAudio, payload, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(error, protocolError ? protocolError->diagnostic
                                      : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::RouteAudioResult) {
        setError(error, "unexpected audio route response");
        return std::nullopt;
    }
    const auto result = decodeAudioMutationResult(response->payload);
    if (!result) {
        setError(error, "invalid audio route result payload");
        return std::nullopt;
    }
    return result->status;
}

std::optional<AudioMutationStatus> HostPipeClient::resetAudio(
    std::uint32_t processId,
    std::uint64_t creationIdentity,
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto payload =
        encodeProcessRequest(ProcessRequest{processId, creationIdentity});
    if (payload.empty()) {
        setError(error, "invalid audio reset request");
        return std::nullopt;
    }
    const auto response = impl_->transact(
        MessageType::ResetAudio, payload, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(error, protocolError ? protocolError->diagnostic
                                      : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::ResetAudioResult) {
        setError(error, "unexpected audio reset response");
        return std::nullopt;
    }
    const auto result = decodeAudioMutationResult(response->payload);
    if (!result) {
        setError(error, "invalid audio reset result payload");
        return std::nullopt;
    }
    return result->status;
}

std::optional<HostSnapshot> HostPipeClient::launchGame(
    const LaunchGameRequest& request,
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto payload = encodeLaunchGameRequest(request);
    if (payload.empty()) {
        setError(error, "invalid game launch request");
        return std::nullopt;
    }
    const auto response = impl_->transact(
        MessageType::LaunchGame, payload, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(error, protocolError ? protocolError->diagnostic
                                      : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::LaunchGameResult) {
        setError(error, "unexpected game launch response");
        return std::nullopt;
    }
    const auto snapshot = decodeSnapshot(response->payload);
    if (!snapshot) setError(error, "invalid game launch snapshot");
    return snapshot;
}

std::optional<HostSnapshot> HostPipeClient::stopGame(
    std::uint32_t seatId,
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return std::nullopt;
    const auto payload = encodeSeatRequest(SeatRequest{seatId});
    if (payload.empty()) {
        setError(error, "invalid Seat id for game stop");
        return std::nullopt;
    }
    const auto response = impl_->transact(
        MessageType::StopGame, payload, timeoutMs, error);
    if (!response) return std::nullopt;
    if (response->type == MessageType::Error) {
        const auto protocolError = decodeError(response->payload);
        setError(error, protocolError ? protocolError->diagnostic
                                      : "host returned malformed error response");
        return std::nullopt;
    }
    if (response->type != MessageType::StopGameResult) {
        setError(error, "unexpected game stop response");
        return std::nullopt;
    }
    const auto snapshot = decodeSnapshot(response->payload);
    if (!snapshot) setError(error, "invalid game stop snapshot");
    return snapshot;
}

bool HostPipeClient::ping(
    std::uint64_t nonce,
    std::uint32_t timeoutMs,
    std::string* error) {
    if (!impl_) return false;
    const auto payload = encodePing(nonce);
    if (payload.empty()) {
        setError(error, "ping nonce must be nonzero");
        return false;
    }

    const auto response = impl_->transact(
        MessageType::Ping, payload, timeoutMs, error);
    if (!response) return false;
    if (response->type != MessageType::Pong) {
        setError(error, "unexpected host ping response");
        return false;
    }
    const auto echoed = decodePing(response->payload);
    if (!echoed || *echoed != nonce) {
        setError(error, "host ping nonce mismatch");
        return false;
    }
    return true;
}

class HostPipeServer::Impl final {
public:
    explicit Impl(runtime::RuntimeHost& hostValue)
        : host(hostValue), gameLauncher(hostValue) {
#if defined(_WIN32)
        inputThread = std::jthread([this](std::stop_token stopToken) {
            InputRouter router;
            router.setIsolationMode(true);
            router.setGlobalCallback([this](const RawInputEvent& event) {
                if (event.rawDevType == RIM_TYPEKEYBOARD) {
                    host.observePhysicalInput(
                        DeviceType::Keyboard, event.devicePath);
                } else if (event.rawDevType == RIM_TYPEMOUSE) {
                    host.observePhysicalInput(
                        DeviceType::Mouse, event.devicePath);
                }
                (void)gameLauncher.routePhysicalInput(event);
            });

            const bool initialized = router.initialize();
            {
                std::lock_guard lock(inputStartupMutex);
                inputStartupComplete = true;
                inputStartupOk = initialized;
                inputStartupError = initialized
                    ? std::string{}
                    : "failed to initialize the host Raw Input router";
            }
            inputStartupCv.notify_all();
            if (!initialized) return;

            while (!stopToken.stop_requested()) {
                router.processMessages();
                Sleep(1);
            }
            router.stop();
        });

        std::unique_lock lock(inputStartupMutex);
        if (!inputStartupCv.wait_for(
                lock,
                std::chrono::seconds(3),
                [this] { return inputStartupComplete; })) {
            inputStartupError = "timed out initializing the host Raw Input router";
        }
#endif
    }

    ~Impl() {
#if defined(_WIN32)
        stopRequested.store(true, std::memory_order_release);
        inputThread.request_stop();
        cancelActiveConnections();
        joinConnectionWorkers();
#endif
    }

    runtime::RuntimeHost& host;
    GameLauncher gameLauncher;
#if defined(_WIN32)
    windows::WindowsAudioRouter audioRouter;
    std::mutex inputStartupMutex;
    std::condition_variable inputStartupCv;
    bool inputStartupComplete{false};
    bool inputStartupOk{false};
    std::string inputStartupError;
    std::jthread inputThread;

    struct ConnectionWorker {
        std::shared_ptr<std::atomic<bool>> finished;
        std::jthread thread;
    };

    std::mutex connectionWorkersMutex;
    std::vector<ConnectionWorker> connectionWorkers;
    std::mutex activePipesMutex;
    std::vector<HANDLE> activePipes;
#endif
    std::atomic<bool> stopRequested{false};

#if defined(_WIN32)
    bool inputReady(std::string* error) {
        std::lock_guard lock(inputStartupMutex);
        if (inputStartupComplete && inputStartupOk) return true;
        setError(
            error,
            inputStartupError.empty()
                ? "host Raw Input router is unavailable"
                : inputStartupError);
        return false;
    }

    ScopedHandle createServerPipe(std::string* error) {
        const auto endpoint = currentHostPipeName();
        if (endpoint.empty()) {
            setError(error, "unable to resolve current Windows session");
            return ScopedHandle{};
        }

        const DWORD bufferBytes = static_cast<DWORD>(
            kHostProtocolHeaderBytes + kHostProtocolMaxPayloadBytes);
        ScopedHandle pipe(CreateNamedPipeW(
            endpoint.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            bufferBytes,
            bufferBytes,
            0,
            nullptr));
        if (!pipe.valid()) {
            setError(error, windowsError("CreateNamedPipeW failed"));
        }
        return pipe;
    }

    void registerActivePipe(HANDLE pipe) {
        std::lock_guard lock(activePipesMutex);
        activePipes.push_back(pipe);
    }

    void unregisterActivePipe(HANDLE pipe) {
        std::lock_guard lock(activePipesMutex);
        std::erase(activePipes, pipe);
    }

    void cancelActiveConnections() noexcept {
        std::lock_guard lock(activePipesMutex);
        for (const HANDLE pipe : activePipes) {
            if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) continue;
            CancelIoEx(pipe, nullptr);
            DisconnectNamedPipe(pipe);
        }
    }

    void reapConnectionWorkers() {
        std::vector<std::jthread> completed;
        {
            std::lock_guard lock(connectionWorkersMutex);
            for (auto it = connectionWorkers.begin();
                 it != connectionWorkers.end();) {
                if (!it->finished->load(std::memory_order_acquire)) {
                    ++it;
                    continue;
                }
                completed.push_back(std::move(it->thread));
                it = connectionWorkers.erase(it);
            }
        }
        for (auto& thread : completed) {
            if (thread.joinable()) thread.join();
        }
    }

    void joinConnectionWorkers() {
        std::vector<std::jthread> workers;
        {
            std::lock_guard lock(connectionWorkersMutex);
            workers.reserve(connectionWorkers.size());
            for (auto& worker : connectionWorkers) {
                worker.thread.request_stop();
                workers.push_back(std::move(worker.thread));
            }
            connectionWorkers.clear();
        }
        for (auto& thread : workers) {
            if (thread.joinable()) thread.join();
        }
    }

    bool serveConnected(
        ScopedHandle pipe,
        std::uint32_t ioTimeoutMs,
        std::string* error) {
        if (!pipe.valid()) {
            setError(error, "connected host pipe is invalid");
            return false;
        }

        const HANDLE nativePipe = pipe.get();
        registerActivePipe(nativePipe);
        struct ActivePipeRegistration {
            Impl& owner;
            HANDLE pipe;
            ~ActivePipeRegistration() { owner.unregisterActivePipe(pipe); }
        } activeRegistration{*this, nativePipe};

        if (stopRequested.load(std::memory_order_acquire)) {
            return true;
        }

        // Windows 11 is the v1 release target. Per-process routing uses the
        // AudioPolicyConfig family used by established Windows audio tools.
        // Exact process identity and active endpoint state are still validated
        // before every mutation, and unsupported OS behavior fails closed.
        auto session = std::make_unique<HostConnectionSession>(
            host,
            &audioRouter,
            &gameLauncher);

        bool result = true;
        bool handledAny = false;
        std::string localError;
        for (;;) {
            localError.clear();
            const auto request = readFrame(nativePipe, ioTimeoutMs, &localError);
            if (!request) {
                if (stopRequested.load(std::memory_order_acquire) ||
                    handledAny) {
                    result = true;
                } else {
                    setError(error, std::move(localError));
                    result = false;
                }
                break;
            }
            handledAny = true;

            // RuntimeHost and GameLauncher own their own synchronization.
            // Do not serialize every client request behind launch/stop work:
            // a long game/window operation must not make the read-only poller
            // time out and report the canonical host as unavailable.
            Frame response = session->handle(*request);
            if (!writeFrame(nativePipe, response, ioTimeoutMs, &localError)) {
                if (stopRequested.load(std::memory_order_acquire)) {
                    result = true;
                } else {
                    setError(error, std::move(localError));
                    result = false;
                }
                break;
            }
        }

        // RuntimeHost serializes connection-scoped lease release internally.
        session.reset();

        if (!stopRequested.load(std::memory_order_acquire)) {
            FlushFileBuffers(nativePipe);
        }
        DisconnectNamedPipe(nativePipe);
        return result;
    }

    bool dispatchConnectedPipe(ScopedHandle pipe, std::string* error) {
        auto finished = std::make_shared<std::atomic<bool>>(false);
        try {
            std::jthread thread(
                [this, finished, pipe = std::move(pipe)](
                    std::stop_token stopToken) mutable {
                    std::string ignored;
                    if (!stopToken.stop_requested() &&
                        !stopRequested.load(std::memory_order_acquire)) {
                        // Production clients are persistent. INFINITE applies
                        // only to the per-connection read/write wait; requestStop
                        // cancels pending I/O explicitly.
                        (void)serveConnected(
                            std::move(pipe), INFINITE, &ignored);
                    }
                    finished->store(true, std::memory_order_release);
                });

            std::lock_guard lock(connectionWorkersMutex);
            connectionWorkers.push_back(
                ConnectionWorker{std::move(finished), std::move(thread)});
            return true;
        } catch (...) {
            setError(error, "failed to create host client worker thread");
            return false;
        }
    }
#endif

    bool serveOne(std::uint32_t timeoutMs, std::string* error) {
#if defined(_WIN32)
        if (!inputReady(error)) return false;

        auto pipe = createServerPipe(error);
        if (!pipe.valid()) return false;
        if (!connectServerPipe(pipe.get(), timeoutMs, error)) {
            return false;
        }
        return serveConnected(std::move(pipe), timeoutMs, error);
#else
        (void)timeoutMs;
        setError(error, "host pipe transport is available only on Windows");
        return false;
#endif
    }

    bool serve(std::string* error) {
#if defined(_WIN32)
        if (!inputReady(error)) return false;

        while (!stopRequested.load(std::memory_order_acquire)) {
            reapConnectionWorkers();

            auto pipe = createServerPipe(error);
            if (!pipe.valid()) {
                cancelActiveConnections();
                joinConnectionWorkers();
                return false;
            }

            std::string connectError;
            if (!connectServerPipe(pipe.get(), 250, &connectError)) {
                if (stopRequested.load(std::memory_order_acquire)) break;
                if (connectError == "timeout waiting for host client") {
                    continue;
                }
                setError(error, std::move(connectError));
                cancelActiveConnections();
                joinConnectionWorkers();
                return false;
            }

            if (!dispatchConnectedPipe(std::move(pipe), error)) {
                cancelActiveConnections();
                joinConnectionWorkers();
                return false;
            }
        }

        cancelActiveConnections();
        joinConnectionWorkers();
        return true;
#else
        setError(error, "host pipe transport is available only on Windows");
        return false;
#endif
    }

    void requestStop() noexcept {
        stopRequested.store(true, std::memory_order_release);
#if defined(_WIN32)
        inputThread.request_stop();
        cancelActiveConnections();
#endif
    }
};

HostPipeServer::HostPipeServer(runtime::RuntimeHost& host)
    : impl_(std::make_unique<Impl>(host)) {}

HostPipeServer::~HostPipeServer() {
    requestStop();
}

bool HostPipeServer::serveOne(std::uint32_t timeoutMs, std::string* error) {
    if (!impl_) {
        setError(error, "host pipe server is unavailable");
        return false;
    }
    return impl_->serveOne(timeoutMs, error);
}

bool HostPipeServer::serve(std::string* error) {
    if (!impl_) {
        setError(error, "host pipe server is unavailable");
        return false;
    }
    return impl_->serve(error);
}
void HostPipeServer::requestStop() noexcept {
    if (impl_) impl_->requestStop();
}


} // namespace hydra::hostipc
