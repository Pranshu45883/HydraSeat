#include "hydra/virtual_xinput_pipe.hpp"

#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace hydra::controller {

NamedPipeVirtualXInputServer::NamedPipeVirtualXInputServer(
    std::wstring endpoint,
    VirtualXInputService& service) noexcept
    : endpoint_(std::move(endpoint)), service_(service) {}

#if defined(_WIN32)
namespace {

class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) CloseHandle(handle_);
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }
private:
    HANDLE handle_;
};

bool waitOverlapped(HANDLE handle,
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

bool connectWithTimeout(HANDLE pipe, std::uint32_t timeoutMs) noexcept {
    ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) return false;

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &overlapped)) return true;

    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;

    DWORD transferred = 0;
    return waitOverlapped(pipe, overlapped, timeoutMs, transferred);
}

bool readExact(HANDLE handle,
               std::uint8_t* buffer,
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
        const BOOL started = ReadFile(handle, buffer + total, remaining, &transferred, &overlapped);
        if (!started) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) return false;
            if (!waitOverlapped(handle, overlapped, timeoutMs, transferred)) return false;
        }
        if (transferred == 0) return false;
        total += transferred;
    }
    return true;
}

bool writeExact(HANDLE handle,
                const std::uint8_t* buffer,
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
        const BOOL started = WriteFile(handle, buffer + total, remaining, &transferred, &overlapped);
        if (!started) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) return false;
            if (!waitOverlapped(handle, overlapped, timeoutMs, transferred)) return false;
        }
        if (transferred == 0) return false;
        total += transferred;
    }
    return true;
}

} // namespace
#endif

PipeServerResult NamedPipeVirtualXInputServer::serveOne(
    std::uint32_t timeoutMs) noexcept {
#if defined(_WIN32)
    const DWORD requestSize = static_cast<DWORD>(ipc::kEncodedRequestSize);
    const DWORD responseSize = static_cast<DWORD>(ipc::kEncodedResponseSize);
    ScopedHandle pipe(CreateNamedPipeW(
        endpoint_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        responseSize,
        requestSize,
        0,
        nullptr));
    if (!pipe.valid()) return {};
    if (!connectWithTimeout(pipe.get(), timeoutMs)) return {};

    std::vector<std::uint8_t> requestBytes(ipc::kEncodedRequestSize, 0);
    if (!readExact(pipe.get(), requestBytes.data(), requestBytes.size(), timeoutMs)) {
        DisconnectNamedPipe(pipe.get());
        return {};
    }

    ipc::VirtualXInputResponse response;
    const auto request = ipc::decodeRequest(requestBytes);
    if (request) {
        response = service_.handle(*request);
    } else {
        response.status = ipc::ProtocolStatus::InvalidRequest;
    }

    const auto responseBytes = ipc::encodeResponse(response);
    const bool wrote = writeExact(
        pipe.get(), responseBytes.data(), responseBytes.size(), timeoutMs);
    FlushFileBuffers(pipe.get());
    DisconnectNamedPipe(pipe.get());
    if (!wrote) return {};
    return {true, response.status};
#else
    (void)timeoutMs;
    return {};
#endif
}

std::optional<ipc::VirtualXInputResponse> sendVirtualXInputRequest(
    const std::wstring& endpoint,
    const ipc::VirtualXInputRequest& request,
    std::uint32_t timeoutMs) noexcept {
#if defined(_WIN32)
    const ULONGLONG start = GetTickCount64();
    HANDLE pipeHandle = INVALID_HANDLE_VALUE;
    for (;;) {
        pipeHandle = CreateFileW(
            endpoint.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        if (pipeHandle != INVALID_HANDLE_VALUE) break;

        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
            return std::nullopt;
        }
        if (timeoutMs == 0 || GetTickCount64() - start >= timeoutMs) {
            return std::nullopt;
        }
        Sleep(1);
    }
    ScopedHandle pipe(pipeHandle);

    const auto requestBytes = ipc::encodeRequest(request);
    if (!writeExact(pipe.get(), requestBytes.data(), requestBytes.size(), timeoutMs)) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> responseBytes(ipc::kEncodedResponseSize, 0);
    if (!readExact(pipe.get(), responseBytes.data(), responseBytes.size(), timeoutMs)) {
        return std::nullopt;
    }
    return ipc::decodeResponse(responseBytes);
#else
    (void)endpoint;
    (void)request;
    (void)timeoutMs;
    return std::nullopt;
#endif
}

} // namespace hydra::controller
