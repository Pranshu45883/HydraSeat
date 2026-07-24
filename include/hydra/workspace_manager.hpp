#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace hydra {

struct WorkspaceConfig {
    uint32_t workspaceId{1};
    std::wstring name;
    std::wstring displayDeviceName;
    std::wstring keyboardDevicePath;
    std::wstring mouseDevicePath;
    uint32_t controllerIndex{0};
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

    // Assign gamepad controller to workspace
    bool assignController(uint32_t workspaceId, uint32_t controllerIndex);

    // Get configuration for a specific workspace
    const WorkspaceConfig* getWorkspace(uint32_t workspaceId) const;

    // Get all active workspaces
    std::vector<WorkspaceConfig> getAllWorkspaces() const;

    // Save profile configurations to file
    bool saveToFile(const std::string& filePath) const;

    // Load profile configurations from file
    bool loadFromFile(const std::string& filePath);

private:
    uint32_t m_nextId{1};
    std::unordered_map<uint32_t, WorkspaceConfig> m_workspaces;
};

} // namespace hydra
