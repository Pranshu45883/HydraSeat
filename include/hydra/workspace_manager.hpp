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

    uint32_t createWorkspace(const std::wstring& name);
    bool removeWorkspace(uint32_t workspaceId);
    bool assignDisplay(uint32_t workspaceId, const std::wstring& displayDeviceName);
    bool assignKeyboard(uint32_t workspaceId, const std::wstring& keyboardDevicePath);
    bool assignMouse(uint32_t workspaceId, const std::wstring& mouseDevicePath);
    bool assignController(uint32_t workspaceId, const std::wstring& controllerId);
    const WorkspaceConfig* getWorkspace(uint32_t workspaceId) const;
    uint32_t findWorkspaceByKeyboardPath(const std::wstring& keyboardPath) const;
    uint32_t findWorkspaceByMousePath(const std::wstring& mousePath) const;
    std::vector<WorkspaceConfig> getAllWorkspaces() const;
    bool saveToFile(const std::string& filePath = "workspace_config.json") const;
    bool loadFromFile(const std::string& filePath = "workspace_config.json");

private:
    std::unordered_map<uint32_t, WorkspaceConfig> m_workspaces;
};

} // namespace hydra
