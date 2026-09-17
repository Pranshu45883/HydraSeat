#pragma once

#include "hydra/runtime_authority.hpp"

#include <string>

namespace hydra::windows {

// Defines the conclusive result of an audio routing feasibility experiment.
enum class RoutingMechanismStatus {
    SupportedAndVerified,
    SupportedButUnverified,
    Unsupported,
    Failed,
    NotApplicable
};

// Captures evidence of what the experiment changed, verified, and restored.
struct RoutingExperimentEvidence {
    std::string mechanism;
    RoutingMechanismStatus status;
    std::uint32_t targetProcessId;
    std::uint64_t targetCreationIdentity;
    std::wstring targetEndpointId;
    std::wstring defaultEndpointBefore;
    std::wstring defaultEndpointAfter;
    bool globalDefaultChanged;
    bool rollbackVerified;
    std::string windowsVersion;
    std::string notes;
};

class AudioRoutingExperiment {
public:
    // Attempts to route the target process to the specified render endpoint
    // using the undocumented IAudioPolicyConfigFactory mechanism.
    // Proves isolation by strictly validating global default stability.
    static RoutingExperimentEvidence runPolicyConfigExperiment(
        const hydra::runtime::ProcessIdentity& targetProcess,
        const std::wstring& targetEndpointId);
};

} // namespace hydra::windows
