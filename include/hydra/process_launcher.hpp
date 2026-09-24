#pragma once

#include "hydra/process_group.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hydra::process {

enum class ProcessArchitecture : std::uint8_t {
    Any = 0,
    X86 = 1,
    X64 = 2,
};

enum class ProcessContainmentPolicy : std::uint8_t {
    RequireJobObject = 0,
    AllowBreakawayChildren = 1,
    AllowRootOnlyFallback = 2,
    RootOnly = 3,
};

struct ProcessLaunchSpec {
    SeatId seatId{0};
    std::wstring executablePath;
    std::vector<std::wstring> arguments;
    std::wstring workingDirectory;
    std::vector<std::pair<std::wstring, std::wstring>> environmentOverrides;
    ProcessArchitecture architecture{ProcessArchitecture::Any};
    ProcessContainmentPolicy containment{ProcessContainmentPolicy::RequireJobObject};
    bool createNewConsole{false};
};

struct ProcessLaunchResult {
    std::unique_ptr<SeatProcessGroup> group;
    ProcessIdentity root;
};

class ProcessLauncher {
public:
    static ProcessLaunchResult launch(const ProcessLaunchSpec& spec,
                                      std::string* error = nullptr);
};

} // namespace hydra::process
