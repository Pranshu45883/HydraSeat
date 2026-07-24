#include "hydra/game_launcher.hpp"

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hydra {

bool GameLauncher::launchGameForWorkspace(const GameProfile& game, const WorkspaceConfig& workspace) {
#ifdef _WIN32
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::wstring cmdLine = game.executablePath + L" " + game.launchArguments;
    std::wstring workDir = game.workingDirectory;

    LPCWSTR pWorkDir = workDir.empty() ? NULL : workDir.c_str();

    BOOL success = CreateProcessW(
        NULL,
        cmdLine.data(),
        NULL,
        NULL,
        FALSE,
        CREATE_NEW_CONSOLE,
        NULL,
        pWorkDir,
        &si,
        &pi
    );

    if (success) {
        std::wcout << L"[GameLauncher] Started " << game.title 
                   << L" for Workspace #" << workspace.workspaceId 
                   << L" (PID: " << pi.dwProcessId << L")\n";
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
#else
    (void)game;
    (void)workspace;
#endif

    return false;
}

bool GameLauncher::stopWorkspaceGame(uint32_t workspaceId) {
    (void)workspaceId;
    return true;
}

} // namespace hydra
