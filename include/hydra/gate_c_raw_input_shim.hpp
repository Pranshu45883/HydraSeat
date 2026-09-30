#pragma once

#include "hydra/gate_c_adapter.h"

#include <cstdint>

namespace hydra::gatec {

// Maps adapter-side failures to the deterministic Win32 error contract used
// by the controlled Raw Input hooks. The function is portable for component
// tests; returned values are Win32 error numbers.
std::uint32_t rawInputSystemError(
    HydraGateCAdapterResult result) noexcept;

// Distinguishes expected caller/token contract failures from loss or
// corruption of the adapter backend. Only the latter tears down ACTIVE mode.
bool rawInputResultIsBackendFatal(
    HydraGateCAdapterResult result) noexcept;

} // namespace hydra::gatec
