#include "hydra/host_transport.hpp"

#include <atomic>
#include <chrono>
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

std::wstring widenAscii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

#if defined(_WIN32)
bool experimentalAudioPolicyEnabled() noexcept {
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(
        L"HYDRA_EXPERIMENTAL_AUDIO_POLICY", value,
        static_cast<DWORD>(std::size(value)));
    return length == 1 && value[0] == L'1';
}
#endif

} // namespace

HostConnectionSession::HostConnectionSession(
    runtime::RuntimeHost& host,
    runtime::AudioRouter* audioRouter) noexcept
    : host_(host), audioRouter_(audioRouter) {}

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

        const runtime::AudioEndpointIdentity endpoint{
            widenAscii(route->endpointId),
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

    default:
        return error(
            request.correlationId,
            ErrorCode::Unsupported,
            "request direction is not enabled by host protocol v2");
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
    HANDLE get() const noexcept { return value_; }
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
    if (waitResult != WAIT_OBJECT_0) {
        CancelIoEx(handle, &overlapped);
        return false;
    }
    return GetOverlappedResult(handle, &overlapped, &transferred, FALSE) != FALSE;
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
    return L"\\\\.\\pipe\\HydraSeat.Host.v2." + std::to_wstring(sessionId);
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
    explicit Impl(runtime::RuntimeHost& hostValue) noexcept : host(hostValue) {}

    runtime::RuntimeHost& host;
#if defined(_WIN32)
    windows::WindowsAudioRouter audioRouter;
#endif
    std::atomic<bool> stopRequested{false};

    bool serveOne(std::uint32_t timeoutMs, std::string* error) {
#if defined(_WIN32)
        const auto endpoint = currentHostPipeName();
        if (endpoint.empty()) {
            setError(error, "unable to resolve current Windows session");
            return false;
        }

        const DWORD bufferBytes = static_cast<DWORD>(
            kHostProtocolHeaderBytes + kHostProtocolMaxPayloadBytes);
        ScopedHandle pipe(CreateNamedPipeW(
            endpoint.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1,
            bufferBytes,
            bufferBytes,
            0,
            nullptr));
        if (!pipe.valid()) {
            setError(error, windowsError("CreateNamedPipeW failed"));
            return false;
        }

        if (!connectServerPipe(pipe.get(), timeoutMs, error)) {
            return false;
        }

        // The Windows AudioPolicyConfig factory is undocumented and has not yet
        // passed HydraSeat's physical receiver-verification/rollback gate. Keep
        // the implementation available for controlled experiments, but fail
        // closed in normal production runs.
        HostConnectionSession session(
            host,
            experimentalAudioPolicyEnabled() ? &audioRouter : nullptr);
        std::size_t handled = 0;
        for (; handled < kMaxFramesPerConnection; ++handled) {
            std::string readError;
            const auto request = readFrame(pipe.get(), timeoutMs, &readError);
            if (!request) {
                if (handled != 0) {
                    DisconnectNamedPipe(pipe.get());
                    return true;
                }
                setError(error, std::move(readError));
                DisconnectNamedPipe(pipe.get());
                return false;
            }

            const auto response = session.handle(*request);
            if (!writeFrame(pipe.get(), response, timeoutMs, error)) {
                DisconnectNamedPipe(pipe.get());
                return false;
            }
        }

        FlushFileBuffers(pipe.get());
        DisconnectNamedPipe(pipe.get());
        return true;
#else
        (void)timeoutMs;
        setError(error, "host pipe transport is available only on Windows");
        return false;
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

#if defined(_WIN32)
    while (!impl_->stopRequested.load(std::memory_order_acquire)) {
        std::string localError;
        if (impl_->serveOne(250, &localError)) {
            continue;
        }
        if (impl_->stopRequested.load(std::memory_order_acquire)) {
            return true;
        }
        if (localError == "timeout waiting for host client") {
            continue;
        }
        setError(error, std::move(localError));
        return false;
    }
    return true;
#else
    setError(error, "host pipe transport is available only on Windows");
    return false;
#endif
}

void HostPipeServer::requestStop() noexcept {
    if (impl_) {
        impl_->stopRequested.store(true, std::memory_order_release);
    }
}

} // namespace hydra::hostipc
