#include "hydra/workspace_manager.hpp"

#include <fstream>
#include <iostream>

namespace hydra {

uint32_t WorkspaceManager::createWorkspace(const std::wstring& name) {
    uint32_t id = m_nextId++;
    WorkspaceConfig ws{};
    ws.workspaceId = id;
    ws.name = name.empty() ? (L"Workspace " + std::to_wstring(id)) : name;
    ws.active = true;
    m_workspaces[id] = ws;
    return id;
}

bool WorkspaceManager::removeWorkspace(uint32_t workspaceId) {
    return m_workspaces.erase(workspaceId) > 0;
}

bool WorkspaceManager::assignDisplay(uint32_t workspaceId, const std::wstring& displayDeviceName) {
    auto it = m_workspaces.find(workspaceId);
    if (it == m_workspaces.end()) return false;
    it->second.displayDeviceName = displayDeviceName;
    return true;
}

bool WorkspaceManager::assignKeyboard(uint32_t workspaceId, const std::wstring& keyboardDevicePath) {
    auto it = m_workspaces.find(workspaceId);
    if (it == m_workspaces.end()) return false;
    it->second.keyboardDevicePath = keyboardDevicePath;
    return true;
}

bool WorkspaceManager::assignMouse(uint32_t workspaceId, const std::wstring& mouseDevicePath) {
    auto it = m_workspaces.find(workspaceId);
    if (it == m_workspaces.end()) return false;
    it->second.mouseDevicePath = mouseDevicePath;
    return true;
}

bool WorkspaceManager::assignController(uint32_t workspaceId, uint32_t controllerIndex) {
    auto it = m_workspaces.find(workspaceId);
    if (it == m_workspaces.end()) return false;
    it->second.controllerIndex = controllerIndex;
    return true;
}

const WorkspaceConfig* WorkspaceManager::getWorkspace(uint32_t workspaceId) const {
    auto it = m_workspaces.find(workspaceId);
    if (it == m_workspaces.end()) return nullptr;
    return &it->second;
}

std::vector<WorkspaceConfig> WorkspaceManager::getAllWorkspaces() const {
    std::vector<WorkspaceConfig> result;
    result.reserve(m_workspaces.size());
    for (const auto& kv : m_workspaces) {
        result.push_back(kv.second);
    }
    return result;
}

bool WorkspaceManager::saveToFile(const std::string& filePath) const {
    std::ofstream ofs(filePath);
    if (!ofs.is_open()) return false;

    ofs << "{\n  \"workspaces\": [\n";
    bool first = true;
    for (const auto& kv : m_workspaces) {
        if (!first) ofs << ",\n";
        first = false;
        const auto& w = kv.second;
        ofs << "    {\n"
            << "      \"id\": " << w.workspaceId << ",\n"
            << "      \"controller_index\": " << w.controllerIndex << "\n"
            << "    }";
    }
    ofs << "\n  ]\n}\n";
    return true;
}

bool WorkspaceManager::loadFromFile(const std::string& filePath) {
    std::ifstream ifs(filePath);
    return ifs.is_open();
}

} // namespace hydra
