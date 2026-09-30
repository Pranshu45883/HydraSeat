#include "hydra/gate_c_transport.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#ifdef _WIN32
#include <bcrypt.h>
#endif
#include <utility>

namespace hydra::gatec {
namespace {

#ifdef _WIN32

class ScopedEvent {
public:
    ScopedEvent() : m_handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ScopedEvent() {
        if (m_handle != nullptr) {
            CloseHandle(m_handle);
        }
    }

    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;

    HANDLE get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

private:
    HANDLE m_handle{nullptr};
};

bool isDisconnectedError(DWORD error) noexcept {
    return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
           error == ERROR_NO_DATA;
}

bool waitForOverlapped(HANDLE handle, OVERLAPPED& overlapped,
                       std::uint32_t timeoutMilliseconds,
                       DWORD& transferred, DWORD& error) {
    const DWORD wait = WaitForSingleObject(overlapped.hEvent,
                                           timeoutMilliseconds);
    if (wait == WAIT_TIMEOUT) {
        CancelIoEx(handle, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        DWORD ignored = 0;
        GetOverlappedResult(handle, &overlapped, &ignored, FALSE);
        error = ERROR_TIMEOUT;
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        const DWORD waitError =
            wait == WAIT_FAILED ? GetLastError() : ERROR_OPERATION_ABORTED;
        CancelIoEx(handle, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        DWORD ignored = 0;
        GetOverlappedResult(handle, &overlapped, &ignored, FALSE);
        error = waitError;
        return false;
    }
    if (!GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
        error = GetLastError();
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

void setOptionalError(std::string* text, std::uint32_t* systemError,
                      std::string message, std::uint32_t error) {
    if (text != nullptr) {
        *text = std::move(message);
    }
    if (systemError != nullptr) {
        *systemError = error;
    }
}

#endif

} // namespace

PipeChannel::~PipeChannel() {
    close();
}

PipeChannel::PipeChannel(PipeChannel&& other) noexcept {
#ifdef _WIN32
    m_handle = std::exchange(other.m_handle, INVALID_HANDLE_VALUE);
#else
    (void)other;
#endif
}

PipeChannel& PipeChannel::operator=(PipeChannel&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    close();
#ifdef _WIN32
    m_handle = std::exchange(other.m_handle, INVALID_HANDLE_VALUE);
#else
    (void)other;
#endif
    return *this;
}

bool PipeChannel::valid() const noexcept {
#ifdef _WIN32
    return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
#else
    return false;
#endif
}

void PipeChannel::close() noexcept {
#ifdef _WIN32
    if (valid()) {
        CancelIoEx(m_handle, nullptr);
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
#endif
}

bool PipeChannel::writeFrame(const std::vector<std::byte>& frame,
                             std::uint32_t timeoutMilliseconds,
                             std::string* error,
                             std::uint32_t* systemError) {
#ifdef _WIN32
    if (!valid()) {
        setOptionalError(error, systemError, "pipe is not open",
                         ERROR_INVALID_HANDLE);
        return false;
    }
    if (frame.empty() || frame.size() > kMaximumFrameBytes) {
        setOptionalError(error, systemError, "frame size is invalid",
                         ERROR_INVALID_DATA);
        return false;
    }

    ScopedEvent event;
    if (!event) {
        setOptionalError(error, systemError, "could not create I/O event",
                         GetLastError());
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD written = 0;
    BOOL completed = WriteFile(
        m_handle, frame.data(), static_cast<DWORD>(frame.size()),
        &written, &overlapped);
    DWORD ioError = ERROR_SUCCESS;
    if (!completed) {
        ioError = GetLastError();
        if (ioError == ERROR_IO_PENDING) {
            if (!waitForOverlapped(m_handle, overlapped,
                                   timeoutMilliseconds, written, ioError)) {
                setOptionalError(
                    error, systemError,
                    ioError == ERROR_TIMEOUT ? "pipe write timed out"
                                             : "pipe write failed",
                    ioError);
                return false;
            }
        } else {
            setOptionalError(error, systemError,
                             isDisconnectedError(ioError)
                                 ? "pipe is disconnected"
                                 : "pipe write failed",
                             ioError);
            return false;
        }
    }

    if (written != frame.size()) {
        setOptionalError(error, systemError, "partial pipe frame write",
                         ERROR_WRITE_FAULT);
        return false;
    }
    if (error != nullptr) error->clear();
    if (systemError != nullptr) *systemError = ERROR_SUCCESS;
    return true;
#else
    (void)frame;
    (void)timeoutMilliseconds;
    if (error != nullptr) *error = "named-pipe transport is Windows-only";
    if (systemError != nullptr) *systemError = 0;
    return false;
#endif
}

TransportResult PipeChannel::readFrame(
    std::uint32_t timeoutMilliseconds) {
    TransportResult result;
#ifdef _WIN32
    if (!valid()) {
        result.status = TransportStatus::IoError;
        result.systemError = ERROR_INVALID_HANDLE;
        result.error = "pipe is not open";
        return result;
    }

    std::vector<std::byte> buffer(kMaximumFrameBytes);
    ScopedEvent event;
    if (!event) {
        result.status = TransportStatus::IoError;
        result.systemError = GetLastError();
        result.error = "could not create I/O event";
        return result;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD read = 0;
    BOOL completed = ReadFile(
        m_handle, buffer.data(), static_cast<DWORD>(buffer.size()),
        &read, &overlapped);
    DWORD ioError = ERROR_SUCCESS;
    if (!completed) {
        ioError = GetLastError();
        if (ioError == ERROR_IO_PENDING) {
            if (!waitForOverlapped(m_handle, overlapped,
                                   timeoutMilliseconds, read, ioError)) {
                result.systemError = ioError;
                if (ioError == ERROR_TIMEOUT) {
                    result.status = TransportStatus::Timeout;
                    result.error = "pipe read timed out";
                } else if (ioError == ERROR_MORE_DATA) {
                    result.status = TransportStatus::ProtocolError;
                    result.error = "pipe message exceeds maximum frame size";
                } else if (isDisconnectedError(ioError)) {
                    result.status = TransportStatus::Disconnected;
                    result.error = "pipe is disconnected";
                } else {
                    result.status = TransportStatus::IoError;
                    result.error = "pipe read failed";
                }
                return result;
            }
        } else {
            result.systemError = ioError;
            if (ioError == ERROR_MORE_DATA) {
                result.status = TransportStatus::ProtocolError;
                result.error = "pipe message exceeds maximum frame size";
            } else if (isDisconnectedError(ioError)) {
                result.status = TransportStatus::Disconnected;
                result.error = "pipe is disconnected";
            } else {
                result.status = TransportStatus::IoError;
                result.error = "pipe read failed";
            }
            return result;
        }
    }

    if (read == 0) {
        result.status = TransportStatus::Disconnected;
        result.error = "pipe returned an empty message";
        return result;
    }
    buffer.resize(read);
    const auto decoded = decodeFrame(buffer);
    if (!decoded) {
        result.status = TransportStatus::ProtocolError;
        result.error = decoded.error;
        return result;
    }

    result.status = TransportStatus::Success;
    result.frame = std::move(decoded.frame);
    return result;
#else
    (void)timeoutMilliseconds;
    result.status = TransportStatus::IoError;
    result.error = "named-pipe transport is Windows-only";
    return result;
#endif
}

std::optional<SessionToken> generateSessionToken() {
    SessionToken token{};
#ifdef _WIN32
    const NTSTATUS status = BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(token.data()),
        static_cast<ULONG>(token.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return std::nullopt;
    }
#else
    try {
        std::random_device random;
        for (auto& byte : token) {
            byte = static_cast<std::uint8_t>(random() & 0xffu);
        }
    } catch (...) {
        return std::nullopt;
    }
#endif
    const bool allZero = std::all_of(
        token.begin(), token.end(),
        [](std::uint8_t byte) { return byte == 0; });
    if (allZero) {
        return std::nullopt;
    }
    return token;
}

std::wstring makeGateCPipeName(std::uint32_t hostProcessId,
                               std::uint32_t seatId,
                               const SessionToken& token) {
    const auto tokenHex = tokenToHex(token);
    std::wstring shortToken;
    shortToken.reserve(16);
    const std::size_t shortLength = tokenHex.size() < 16
                                        ? tokenHex.size()
                                        : 16;
    for (std::size_t index = 0; index < shortLength; ++index) {
        shortToken.push_back(static_cast<wchar_t>(tokenHex[index]));
    }
    return L"\\\\.\\pipe\\HydraSeat.GateC." +
           std::to_wstring(hostProcessId) + L"." +
           std::to_wstring(seatId) + L"." + shortToken;
}

#ifdef _WIN32

PipeChannel createGateCServerPipe(const std::wstring& pipeName,
                                  std::string* error,
                                  std::uint32_t* systemError) {
    const HANDLE handle = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, static_cast<DWORD>(kMaximumFrameBytes),
        static_cast<DWORD>(kMaximumFrameBytes), 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        setOptionalError(error, systemError,
                         "could not create Gate C server pipe",
                         GetLastError());
        return {};
    }
    if (error != nullptr) error->clear();
    if (systemError != nullptr) *systemError = ERROR_SUCCESS;
    return PipeChannel(handle);
}

bool waitForGateCClient(PipeChannel& channel,
                        std::uint32_t timeoutMilliseconds,
                        std::string* error,
                        std::uint32_t* systemError) {
    if (!channel.valid()) {
        setOptionalError(error, systemError, "server pipe is not open",
                         ERROR_INVALID_HANDLE);
        return false;
    }

    ScopedEvent event;
    if (!event) {
        setOptionalError(error, systemError, "could not create I/O event",
                         GetLastError());
        return false;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    const BOOL connected = ConnectNamedPipe(channel.nativeHandle(), &overlapped);
    if (connected) {
        if (error != nullptr) error->clear();
        if (systemError != nullptr) *systemError = ERROR_SUCCESS;
        return true;
    }

    DWORD connectError = GetLastError();
    if (connectError == ERROR_PIPE_CONNECTED) {
        SetEvent(event.get());
        if (error != nullptr) error->clear();
        if (systemError != nullptr) *systemError = ERROR_SUCCESS;
        return true;
    }
    if (connectError != ERROR_IO_PENDING) {
        setOptionalError(error, systemError, "pipe connection failed",
                         connectError);
        return false;
    }

    DWORD ignored = 0;
    if (!waitForOverlapped(channel.nativeHandle(), overlapped,
                           timeoutMilliseconds, ignored, connectError)) {
        setOptionalError(error, systemError,
                         connectError == ERROR_TIMEOUT
                             ? "pipe connection timed out"
                             : "pipe connection failed",
                         connectError);
        return false;
    }

    if (error != nullptr) error->clear();
    if (systemError != nullptr) *systemError = ERROR_SUCCESS;
    return true;
}

PipeChannel connectGateCClient(const std::wstring& pipeName,
                               std::uint32_t timeoutMilliseconds,
                               std::string* error,
                               std::uint32_t* systemError) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
    DWORD lastError = ERROR_FILE_NOT_FOUND;
    while (GetTickCount64() <= deadline) {
        const HANDLE handle = CreateFileW(
            pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
                lastError = GetLastError();
                CloseHandle(handle);
                setOptionalError(error, systemError,
                                 "could not enable message-mode pipe reads",
                                 lastError);
                return {};
            }
            if (error != nullptr) error->clear();
            if (systemError != nullptr) *systemError = ERROR_SUCCESS;
            return PipeChannel(handle);
        }

        lastError = GetLastError();
        if (lastError != ERROR_PIPE_BUSY &&
            lastError != ERROR_FILE_NOT_FOUND) {
            break;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            lastError = ERROR_TIMEOUT;
            break;
        }
        const ULONGLONG available = deadline - now;
        const auto remaining = static_cast<DWORD>(
            available < 100 ? available : 100);
        WaitNamedPipeW(pipeName.c_str(), remaining);
    }

    setOptionalError(error, systemError,
                     lastError == ERROR_TIMEOUT
                         ? "client pipe connection timed out"
                         : "could not connect to Gate C pipe",
                     lastError);
    return {};
}

#endif

std::string_view transportStatusName(TransportStatus status) noexcept {
    switch (status) {
    case TransportStatus::Success: return "Success";
    case TransportStatus::Timeout: return "Timeout";
    case TransportStatus::Disconnected: return "Disconnected";
    case TransportStatus::ProtocolError: return "ProtocolError";
    case TransportStatus::IoError: return "IoError";
    }
    return "Unknown";
}

} // namespace hydra::gatec
