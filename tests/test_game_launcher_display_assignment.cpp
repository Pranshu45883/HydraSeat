#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/game_launcher.hpp"
#include "hydra/runtime_host.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc != 2 || argv[1] == nullptr) {
        std::cerr << "[FAIL] expected window fixture path\n";
        return 2;
    }

    hydra::runtime::RuntimeHost host;
    const auto devices = host.hardwareInventory();
    const auto display = std::find_if(
        devices.begin(),
        devices.end(),
        [](const hydra::DeviceInfo& device) {
            return device.type == hydra::DeviceType::Display;
        });
    if (display == devices.end()) {
        std::cout << "[SKIP] no attached desktop display\n";
        return 0;
    }

    const auto uiLease = host.acquireUiLease(1);
    assert(uiLease.valid());

    hydra::runtime::SeatHardwareConfiguration configuration;
    configuration.seatId = 1;
    configuration.displayId = display->id;

    std::string configurationError;
    assert(host.configureSeatHardware(
        uiLease,
        configuration,
        &configurationError));

    const std::filesystem::path fixture =
        std::filesystem::absolute(argv[1]);

    hydra::GameProfile game;
    game.title = L"HydraSeat display assignment fixture";
    game.platform = hydra::GamePlatform::CustomExecutable;
    game.executablePath = fixture.wstring();
    game.launchArguments = L"--mode unowned";
    game.workingDirectory = fixture.parent_path().wstring();

    hydra::WorkspaceConfig workspace;
    workspace.workspaceId = 1;
    workspace.name = L"Seat 1";

    hydra::GameLauncher launcher(host);
    assert(launcher.launchGameForWorkspace(game, workspace));

    auto snapshot = host.snapshot();
    const auto& seat = snapshot.seats[0];
    assert(seat.gameLeaseActive);
    assert(seat.processOwned);
    assert(seat.windowOwned);
    assert(seat.targetHwnd != 0);

    const auto hwnd = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(seat.targetHwnd));
    assert(IsWindow(hwnd) != FALSE);

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    assert(!display->devicePath.empty());
    assert(EnumDisplaySettingsExW(
        display->devicePath.c_str(),
        ENUM_CURRENT_SETTINGS,
        &mode,
        0) != FALSE);

    RECT rect{};
    assert(GetWindowRect(hwnd, &rect) != FALSE);
    const LONG expectedLeft = mode.dmPosition.x;
    const LONG expectedTop = mode.dmPosition.y;
    assert(std::abs(rect.left - expectedLeft) <= 4);
    assert(std::abs(rect.top - expectedTop) <= 4);

    assert(launcher.stopWorkspaceGame(1));

    snapshot = host.snapshot();
    assert(snapshot.seats[0].uiLeaseActive);
    assert(!snapshot.seats[0].gameLeaseActive);
    assert(!snapshot.seats[0].processOwned);
    assert(!snapshot.seats[0].windowOwned);
    assert(snapshot.seats[0].targetHwnd == 0);

    assert(host.releaseUiLease(uiLease));
    return 0;
#else
    (void)argc;
    (void)argv;
    return 0;
#endif
}
