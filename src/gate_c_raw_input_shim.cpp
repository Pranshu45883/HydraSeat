#include "hydra/gate_c_raw_input_shim.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace hydra::gatec {

std::uint32_t rawInputSystemError(
    HydraGateCAdapterResult result) noexcept {
#ifdef _WIN32
    constexpr std::uint32_t invalidParameter = ERROR_INVALID_PARAMETER;
    constexpr std::uint32_t insufficientBuffer = ERROR_INSUFFICIENT_BUFFER;
    constexpr std::uint32_t invalidHandle = ERROR_INVALID_HANDLE;
    constexpr std::uint32_t disconnected = ERROR_DEVICE_NOT_CONNECTED;
#else
    constexpr std::uint32_t invalidParameter = 87;
    constexpr std::uint32_t insufficientBuffer = 122;
    constexpr std::uint32_t invalidHandle = 6;
    constexpr std::uint32_t disconnected = 1167;
#endif
    switch (result) {
    case HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT:
    case HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH:
    case HYDRA_GATE_C_ADAPTER_INVALID_STATE:
        return invalidParameter;
    case HYDRA_GATE_C_ADAPTER_BUFFER_TOO_SMALL:
        return insufficientBuffer;
    case HYDRA_GATE_C_ADAPTER_RAW_INPUT_FAILURE:
        return invalidHandle;
    default:
        return disconnected;
    }
}

bool rawInputResultIsBackendFatal(
    HydraGateCAdapterResult result) noexcept {
    switch (result) {
    case HYDRA_GATE_C_ADAPTER_OK:
    case HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT:
    case HYDRA_GATE_C_ADAPTER_BUFFER_TOO_SMALL:
    case HYDRA_GATE_C_ADAPTER_RAW_INPUT_FAILURE:
        return false;
    default:
        return true;
    }
}

} // namespace hydra::gatec
