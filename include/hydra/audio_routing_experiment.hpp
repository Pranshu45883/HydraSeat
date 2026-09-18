#pragma once

#include "hydra/runtime_authority.hpp"

#include <string>
#include <windows.h>

namespace hydra::windows {

class AudioRoutingExperiment {
public:
    // Manual test API for physical audio experiment
    static HRESULT manualRoute(DWORD pid, const std::wstring& targetEndpointId);
};

} // namespace hydra::windows
