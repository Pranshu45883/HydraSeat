#pragma once

#include "hydra/host_protocol.hpp"
#include "hydra/runtime_host.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace hydra::hostipc {

constexpr std::uint32_t kDefaultHostPipeTimeoutMs = 5000u;
constexpr std::size_t kMaxFramesPerConnection = 32u;

// Per-connection protocol state. The first frame must be Hello; after that this
// transport layer remains read-only until trusted launch/lifecycle commands land
// in a dependent backend layer.
class HostConnectionSession final {
public:
    explicit HostConnectionSession(runtime::RuntimeHost& host) noexcept;

    Frame handle(const Frame& request);

private:
    Frame error(std::uint64_t correlationId,
                ErrorCode code,
                std::string diagnostic) const;

    runtime::RuntimeHost& host_;
    bool helloComplete_{false};
    ClientRole role_{ClientRole::ReadOnly};
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
