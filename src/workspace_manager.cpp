#include "hydra/workspace_manager.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hydra {

static std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
#ifdef _WIN32
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), NULL, 0, NULL, NULL);
    std::string strTo(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &strTo[0], sizeNeeded, NULL, NULL);
    return strTo;
#else
    return std::string(wstr.begin(), wstr.end());
#endif
}

uint32_t WorkspaceManager::createWorkspace(const std::wstring& name) {
    uint32_t id = m_nextId++;
    WorkspaceConfig ws{};
    ws.workspaceId = id;
    ws.name = name.empty() ? (L"Player Workspace #" + std::to_wstring(id)) : name;
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

bool WorkspaceManager::assignTargetWindow(uint32_t workspaceId, uint64_t hwnd) {
    auto it = m_workspaces.find(workspaceId);
    if (it == m_workspaces.end()) return false;
    it->second.targetHwnd = hwnd;
    return true;
}

const WorkspaceConfig* WorkspaceManager::getWorkspace(uint32_t workspaceId) const {
    auto it = m_workspaces.find(workspaceId);
    if (it == m_workspaces.end()) return nullptr;
    return &it->second;
}

uint32_t WorkspaceManager::findWorkspaceByKeyboardPath(const std::wstring& keyboardPath) const {
    if (keyboardPath.empty()) return 0;
    for (const auto& kv : m_workspaces) {
        if (!kv.second.keyboardDevicePath.empty() && 
            (kv.second.keyboardDevicePath.find(keyboardPath) != std::wstring::npos ||
             keyboardPath.find(kv.second.keyboardDevicePath) != std::wstring::npos)) {
            return kv.first;
        }
    }
    return 0;
}

uint32_t WorkspaceManager::findWorkspaceByMousePath(const std::wstring& mousePath) const {
    if (mousePath.empty()) return 0;
    for (const auto& kv : m_workspaces) {
        if (!kv.second.mouseDevicePath.empty() && 
            (kv.second.mouseDevicePath.find(mousePath) != std::wstring::npos ||
             mousePath.find(kv.second.mouseDevicePath) != std::wstring::npos)) {
            return kv.first;
        }
    }
    return 0;
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

    ofs << "{\n  \"version\": \"1.0\",\n  \"workspaces\": [\n";
    bool first = true;
    for (const auto& kv : m_workspaces) {
        if (!first) ofs << ",\n";
        first = false;
        const auto& w = kv.second;

        std::string kbdUtf8 = wideToUtf8(w.keyboardDevicePath);
        std::string mouseUtf8 = wideToUtf8(w.mouseDevicePath);
        std::string dispUtf8 = wideToUtf8(w.displayDeviceName);

        auto escapeJson = [](std::string s) {
            std::string res;
            for (char c : s) {
                if (c == '\\') res += "\\\\";
                else if (c == '"') res += "\\\"";
                else res += c;
            }
            return res;
        };

        ofs << "    {\n"
            << "      \"id\": " << w.workspaceId << ",\n"
            << "      \"display\": \"" << escapeJson(dispUtf8) << "\",\n"
            << "      \"keyboard\": \"" << escapeJson(kbdUtf8) << "\",\n"
            << "      \"mouse\": \"" << escapeJson(mouseUtf8) << "\",\n"
            << "      \"controller\": " << w.controllerIndex << "\n"
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
