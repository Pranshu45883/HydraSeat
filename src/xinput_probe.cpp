#include "hydra/xinput_probe.hpp"
#include "hydra/controller_virtual_xinput.hpp"

#include <limits>
#include <ostream>
#include <type_traits>

namespace hydra::controller::probe {
namespace {

template <typename T>
bool parseDecimal(const std::wstring& text, T& value) noexcept {
    static_assert(std::is_unsigned_v<T>);
    if (text.empty()) return false;

    T parsed = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        const T digit = static_cast<T>(ch - L'0');
        if (parsed > (std::numeric_limits<T>::max() - digit) / 10) return false;
        parsed = static_cast<T>(parsed * 10 + digit);
    }
    value = parsed;
    return true;
}

} // namespace

std::optional<ProbeOptions> parseProbeArgs(
    std::span<const std::wstring> args) noexcept {
    ProbeOptions options;
    bool havePipe = false;
    bool haveSeat = false;
    bool haveActivation = false;
    bool haveSource = false;
    bool haveMode = false;
    bool haveLow = false;
    bool haveHigh = false;

    for (std::size_t i = 0; i < args.size();) {
        if (i + 1 >= args.size()) return std::nullopt;
        const auto& key = args[i];
        const auto& value = args[i + 1];
        i += 2;

        if (key == L"--pipe") {
            if (havePipe || value.empty()) return std::nullopt;
            options.pipeEndpoint = value;
            havePipe = true;
        } else if (key == L"--seat") {
            if (haveSeat || !parseDecimal(value, options.seatId)) return std::nullopt;
            haveSeat = true;
        } else if (key == L"--activation-generation") {
            if (haveActivation || !parseDecimal(value, options.activationGeneration)) {
                return std::nullopt;
            }
            haveActivation = true;
        } else if (key == L"--source-generation") {
            if (haveSource || !parseDecimal(value, options.sourceGeneration)) {
                return std::nullopt;
            }
            haveSource = true;
        } else if (key == L"--mode") {
            if (haveMode) return std::nullopt;
            if (value == L"snapshot") {
                options.mode = ProbeMode::Snapshot;
            } else if (value == L"vibrate") {
                options.mode = ProbeMode::Vibrate;
            } else {
                return std::nullopt;
            }
            haveMode = true;
        } else if (key == L"--low") {
            if (haveLow || !parseDecimal(value, options.lowFrequencyMotor)) {
                return std::nullopt;
            }
            haveLow = true;
        } else if (key == L"--high") {
            if (haveHigh || !parseDecimal(value, options.highFrequencyMotor)) {
                return std::nullopt;
            }
            haveHigh = true;
        } else {
            return std::nullopt;
        }
    }

    if (!havePipe || !haveSeat || !haveActivation || !haveSource || !haveMode) {
        return std::nullopt;
    }
    if ((options.seatId != 1 && options.seatId != 2) ||
        options.activationGeneration == 0 || options.sourceGeneration == 0) {
        return std::nullopt;
    }
    if (options.mode == ProbeMode::Vibrate) {
        if (!haveLow || !haveHigh) return std::nullopt;
    } else if (haveLow || haveHigh) {
        return std::nullopt;
    }
    return options;
}

std::string protocolStatusName(ipc::ProtocolStatus status) {
    switch (status) {
        case ipc::ProtocolStatus::Ok:
            return "Ok";
        case ipc::ProtocolStatus::InvalidRequest:
            return "InvalidRequest";
        case ipc::ProtocolStatus::UnsupportedVersion:
            return "UnsupportedVersion";
        case ipc::ProtocolStatus::InvalidMapping:
            return "InvalidMapping";
        case ipc::ProtocolStatus::Disconnected:
            return "Disconnected";
        case ipc::ProtocolStatus::StaleBinding:
            return "StaleBinding";
        case ipc::ProtocolStatus::BackendFailure:
            return "BackendFailure";
    }
    return "Unknown";
}

std::string formatSnapshotLine(
    std::uint8_t logicalSlot,
    const ipc::VirtualXInputResponse& response) {
    std::string line = "slot=" + std::to_string(logicalSlot) +
                       " status=" + protocolStatusName(response.status);
    if (response.status == ipc::ProtocolStatus::Ok && response.hasState) {
        line += " buttons=" + std::to_string(response.state.buttons);
        line += " lx=" + std::to_string(response.state.thumbLX);
    }
    return line;
}

std::string formatVibrationLine(const ipc::VirtualXInputResponse& response) {
    return "vibration status=" + protocolStatusName(response.status);
}

int runProbe(const ProbeOptions& options,
             std::ostream& output,
             std::uint32_t timeoutMs) noexcept {
    try {
        if (options.mode == ProbeMode::Snapshot) {
            for (std::uint8_t slot = 0; slot < kXInputSlotCount; ++slot) {
                ipc::VirtualXInputRequest request;
                request.opcode = ipc::ProtocolOpcode::GetState;
                request.seatId = options.seatId;
                request.activationGeneration = options.activationGeneration;
                request.sourceGeneration = options.sourceGeneration;
                request.logicalSlot = slot;

                const auto response = sendVirtualXInputRequest(
                    options.pipeEndpoint, request, timeoutMs);
                if (!response) return 3;
                if (slot == kSeatLogicalXInputSlot) {
                    if (response->status != ipc::ProtocolStatus::Ok ||
                        !response->hasState) {
                        return 4;
                    }
                } else if (response->status != ipc::ProtocolStatus::Disconnected ||
                           response->hasState) {
                    return 4;
                }
                output << formatSnapshotLine(slot, *response) << '\n';
                if (!output.good()) return 5;
            }
            return 0;
        }

        ipc::VirtualXInputRequest request;
        request.opcode = ipc::ProtocolOpcode::SetVibration;
        request.seatId = options.seatId;
        request.activationGeneration = options.activationGeneration;
        request.sourceGeneration = options.sourceGeneration;
        request.logicalSlot = kSeatLogicalXInputSlot;
        request.lowFrequencyMotor = options.lowFrequencyMotor;
        request.highFrequencyMotor = options.highFrequencyMotor;

        const auto response = sendVirtualXInputRequest(
            options.pipeEndpoint, request, timeoutMs);
        if (!response) return 3;
        if (response->status != ipc::ProtocolStatus::Ok || response->hasState) return 4;
        output << formatVibrationLine(*response) << '\n';
        return output.good() ? 0 : 5;
    } catch (...) {
        return 5;
    }
}

} // namespace hydra::controller::probe
