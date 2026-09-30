#pragma once

#include "hydra/host_protocol.hpp"
#include "hydra/runtime_host.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace hydra::hostipc {

constexpr std::uint32_t kDefaultHostPipeTimeoutMs = 5000u;
constexpr std::size_t kMaxFramesPerConnection = 4096u;

// Per-connection protocol state. UI configuration leases are connection-scoped:
// the session releases every lease it acquired when the client disconnects.
class HostConnectionSession final {
public:
    explicit HostConnectionSession(
        runtime::RuntimeHost& host,
        runtime::AudioRouter* audioRouter = nullptr) noexcept;
    ~HostConnectionSession();

    Frame handle(const Frame& request);

private:
    Frame error(std::uint64_t correlationId,
                ErrorCode code,
                std::string diagnostic) const;

    runtime::ActivationToken* uiLease(std::uint32_t seatId) noexcept;
    const runtime::ActivationToken* uiLease(std::uint32_t seatId) const noexcept;

    runtime::RuntimeHost& host_;
    runtime::AudioRouter* audioRouter_{nullptr};
    bool helloComplete_{false};
    ClientRole role_{ClientRole::ReadOnly};
    std::array<std::optional<runtime::ActivationToken>, kHostSeatCount> uiLeases_{};
};

std::wstring currentHostPipeName();

class HostPipeClient final {
public:
    HostPipeClient();
    ~HostPipeClient();

    HostPipeClient(const HostPipeClient&) = delete;
    HostPipeClient& operator=(const HostPipeClient&) = delete;
    HostPipeClient(HostPipeClient&&) noexcept;
    HostPipeClient& operator=(HostPipeClient&&) noexcept;

    bool connect(ClientRole role,
                 std::uint32_t timeoutMs = kDefaultHostPipeTimeoutMs,
                 std::string* error = nullptr);
    void close() noexcept;
    bool connected() const noexcept;

    std::optional<HostSnapshot> getSnapshot(
        std::uint32_t timeoutMs = kDefaultHostPipeTimeoutMs,
        std::string* error = nullptr);
    std::optional<HostSnapshot> acquireUiLease(
        std::uint32_t seatId,
        std::uint32_t timeoutMs = kDefaultHostPipeTimeoutMs,
        std::string* error = nullptr);
    std::optional<HostSnapshot> releaseUiLease(
        std::uint32_t seatId,
        std::uint32_t timeoutMs = kDefaultHostPipeTimeoutMs,
        std::string* error = nullptr);
    std::optional<HostSnapshot> pairController(
        std::uint32_t seatId,
        const std::string& persistentControllerId,
        std::uint8_t runtimeXInputSlot,
        std::uint32_t timeoutMs = kDefaultHostPipeTimeoutMs,
        std::string* error = nullptr);
    std::optional<AudioMutationStatus> routeAudio(
        std::uint32_t seatId,
        std::uint32_t processId,
        std::uint64_t creationIdentity,
        const std::string& endpointId,
        std::uint32_t timeoutMs = kDefaultHostPipeTimeoutMs,
        std::string* error = nullptr);
    std::optional<AudioMutationStatus> resetAudio(
        std::uint32_t seatId,
        std::uint32_t processId,
        std::uint64_t creationIdentity,
        std::uint32_t timeoutMs = kDefaultHostPipeTimeoutMs,
        std::string* error = nullptr);
    bool ping(std::uint64_t nonce,
              std::uint32_t timeoutMs = kDefaultHostPipeTimeoutMs,
              std::string* error = nullptr);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class HostPipeServer final {
public:
    explicit HostPipeServer(runtime::RuntimeHost& host);
    ~HostPipeServer();

    HostPipeServer(const HostPipeServer&) = delete;
    HostPipeServer& operator=(const HostPipeServer&) = delete;

    bool serveOne(std::uint32_t timeoutMs = kDefaultHostPipeTimeoutMs,
                  std::string* error = nullptr);
    bool serve(std::string* error = nullptr);
    void requestStop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hydra::hostipc
