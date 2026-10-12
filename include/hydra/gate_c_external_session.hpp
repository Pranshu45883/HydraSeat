#pragma once

#include "hydra/gate_c_external_profile.hpp"
#include "hydra/gate_c_protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace hydra::gatec {

struct ExternalInputSessionOptions {
    std::uint32_t seatId{0};
    std::uintptr_t processHandle{0};
    std::uint32_t processId{0};
    std::filesystem::path artifactDirectory;
    std::uint32_t requiredApiMask{kExternalBridgeAutoDetectApiMask};
    bool enableXInputRedirect{false};
};

class ExternalInputSession final {
public:
    static std::shared_ptr<ExternalInputSession> attach(
        const ExternalInputSessionOptions& options,
        std::string* error = nullptr);

    ~ExternalInputSession();

    ExternalInputSession(const ExternalInputSession&) = delete;
    ExternalInputSession& operator=(const ExternalInputSession&) = delete;

    bool active() const noexcept;
    bool sendInput(
        const InputEventMessage& input,
        std::string* error = nullptr);
    bool sendControl(
        const ControlStateMessage& control,
        std::string* error = nullptr);
    void shutdown() noexcept;

private:
    class Impl;

    explicit ExternalInputSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace hydra::gatec
