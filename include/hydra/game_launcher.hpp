#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "hydra/controller_inventory.hpp"
#include "hydra/runtime_authority.hpp"
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
    explicit GameLauncher(runtime::SessionController& controller) noexcept
        : controller_(&controller) {}
    ~GameLauncher();

    GameLauncher(const GameLauncher&) = delete;
    GameLauncher& operator=(const GameLauncher&) = delete;
    GameLauncher(GameLauncher&&) = delete;
    GameLauncher& operator=(GameLauncher&&) = delete;

    // Launch a target for one v1 Seat. A runtime authority must be supplied at
    // construction; the default-constructed compatibility shell fails closed.
    bool launchGameForWorkspace(const GameProfile& game, const WorkspaceConfig& workspace);

    // Launch with an explicit Seat-owned XInput source. The controller binding is
    // accepted by the runtime authority before the child resumes, and the child
    // receives only the Seat-private adapter session context.
    bool launchGameForWorkspace(
        const GameProfile& game,
        const WorkspaceConfig& workspace,
        const controller::SeatBinding& controllerBinding,
        const controller::InventorySnapshot& inventory,
        std::wstring xinputPipeEndpoint);

    // Terminate only the process tree owned by the requested Seat and end the
    // matching activation generation after the Job Object is verified empty.
    bool stopWorkspaceGame(uint32_t workspaceId);

private:
    struct SeatProcess {
        std::uintptr_t processHandle{0};
        std::uintptr_t jobHandle{0};
        runtime::ActivationToken token{};
        runtime::ProcessIdentity identity{};
    };

    static std::optional<std::size_t> seatIndex(std::uint32_t workspaceId) noexcept;
    bool launchGameForWorkspaceImpl(
        const GameProfile& game,
        const WorkspaceConfig& workspace,
        const controller::SeatBinding* controllerBinding,
        const controller::InventorySnapshot* inventory,
        const std::wstring* xinputPipeEndpoint);

    runtime::SessionController* controller_{nullptr};
    std::array<std::optional<SeatProcess>, 2> seatProcesses_{};
};

} // namespace hydra
