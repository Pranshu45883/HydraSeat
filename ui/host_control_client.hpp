#pragma once

#include "hydra/host_transport.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace hydra::ui {

// One persistent Control-role connection for the whole UI process.
// UI configuration leases are connection-scoped in host IPC v2, so every
// mutating UI action must pass through this object instead of constructing
// process-local runtime authority.
class HostControlClient final {
public:
    HostControlClient() = default;
    ~HostControlClient();

    HostControlClient(const HostControlClient&) = delete;
    HostControlClient& operator=(const HostControlClient&) = delete;

    bool ensureConnected(std::string* error = nullptr);
    bool connected() const noexcept;
    void close() noexcept;

    bool ownsUiLease(std::uint32_t seatId) const noexcept;

    std::optional<hostipc::HostSnapshot> snapshot(std::string* error = nullptr);
    std::optional<hostipc::HostSnapshot> acquireUiLease(
        std::uint32_t seatId,
        std::string* error = nullptr);
    std::optional<hostipc::HostSnapshot> releaseUiLease(
        std::uint32_t seatId,
        std::string* error = nullptr);
    std::optional<hostipc::HostSnapshot> pairController(
        std::uint32_t seatId,
        const std::string& persistentControllerId,
        std::uint8_t runtimeXInputSlot,
        std::string* error = nullptr);
    std::optional<hostipc::AudioMutationStatus> routeAudio(
        std::uint32_t processId,
        std::uint64_t creationIdentity,
        const std::string& endpointId,
        std::string* error = nullptr);
    std::optional<hostipc::AudioMutationStatus> resetAudio(
        std::uint32_t processId,
        std::uint64_t creationIdentity,
        std::string* error = nullptr);
    std::optional<hostipc::HostSnapshot> launchGame(
        std::uint32_t seatId,
        const std::string& titleUtf8,
        const std::string& executablePathUtf8,
        const std::string& launchArgumentsUtf8,
        const std::string& workingDirectoryUtf8,
        std::string* error = nullptr);
    std::optional<hostipc::HostSnapshot> stopGame(
        std::uint32_t seatId,
        std::string* error = nullptr);

private:
    bool ensureUiLeaseForSeat(
        std::uint32_t seatId,
        std::string* error);
    std::optional<std::uint32_t> seatForProcess(
        const hostipc::HostSnapshot& snapshot,
        std::uint32_t processId,
        std::uint64_t creationIdentity) const noexcept;
    bool ensureUiLeaseForProcess(
        std::uint32_t processId,
        std::uint64_t creationIdentity,
        std::string* error);

    hostipc::HostPipeClient client_;
    std::array<bool, hostipc::kHostSeatCount> ownedUiLeases_{};
};

} // namespace hydra::ui
