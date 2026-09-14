#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>

namespace hydra {

struct WorkspaceConfig {
    uint32_t workspaceId{1};
    std::wstring name;
    std::wstring displayDeviceName;
    std::wstring keyboardDevicePath;
    std::wstring mouseDevicePath;
    std::optional<std::wstring> controllerId;
    bool active{true};
};

class WorkspaceManager {
public:
    WorkspaceManager() = default;
    ~WorkspaceManager() = default;

    // Create a new player workspace
    uint32_t createWorkspace(const std::wstring& name);

    // Remove a workspace by ID
    bool removeWorkspace(uint32_t workspaceId);

    // Assign display to workspace
    bool assignDisplay(uint32_t workspaceId, const std::wstring& displayDeviceName);

    // Assign physical keyboard to workspace
    bool assignKeyboard(uint32_t workspaceId, const std::wstring& keyboardDevicePath);

    // Assign physical mouse to workspace
    bool assignMouse(uint32_t workspaceId, const std::wstring& mouseDevicePath);

    // Assign stable physical gamepad identity to workspace
    bool assignController(uint32_t workspaceId, const std::wstring& controllerId);

    // Get configuration for a specific workspace
    const WorkspaceConfig* getWorkspace(uint32_t workspaceId) const;

    // Get workspace by keyboard path
    uint32_t findWorkspaceByKeyboardPath(const std::wstring& keyboardPath) const;

    // Get workspace by mouse path
    uint32_t findWorkspaceByMousePath(const std::wstring& mousePath) const;

    // Get all active workspaces
    std::vector<WorkspaceConfig> getAllWorkspaces() const;

    // Save profile configurations to file
    bool saveToFile(const std::string& filePath = "workspace_config.json") const;

    // Load profile configurations from file
    bool loadFromFile(const std::string& filePath = "workspace_config.json");

private:
    std::unordered_map<uint32_t, WorkspaceConfig> m_workspaces;
};

} // namespace hydra
