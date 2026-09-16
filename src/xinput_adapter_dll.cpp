#include "hydra/xinput_adapter_session.hpp"
#include "hydra/virtual_xinput_pipe.hpp"

#if defined(_WIN32)
#include <windows.h>

// Consume only XInput ABI types/constants. Rename the Windows SDK function
// declarations so this DLL can provide the real exported names itself without
// importing the system XInput implementation.
#define XInputGetState HydraSystemXInputGetStateDeclaration
#define XInputSetState HydraSystemXInputSetStateDeclaration
#define XInputGetCapabilities HydraSystemXInputGetCapabilitiesDeclaration
#include <Xinput.h>
#undef XInputGetCapabilities
#undef XInputSetState
#undef XInputGetState

#include <cstring>
#include <optional>

namespace {

using hydra::controller::adapter::SessionConfig;
using hydra::controller::ipc::ProtocolOpcode;
using hydra::controller::ipc::ProtocolStatus;
using hydra::controller::ipc::VirtualXInputRequest;
using hydra::controller::ipc::VirtualXInputResponse;

constexpr std::uint32_t kAdapterTimeoutMs = 2000;

const std::optional<SessionConfig>& adapterSession() noexcept {
    static const std::optional<SessionConfig> session =
        hydra::controller::adapter::loadSessionConfigFromEnvironment();
    return session;
}

std::optional<VirtualXInputResponse> sendStateRequest(
    const SessionConfig& session) noexcept {
    VirtualXInputRequest request;
    request.opcode = ProtocolOpcode::GetState;
    request.seatId = session.seatId;
    request.activationGeneration = session.activationGeneration;
    request.sourceGeneration = session.sourceGeneration;
    request.logicalSlot = 0;
    return hydra::controller::sendVirtualXInputRequest(
        session.pipeEndpoint, request, kAdapterTimeoutMs);
}

DWORD disconnected() noexcept {
    return ERROR_DEVICE_NOT_CONNECTED;
}

bool responseHasState(const std::optional<VirtualXInputResponse>& response) noexcept {
    return response.has_value() && response->status == ProtocolStatus::Ok &&
           response->hasState;
}

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI XInputGetState(
    DWORD dwUserIndex,
    XINPUT_STATE* pState) noexcept {
    if (pState == nullptr) return ERROR_BAD_ARGUMENTS;
    std::memset(pState, 0, sizeof(*pState));

    if (dwUserIndex != 0) return disconnected();
    const auto& session = adapterSession();
    if (!session || !session->valid()) return disconnected();

    const auto response = sendStateRequest(*session);
    if (!responseHasState(response)) return disconnected();

    pState->dwPacketNumber = response->state.packetNumber;
    pState->Gamepad.wButtons = response->state.buttons;
    pState->Gamepad.bLeftTrigger = response->state.leftTrigger;
    pState->Gamepad.bRightTrigger = response->state.rightTrigger;
    pState->Gamepad.sThumbLX = response->state.thumbLX;
    pState->Gamepad.sThumbLY = response->state.thumbLY;
    pState->Gamepad.sThumbRX = response->state.thumbRX;
    pState->Gamepad.sThumbRY = response->state.thumbRY;
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI XInputSetState(
    DWORD dwUserIndex,
    XINPUT_VIBRATION* pVibration) noexcept {
    if (pVibration == nullptr) return ERROR_BAD_ARGUMENTS;
    if (dwUserIndex != 0) return disconnected();

    const auto& session = adapterSession();
    if (!session || !session->valid()) return disconnected();

    VirtualXInputRequest request;
    request.opcode = ProtocolOpcode::SetVibration;
    request.seatId = session->seatId;
    request.activationGeneration = session->activationGeneration;
    request.sourceGeneration = session->sourceGeneration;
    request.logicalSlot = 0;
    request.lowFrequencyMotor = pVibration->wLeftMotorSpeed;
    request.highFrequencyMotor = pVibration->wRightMotorSpeed;

    const auto response = hydra::controller::sendVirtualXInputRequest(
        session->pipeEndpoint, request, kAdapterTimeoutMs);
    if (!response || response->status != ProtocolStatus::Ok) return disconnected();
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI XInputGetCapabilities(
    DWORD dwUserIndex,
    DWORD dwFlags,
    XINPUT_CAPABILITIES* pCapabilities) noexcept {
    if (pCapabilities == nullptr) return ERROR_BAD_ARGUMENTS;
    std::memset(pCapabilities, 0, sizeof(*pCapabilities));

    if (dwUserIndex != 0) return disconnected();
    if (dwFlags != 0 && dwFlags != XINPUT_FLAG_GAMEPAD) return ERROR_BAD_ARGUMENTS;

    const auto& session = adapterSession();
    if (!session || !session->valid()) return disconnected();

    const auto response = sendStateRequest(*session);
    if (!responseHasState(response)) return disconnected();

    pCapabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
    pCapabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    return ERROR_SUCCESS;
}
#endif
