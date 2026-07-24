#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "hydra/workspace_manager.hpp"

namespace hydra {

enum class GamePlatform {
    Steam,
    Epic,
    EA,
    GOG,
    CustomExecutable
};

struct GameProfile {
    std::wstring title;
    GamePlatform platform{GamePlatform::CustomExecutable};
    std::wstring executablePath;
    std::wstring launchArguments;
    std::wstring workingDirectory;
    uint32_t appId{0}; // Steam AppID or Epic Launch ID
};

class GameLauncher {
public:
    GameLauncher() = default;
    ~GameLauncher() = default;

    // Launch game target instance configured for a specific workspace
    bool launchGameForWorkspace(const GameProfile& game, const WorkspaceConfig& workspace);

    // Terminate all process instances associated with a workspace
    bool stopWorkspaceGame(uint32_t workspaceId);
};

} // namespace hydra
