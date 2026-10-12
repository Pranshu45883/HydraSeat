#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hydra/controller_inventory.hpp"
#include "hydra/runtime_authority.hpp"
#include "hydra/workspace_manager.hpp"

namespace hydra::runtime {
class RuntimeHost;
}

namespace hydra::gatec {
class ExternalInputSession;
}

namespace hydra {

struct RawInputEvent;

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
    uint32_t appId{0};
};

class GameLauncher {
public:
    GameLauncher() = default;
    explicit GameLauncher(runtime::SessionController& controller) noexcept
        : controller_(&controller) {}
    explicit GameLauncher(runtime::RuntimeHost& host) noexcept
        : host_(&host) {}
    ~GameLauncher();

    GameLauncher(const GameLauncher&) = delete;
    GameLauncher& operator=(const GameLauncher&) = delete;
    GameLauncher(GameLauncher&&) = delete;
    GameLauncher& operator=(GameLauncher&&) = delete;

    // Launch a target for one v1 Seat. In production hydra_host owns this
    // object and all authority calls pass through RuntimeHost. The
    // SessionController constructor remains only as a compatibility seam for
    // focused lower-level tests.
    bool launchGameForWorkspace(
        const GameProfile& game,
        const WorkspaceConfig& workspace);

    // Explicit controller launch seam retained for controlled tests. Production
    // host launch automatically reuses an already host-owned Seat binding.
    bool launchGameForWorkspace(
        const GameProfile& game,
        const WorkspaceConfig& workspace,
        const controller::SeatBinding& controllerBinding,
        const controller::InventorySnapshot& inventory,
        std::wstring xinputPipeEndpoint);

    bool stopWorkspaceGame(uint32_t workspaceId);
    // Reconcile naturally exited process trees with canonical Seat authority.
    // This never terminates a live process; it only releases runtime state once
    // the strict Seat Job Object reports zero active processes.
    void reapExitedGames() noexcept;
    bool hasWorkspaceGame(uint32_t workspaceId) const;
    bool routePhysicalInput(const RawInputEvent& event);
    std::string lastError() const;

private:
    struct ControllerPipeRuntime;
    struct SeatWindowRuntime;

    struct SeatProcess {
        std::uintptr_t processHandle{0};
        std::uintptr_t jobHandle{0};
        runtime::ActivationToken token{};
        runtime::ProcessIdentity identity{};
        std::shared_ptr<ControllerPipeRuntime> controllerPipe;
        std::shared_ptr<SeatWindowRuntime> windowRuntime;
        std::vector<std::uintptr_t> keyboardHandles;
        std::vector<std::uintptr_t> mouseHandles;
        std::shared_ptr<gatec::ExternalInputSession> inputSession;
    };

    static std::optional<std::size_t> seatIndex(
        std::uint32_t workspaceId) noexcept;

    bool launchGameForWorkspaceImpl(
        const GameProfile& game,
        const WorkspaceConfig& workspace,
        const controller::SeatBinding* controllerBinding,
        const controller::InventorySnapshot* inventory,
        const std::wstring* xinputPipeEndpoint);

    runtime::ActivationToken beginSeatActivation(
        std::uint32_t seatId) noexcept;
    bool publishProcess(
        const runtime::ActivationToken& token,
        const runtime::ProcessIdentity& process) noexcept;
    bool bindTargetWindow(
        const runtime::ActivationToken& token,
        const runtime::ProcessIdentity& process,
        std::uintptr_t hwnd) noexcept;
    bool clearTargetWindow(
        const runtime::ActivationToken& token,
        const runtime::ProcessIdentity& process,
        std::uintptr_t expectedHwnd) noexcept;
    bool bindController(
        const runtime::ActivationToken& token,
        const controller::SeatBinding& binding,
        const controller::InventorySnapshot& inventory) noexcept;
    std::optional<controller::VirtualXInputMapping> virtualXInputMapping(
        const runtime::ActivationToken& token) const noexcept;
    bool endSeatActivation(
        const runtime::ActivationToken& token) noexcept;
    void setLastError(std::string message);

    runtime::SessionController* controller_{nullptr};
    runtime::RuntimeHost* host_{nullptr};
    mutable std::recursive_mutex processMutex_;
    std::array<std::optional<SeatProcess>, 2> seatProcesses_{};
    std::string lastError_;
};

} // namespace hydra
