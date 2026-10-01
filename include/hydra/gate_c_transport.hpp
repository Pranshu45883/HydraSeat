#pragma once

#include "hydra/gate_c_protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hydra::gatec {

enum class TransportStatus {
    Success,
    Timeout,
    Disconnected,
    ProtocolError,
    IoError
};

struct TransportResult {
    TransportStatus status{TransportStatus::IoError};
    std::optional<DecodedFrame> frame;
    std::uint32_t systemError{0};
    std::string error;

    explicit operator bool() const noexcept {
        return status == TransportStatus::Success;
    }
};

class PipeChannel {
public:
    PipeChannel() = default;
#ifdef _WIN32
    explicit PipeChannel(HANDLE handle) noexcept : m_handle(handle) {}
#endif
    ~PipeChannel();

    PipeChannel(const PipeChannel&) = delete;
    PipeChannel& operator=(const PipeChannel&) = delete;
    PipeChannel(PipeChannel&& other) noexcept;
    PipeChannel& operator=(PipeChannel&& other) noexcept;

    bool valid() const noexcept;
    void close() noexcept;

    bool writeFrame(const std::vector<std::byte>& frame,
                    std::uint32_t timeoutMilliseconds,
                    std::string* error = nullptr,
                    std::uint32_t* systemError = nullptr);
    TransportResult readFrame(std::uint32_t timeoutMilliseconds);

#ifdef _WIN32
    HANDLE nativeHandle() const noexcept { return m_handle; }
#endif

private:
#ifdef _WIN32
    HANDLE m_handle{INVALID_HANDLE_VALUE};
#endif
};

std::optional<SessionToken> generateSessionToken();
std::wstring makeGateCPipeName(std::uint32_t hostProcessId,
                               std::uint32_t seatId,
                               const SessionToken& token);

#ifdef _WIN32
PipeChannel createGateCServerPipe(const std::wstring& pipeName,
                                  std::string* error = nullptr,
                                  std::uint32_t* systemError = nullptr);
bool waitForGateCClient(PipeChannel& channel,
                        std::uint32_t timeoutMilliseconds,
                        std::string* error = nullptr,
                        std::uint32_t* systemError = nullptr);
PipeChannel connectGateCClient(const std::wstring& pipeName,
                               std::uint32_t timeoutMilliseconds,
                               std::string* error = nullptr,
                               std::uint32_t* systemError = nullptr);
#endif

std::string_view transportStatusName(TransportStatus status) noexcept;

} // namespace hydra::gatec
