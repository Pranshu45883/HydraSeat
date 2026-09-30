#include "hydra/steam_provider.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace hydra::provider::steam {
namespace {

namespace fs = std::filesystem;

struct KeyValueEntry {
    std::string key;
    std::optional<std::string> value;
    std::vector<KeyValueEntry> children;
};

class KeyValuesParser {
public:
    explicit KeyValuesParser(std::string_view input) : input_(input) {}

    bool parse(std::vector<KeyValueEntry>& output, std::string& error) {
        try {
            std::vector<KeyValueEntry> candidate;
            if (!parseObject(candidate, 0u, false)) return fail(error);
            skipSpaceAndComments();
            if (cursor_ != input_.size()) {
                message_ = "trailing Steam KeyValues data";
                return fail(error);
            }
            output = std::move(candidate);
            return true;
        } catch (...) {
            error = "Steam KeyValues parser allocation failure";
            return false;
        }
    }

private:
    bool parseObject(std::vector<KeyValueEntry>& output,
                     std::size_t depth,
                     bool expectClose) {
        if (depth > kMaximumSteamKeyValuesDepth) {
            message_ = "Steam KeyValues nesting exceeds the bounded depth";
            return false;
        }
        std::set<std::string> keys;
        while (true) {
            skipSpaceAndComments();
            if (cursor_ >= input_.size()) {
                if (expectClose) message_ = "unterminated Steam KeyValues object";
                return !expectClose;
            }
            if (input_[cursor_] == '}') {
                if (!expectClose) {
                    message_ = "unexpected Steam KeyValues closing brace";
                    return false;
                }
                ++cursor_;
                return true;
            }

            std::string key;
            if (!parseString(key)) return false;
            if (!keys.insert(asciiLower(key)).second) {
                message_ = "duplicate Steam KeyValues key";
                return false;
            }
            if (++nodes_ > kMaximumSteamKeyValuesNodes) {
                message_ = "Steam KeyValues node count exceeds the bound";
                return false;
            }

            skipSpaceAndComments();
            KeyValueEntry entry;
            entry.key = std::move(key);
            if (cursor_ < input_.size() && input_[cursor_] == '{') {
                ++cursor_;
                if (!parseObject(entry.children, depth + 1u, true)) return false;
            } else {
                std::string value;
                if (!parseString(value)) return false;
                entry.value = std::move(value);
            }
            output.push_back(std::move(entry));
        }
    }

    bool parseString(std::string& output) {
        skipSpaceAndComments();
        if (cursor_ >= input_.size() || input_[cursor_] != '"') {
            message_ = "Steam KeyValues expected a quoted string";
            return false;
        }
        ++cursor_;
        std::string value;
        while (cursor_ < input_.size()) {
            const char ch = input_[cursor_++];
            if (ch == '"') {
                if (value.size() > profile::kMaximumPathCodeUnits * 4u) {
                    message_ = "Steam KeyValues string exceeds the bound";
                    return false;
                }
                output = std::move(value);
                return true;
            }
            if (ch == '\0' || ch == '\r' || ch == '\n') {
                message_ = "Steam KeyValues contains an invalid control character";
                return false;
            }
            if (ch == '\\') {
                if (cursor_ >= input_.size()) {
                    message_ = "Steam KeyValues has a dangling escape";
                    return false;
                }
                const char escaped = input_[cursor_++];
                switch (escaped) {
                case '\\': value.push_back('\\'); break;
                case '"': value.push_back('"'); break;
                case 't': value.push_back('\t'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                default:
                    value.push_back('\\');
                    value.push_back(escaped);
                    break;
                }
            } else {
                value.push_back(ch);
            }
        }
        message_ = "unterminated Steam KeyValues string";
        return false;
    }

    void skipSpaceAndComments() {
        while (cursor_ < input_.size()) {
            const auto ch = static_cast<unsigned char>(input_[cursor_]);
            if (std::isspace(ch) != 0) {
                ++cursor_;
                continue;
            }
            if (cursor_ + 1u < input_.size() && input_[cursor_] == '/' &&
                input_[cursor_ + 1u] == '/') {
                cursor_ += 2u;
                while (cursor_ < input_.size() && input_[cursor_] != '\n') ++cursor_;
                continue;
            }
            break;
        }
    }

    bool fail(std::string& error) const {
        error = message_.empty() ? "invalid Steam KeyValues document" : message_;
        return false;
    }

    static std::string asciiLower(std::string value) {
        for (char& raw : value) {
            const auto ch = static_cast<unsigned char>(raw);
            if (ch >= static_cast<unsigned char>('A') &&
                ch <= static_cast<unsigned char>('Z')) {
                raw = static_cast<char>(ch - static_cast<unsigned char>('A') +
                                        static_cast<unsigned char>('a'));
            }
        }
        return value;
    }

    std::string_view input_;
    std::size_t cursor_{0};
    std::size_t nodes_{0};
    std::string message_;
};

std::string asciiLower(std::string value) {
    for (char& raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch >= static_cast<unsigned char>('A') &&
            ch <= static_cast<unsigned char>('Z')) {
            raw = static_cast<char>(ch - static_cast<unsigned char>('A') +
                                    static_cast<unsigned char>('a'));
        }
    }
    return value;
}

const KeyValueEntry* findEntry(std::span<const KeyValueEntry> entries,
                               std::string_view key) {
    const auto expected = asciiLower(std::string(key));
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return asciiLower(entry.key) == expected;
    });
    return found == entries.end() ? nullptr : &*found;
}

bool parseDocument(std::string_view bytes,
                   std::vector<KeyValueEntry>& output,
                   std::string& error) {
    if (bytes.empty() || bytes.size() > kMaximumSteamMetadataBytes) {
        error = "Steam metadata document is empty or exceeds the byte bound";
        return false;
    }
    return KeyValuesParser(bytes).parse(output, error);
}

bool utf8ToWide(std::string_view input, std::wstring& output) {
    output.clear();
    try {
        for (std::size_t index = 0u; index < input.size();) {
            const auto first = static_cast<unsigned char>(input[index]);
            std::uint32_t codePoint = 0u;
            std::size_t continuation = 0u;
            if (first <= 0x7fu) {
                codePoint = first;
            } else if ((first & 0xe0u) == 0xc0u) {
                codePoint = first & 0x1fu;
                continuation = 1u;
            } else if ((first & 0xf0u) == 0xe0u) {
                codePoint = first & 0x0fu;
                continuation = 2u;
            } else if ((first & 0xf8u) == 0xf0u) {
                codePoint = first & 0x07u;
                continuation = 3u;
            } else {
                return false;
            }
            if (index + continuation >= input.size()) return false;
            for (std::size_t step = 0u; step < continuation; ++step) {
                const auto next = static_cast<unsigned char>(input[index + step + 1u]);
                if ((next & 0xc0u) != 0x80u) return false;
                codePoint = (codePoint << 6u) | (next & 0x3fu);
            }
            if ((continuation == 1u && codePoint < 0x80u) ||
                (continuation == 2u && codePoint < 0x800u) ||
                (continuation == 3u && codePoint < 0x10000u) ||
                codePoint > 0x10ffffu ||
                (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
                return false;
            }
            if constexpr (sizeof(wchar_t) == 2u) {
                if (codePoint <= 0xffffu) {
                    output.push_back(static_cast<wchar_t>(codePoint));
                } else {
                    codePoint -= 0x10000u;
                    output.push_back(static_cast<wchar_t>(0xd800u + (codePoint >> 10u)));
                    output.push_back(static_cast<wchar_t>(0xdc00u + (codePoint & 0x3ffu)));
                }
            } else {
                output.push_back(static_cast<wchar_t>(codePoint));
            }
            index += continuation + 1u;
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

bool digitsOnly(std::string_view value) noexcept {
    if (value.empty() || value.size() > profile::kMaximumIdentifierBytes) return false;
    return std::all_of(value.begin(), value.end(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return ch >= static_cast<unsigned char>('0') &&
               ch <= static_cast<unsigned char>('9');
    });
}

bool validSteamAppId(std::string_view value) noexcept {
    if (!digitsOnly(value) || value.size() > 10u) return false;
    std::uint64_t number = 0u;
    for (const char raw : value) {
        number = number * 10u +
                 static_cast<unsigned char>(raw) - static_cast<unsigned char>('0');
    }
    return number > 0u && number <= (std::numeric_limits<std::uint32_t>::max)();
}

bool safeInstallDirectory(const fs::path& value) {
    if (value.empty() || value.is_absolute() || value.has_root_name() || value.has_root_directory()) {
        return false;
    }
    for (const auto& component : value) {
        if (component == L".." || component == L".") return false;
    }
    return value.native().size() <= profile::kMaximumPathCodeUnits;
}

std::wstring normalizedPath(const fs::path& value) {
    std::error_code error;
    const auto absolute = fs::absolute(value, error);
    return (error ? value : absolute).lexically_normal().wstring();
}

std::wstring lowerPath(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    for (wchar_t& ch : value) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return value;
}

std::string normalizedSteamAppType(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char raw : value) {
        auto ch = static_cast<unsigned char>(raw);
        if (ch >= static_cast<unsigned char>('A') &&
            ch <= static_cast<unsigned char>('Z')) {
            ch = static_cast<unsigned char>(ch - static_cast<unsigned char>('A') +
                                            static_cast<unsigned char>('a'));
        }
        if ((ch >= static_cast<unsigned char>('a') &&
             ch <= static_cast<unsigned char>('z')) ||
            (ch >= static_cast<unsigned char>('0') &&
             ch <= static_cast<unsigned char>('9'))) {
            normalized.push_back(static_cast<char>(ch));
            continue;
        }
        if (ch == static_cast<unsigned char>(' ') ||
            ch == static_cast<unsigned char>('\t') ||
            ch == static_cast<unsigned char>('-') ||
            ch == static_cast<unsigned char>('_')) {
            continue;
        }
        return {};
    }
    return normalized;
}

bool manifestIsClearlyNonPlayable(std::span<const KeyValueEntry> appState,
                                  std::wstring_view installDirectory,
                                  std::string_view appId) {
    // Valve's Steamworks Common Redistributables package has a stable provider
    // app id. Do not rely on its friendly title or install directory because
    // both have varied across Steam installations/locales.
    if (appId == "228980") return true;

    // Steam's shared redistributable package is identified by its provider-owned
    // install directory, not by the localized/friendly title shown to the user.
    if (lowerPath(std::wstring(installDirectory)) == L"_commonredist") return true;

    const auto* type = findEntry(appState, "type");
    if (!type || !type->value) return false;
    const auto normalized = normalizedSteamAppType(*type->value);
    if (normalized.empty()) return false;

    static constexpr std::array<std::string_view, 9u> nonPlayableTypes{
        "tool", "server", "dedicatedserver", "runtime", "redistributable",
        "redistributables", "support", "supportpackage", "config",
    };
    return std::find(nonPlayableTypes.begin(), nonPlayableTypes.end(), normalized) !=
           nonPlayableTypes.end();
}

std::uint64_t hashBytes(std::uint64_t current, std::string_view bytes) noexcept {
    for (const char raw : bytes) {
        current ^= static_cast<unsigned char>(raw);
        current *= 1099511628211ull;
    }
    return current;
}

std::uint64_t hashWide(std::uint64_t current, std::wstring_view value) noexcept {
    for (const wchar_t ch : value) {
        const auto unit = static_cast<std::uint32_t>(ch);
        for (unsigned shift = 0u; shift < 32u; shift += 8u) {
            current ^= static_cast<unsigned char>((unit >> shift) & 0xffu);
            current *= 1099511628211ull;
        }
    }
    return current;
}

std::string decimal(std::uint64_t value) {
    return std::to_string(value);
}

bool parseLibraryRoots(std::string_view bytes,
                       const std::wstring& steamRoot,
                       std::vector<std::wstring>& roots,
                       std::string& error) {
    std::vector<KeyValueEntry> document;
    if (!parseDocument(bytes, document, error)) return false;
    const auto* libraryFolders = findEntry(document, "libraryfolders");
    if (!libraryFolders || libraryFolders->value) {
        error = "Steam libraryfolders root object is missing";
        return false;
    }

    std::map<std::wstring, std::wstring> unique;
    const auto addRoot = [&](const std::wstring& raw) {
        const auto normalized = normalizedPath(fs::path(raw));
        unique.emplace(lowerPath(normalized), normalized);
    };
    addRoot(steamRoot);
    for (const auto& library : libraryFolders->children) {
        if (!digitsOnly(library.key) || library.value) continue;
        const auto* path = findEntry(library.children, "path");
        if (!path || !path->value) {
            error = "Steam library entry has no path";
            return false;
        }
        std::wstring wide;
        if (!utf8ToWide(*path->value, wide) || wide.empty() || !fs::path(wide).is_absolute()) {
            error = "Steam library path is invalid";
            return false;
        }
        addRoot(wide);
        if (unique.size() > kMaximumSteamLibraries) {
            error = "Steam library count exceeds the bound";
            return false;
        }
    }
    roots.clear();
    for (auto& [key, value] : unique) {
        (void)key;
        roots.push_back(std::move(value));
    }
    return true;
}

bool parseManifest(std::string_view bytes,
                   const std::wstring& manifestPath,
                   const std::wstring& libraryRoot,
                   std::string& appId,
                   std::wstring& title,
                   std::wstring& installRoot,
                   std::optional<std::wstring>& buildId,
                   bool& visibleInGameCatalog,
                   std::string& error) {
    std::vector<KeyValueEntry> document;
    if (!parseDocument(bytes, document, error)) return false;
    const auto* state = findEntry(document, "appstate");
    if (!state || state->value) {
        error = "Steam app manifest AppState object is missing";
        return false;
    }
    const auto* id = findEntry(state->children, "appid");
    const auto* name = findEntry(state->children, "name");
    const auto* directory = findEntry(state->children, "installdir");
    if (!id || !id->value || !validSteamAppId(*id->value) || !name || !name->value ||
        !directory || !directory->value) {
        error = "Steam app manifest is missing a bounded appid/name/installdir";
        return false;
    }
    const auto filename = lowerPath(fs::path(manifestPath).filename().wstring());
    std::wstring expected;
    if (!utf8ToWide("appmanifest_" + *id->value + ".acf", expected) || filename != expected) {
        error = "Steam app manifest filename does not match appid";
        return false;
    }
    if (!utf8ToWide(*name->value, title) || title.empty() ||
        title.size() > profile::kMaximumTitleCodeUnits) {
        error = "Steam app manifest title is invalid";
        return false;
    }
    std::wstring installDirectory;
    if (!utf8ToWide(*directory->value, installDirectory) ||
        !safeInstallDirectory(fs::path(installDirectory))) {
        error = "Steam app manifest install directory is invalid";
        return false;
    }
    installRoot = normalizedPath(fs::path(libraryRoot) / L"steamapps" / L"common" /
                                 fs::path(installDirectory));
    if (installRoot.size() > profile::kMaximumPathCodeUnits) {
        error = "Steam app install path exceeds the bound";
        return false;
    }
    visibleInGameCatalog =
        !manifestIsClearlyNonPlayable(state->children, installDirectory, *id->value);
    buildId.reset();
    if (const auto* build = findEntry(state->children, "buildid")) {
        if (!build->value || !digitsOnly(*build->value)) {
            error = "Steam app manifest buildid is invalid";
            return false;
        }
        std::wstring wideBuild;
        if (!utf8ToWide(*build->value, wideBuild)) {
            error = "Steam app manifest buildid encoding is invalid";
            return false;
        }
        buildId = std::move(wideBuild);
    }
    appId = *id->value;
    return true;
}

#ifdef _WIN32

class NativeSteamMetadataSource final : public SteamMetadataSource {
public:
    SteamSourceResult locateInstallation(std::wstring& steamRoot,
                                         std::string& error) noexcept override {
        try {
            const std::array<std::pair<HKEY, const wchar_t*>, 3> locations{{
                {HKEY_CURRENT_USER, L"Software\\Valve\\Steam"},
                {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam"},
                {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam"},
            }};
            for (const auto& [root, key] : locations) {
                for (const wchar_t* valueName : {L"SteamPath", L"InstallPath"}) {
                    DWORD bytes = 0u;
                    const LSTATUS sizeResult = RegGetValueW(
                        root, key, valueName, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
                    if (sizeResult != ERROR_SUCCESS || bytes < sizeof(wchar_t)) continue;
                    std::wstring value(bytes / sizeof(wchar_t), L'\0');
                    DWORD type = 0u;
                    if (RegGetValueW(root, key, valueName, RRF_RT_REG_SZ, &type,
                                     value.data(), &bytes) != ERROR_SUCCESS) {
                        continue;
                    }
                    while (!value.empty() && value.back() == L'\0') value.pop_back();
                    const auto candidate = normalizedPath(fs::path(value));
                    std::error_code fsError;
                    if (fs::is_regular_file(fs::path(candidate) / L"steam.exe", fsError) &&
                        fs::is_directory(fs::path(candidate) / L"steamapps", fsError)) {
                        steamRoot = candidate;
                        return SteamSourceResult::Success;
                    }
                }
            }
            error = "Steam installation was not found in supported registry locations";
            return SteamSourceResult::NotInstalled;
        } catch (...) {
            error = "Steam installation discovery failed";
            return SteamSourceResult::Failure;
        }
    }

    bool readTextFile(const std::wstring& path,
                      std::size_t maximumBytes,
                      std::string& bytes,
                      std::string& error) noexcept override {
        try {
            std::ifstream input(fs::path(path), std::ios::binary | std::ios::ate);
            if (!input) {
                error = "Steam metadata file could not be opened read-only";
                return false;
            }
            const auto end = input.tellg();
            if (end <= 0 || static_cast<std::uint64_t>(end) > maximumBytes) {
                error = "Steam metadata file is empty or exceeds the byte bound";
                return false;
            }
            std::string candidate(static_cast<std::size_t>(end), '\0');
            input.seekg(0, std::ios::beg);
            input.read(candidate.data(), static_cast<std::streamsize>(candidate.size()));
            if (!input || input.gcount() != static_cast<std::streamsize>(candidate.size())) {
                error = "Steam metadata file changed or could not be read completely";
                return false;
            }
            bytes = std::move(candidate);
            return true;
        } catch (...) {
            error = "Steam metadata file read failed";
            return false;
        }
    }

    bool listManifestFiles(std::span<const std::wstring> steamAppsRoots,
                           std::vector<std::wstring>& paths,
                           std::string& error) noexcept override {
        try {
            std::map<std::wstring, std::wstring> unique;
            for (const auto& root : steamAppsRoots) {
                std::error_code iteratorError;
                fs::directory_iterator iterator(fs::path(root), iteratorError);
                if (iteratorError) continue;
                for (const auto& entry : iterator) {
                    std::error_code statusError;
                    if (!entry.is_regular_file(statusError) || statusError) continue;
                    const auto name = lowerPath(entry.path().filename().wstring());
                    if (!name.starts_with(L"appmanifest_") || !name.ends_with(L".acf")) continue;
                    const auto normalized = normalizedPath(entry.path());
                    unique.emplace(lowerPath(normalized), normalized);
                    if (unique.size() > kMaximumSteamManifests) {
                        error = "Steam manifest count exceeds the bound";
                        return false;
                    }
                }
            }
            paths.clear();
            for (auto& [key, value] : unique) {
                (void)key;
                paths.push_back(std::move(value));
            }
            return true;
        } catch (...) {
            error = "Steam manifest enumeration failed";
            return false;
        }
    }

    bool listExecutableHints(const std::wstring& installRoot,
                             std::vector<std::wstring>& paths,
                             std::string& error) noexcept override {
        try {
            std::map<std::wstring, std::wstring> unique;
            std::error_code iteratorError;
            fs::recursive_directory_iterator iterator(
                fs::path(installRoot), fs::directory_options::skip_permission_denied,
                iteratorError);
            if (iteratorError) {
                paths.clear();
                return true;
            }
            std::size_t visited = 0u;
            for (auto end = fs::recursive_directory_iterator(); iterator != end;
                 iterator.increment(iteratorError)) {
                if (iteratorError) {
                    iteratorError.clear();
                    continue;
                }
                if (++visited > kMaximumSteamExecutableScanEntries) {
                    error = "Steam executable scan entry count exceeds the bound";
                    return false;
                }
                if (iterator.depth() >= static_cast<int>(kMaximumSteamExecutableScanDepth)) {
                    iterator.disable_recursion_pending();
                }
                std::error_code statusError;
                if (!iterator->is_regular_file(statusError) || statusError) continue;
                if (lowerPath(iterator->path().extension().wstring()) != L".exe") continue;
                const auto normalized = normalizedPath(iterator->path());
                unique.emplace(lowerPath(normalized), normalized);
            }
            paths.clear();
            for (auto& [key, value] : unique) {
                (void)key;
                paths.push_back(std::move(value));
                if (paths.size() >= profile::kMaximumExecutableCandidates) break;
            }
            return true;
        } catch (...) {
            error = "Steam executable-hint enumeration failed";
            return false;
        }
    }

    bool observeProcesses(std::span<const std::wstring> executablePaths,
                          std::vector<ProviderProcessEvidence>& processes,
                          std::string& error) noexcept override {
        try {
            std::set<std::wstring> expected;
            for (const auto& path : executablePaths) expected.insert(lowerPath(path));
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0u);
            if (snapshot == INVALID_HANDLE_VALUE) {
                error = "Steam process snapshot failed";
                return false;
            }
            struct HandleCloser {
                void operator()(void* handle) const noexcept {
                    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
                }
            };
            std::unique_ptr<void, HandleCloser> snapshotHandle(snapshot);
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            std::vector<ProviderProcessEvidence> candidate;
            if (Process32FirstW(snapshot, &entry)) {
                do {
                    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                                 entry.th32ProcessID);
                    if (!process) continue;
                    std::unique_ptr<void, HandleCloser> processHandle(process);
                    std::wstring path(32768u, L'\0');
                    DWORD pathSize = static_cast<DWORD>(path.size());
                    if (!QueryFullProcessImageNameW(process, 0u, path.data(), &pathSize)) continue;
                    path.resize(pathSize);
                    if (!expected.contains(lowerPath(path))) continue;
                    FILETIME created{}, exited{}, kernel{}, user{};
                    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) continue;
                    const std::uint64_t creation =
                        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32u) |
                        created.dwLowDateTime;
                    candidate.push_back(
                        {entry.th32ProcessID, creation, std::move(path), false});
                    if (candidate.size() >= kMaximumProcessEvidence) break;
                } while (Process32NextW(snapshot, &entry));
            }
            std::sort(candidate.begin(), candidate.end(), [](const auto& left, const auto& right) {
                if (left.processId != right.processId) return left.processId < right.processId;
                return left.creationTime100ns < right.creationTime100ns;
            });
            processes = std::move(candidate);
            return true;
        } catch (...) {
            error = "Steam process observation failed";
            return false;
        }
    }
};

#else

class NativeSteamMetadataSource final : public SteamMetadataSource {
public:
    SteamSourceResult locateInstallation(std::wstring&,
                                         std::string& error) noexcept override {
        error = "native Steam discovery is available only on Windows";
        return SteamSourceResult::NotInstalled;
    }
    bool readTextFile(const std::wstring&, std::size_t, std::string&,
                      std::string& error) noexcept override {
        error = "native Steam file access is unavailable";
        return false;
    }
    bool listManifestFiles(std::span<const std::wstring>, std::vector<std::wstring>&,
                           std::string& error) noexcept override {
        error = "native Steam file access is unavailable";
        return false;
    }
    bool listExecutableHints(const std::wstring&, std::vector<std::wstring>&,
                             std::string& error) noexcept override {
        error = "native Steam file access is unavailable";
        return false;
    }
    bool observeProcesses(std::span<const std::wstring>,
                          std::vector<ProviderProcessEvidence>&,
                          std::string& error) noexcept override {
        error = "native Steam process observation is unavailable";
        return false;
    }
};

#endif

} // namespace

std::shared_ptr<SteamMetadataSource> makeNativeSteamMetadataSource() {
    return std::make_shared<NativeSteamMetadataSource>();
}

SteamProviderAdapter::SteamProviderAdapter(std::shared_ptr<SteamMetadataSource> source)
    : source_(std::move(source)) {
    (void)refresh();
}

ProviderDiagnostic SteamProviderAdapter::refresh() noexcept {
    try {
        ProviderDescriptor descriptor;
        descriptor.providerId = "steam";
        std::vector<AppSnapshot> apps;
        std::string error;
        std::wstring steamRoot;
        if (!source_) {
            descriptor.availability = ProviderAvailability::Offline;
            descriptor.metadataRevision = 1u;
            descriptor_ = descriptor;
            apps_.clear();
            snapshotResult_ = ProviderResult::ProviderFailure;
            snapshotDiagnostic_ = "Steam metadata source is missing";
            return {snapshotResult_, snapshotDiagnostic_};
        }
        const auto located = source_->locateInstallation(steamRoot, error);
        if (located == SteamSourceResult::NotInstalled) {
            descriptor.availability = ProviderAvailability::Absent;
            descriptor_ = descriptor;
            apps_.clear();
            snapshotResult_ = ProviderResult::ProviderAbsent;
            snapshotDiagnostic_ = error;
            return {snapshotResult_, snapshotDiagnostic_};
        }
        if (located != SteamSourceResult::Success) {
            descriptor.availability = ProviderAvailability::Offline;
            descriptor.metadataRevision = 1u;
            descriptor_ = descriptor;
            apps_.clear();
            snapshotResult_ = ProviderResult::ProviderFailure;
            snapshotDiagnostic_ = error.empty() ? "Steam installation discovery failed" : error;
            return {snapshotResult_, snapshotDiagnostic_};
        }

        std::string libraryBytes;
        const auto libraryFile =
            (fs::path(steamRoot) / L"steamapps" / L"libraryfolders.vdf").wstring();
        if (!source_->readTextFile(libraryFile, kMaximumSteamMetadataBytes,
                                   libraryBytes, error)) {
            throw std::runtime_error(error);
        }
        std::vector<std::wstring> libraryRoots;
        if (!parseLibraryRoots(libraryBytes, steamRoot, libraryRoots, error)) {
            throw std::runtime_error(error);
        }
        std::vector<std::wstring> steamAppsRoots;
        steamAppsRoots.reserve(libraryRoots.size());
        for (const auto& root : libraryRoots) {
            steamAppsRoots.push_back((fs::path(root) / L"steamapps").wstring());
        }
        std::vector<std::wstring> manifests;
        if (!source_->listManifestFiles(steamAppsRoots, manifests, error)) {
            throw std::runtime_error(error);
        }
        if (manifests.size() > kMaximumSteamManifests) {
            throw std::runtime_error("Steam manifest count exceeds the bound");
        }
        std::sort(manifests.begin(), manifests.end(), [](const auto& left, const auto& right) {
            return lowerPath(left) < lowerPath(right);
        });
        if (std::adjacent_find(manifests.begin(), manifests.end(),
                               [](const auto& left, const auto& right) {
                                   return lowerPath(left) == lowerPath(right);
                               }) != manifests.end()) {
            throw std::runtime_error("duplicate Steam manifest path");
        }
        std::set<std::wstring> allowedSteamAppsRoots;
        for (const auto& root : steamAppsRoots) {
            allowedSteamAppsRoots.insert(lowerPath(normalizedPath(fs::path(root))));
        }

        std::uint64_t revision = hashWide(14695981039346656037ull, steamRoot);
        revision = hashBytes(revision, libraryBytes);
        std::set<std::string> appIds;
        for (const auto& manifestPath : manifests) {
            std::string bytes;
            if (!source_->readTextFile(manifestPath, kMaximumSteamMetadataBytes,
                                       bytes, error)) {
                throw std::runtime_error(error);
            }
            revision = hashWide(revision, lowerPath(manifestPath));
            revision = hashBytes(revision, bytes);

            const auto parent = fs::path(manifestPath).parent_path();
            if (lowerPath(parent.filename().wstring()) != L"steamapps" ||
                !allowedSteamAppsRoots.contains(lowerPath(normalizedPath(parent)))) {
                throw std::runtime_error("Steam manifest is outside a declared steamapps root");
            }
            const auto libraryRoot = parent.parent_path().wstring();
            AppSnapshot app;
            bool visibleInGameCatalog = true;
            if (!parseManifest(bytes, manifestPath, libraryRoot, app.appId, app.title,
                               app.installRoot, app.buildId, visibleInGameCatalog, error)) {
                throw std::runtime_error(error);
            }
            if (!appIds.insert(app.appId).second) {
                throw std::runtime_error("duplicate Steam appid across local manifests");
            }
            if (!visibleInGameCatalog) continue;
            if (!source_->listExecutableHints(app.installRoot, app.executableHints, error)) {
                throw std::runtime_error(error);
            }
            std::sort(app.executableHints.begin(), app.executableHints.end(),
                      [](const auto& left, const auto& right) {
                          return lowerPath(left) < lowerPath(right);
                      });
            app.executableHints.erase(
                std::unique(app.executableHints.begin(), app.executableHints.end(),
                            [](const auto& left, const auto& right) {
                                return lowerPath(left) == lowerPath(right);
                            }),
                app.executableHints.end());
            if (app.executableHints.size() > profile::kMaximumExecutableCandidates) {
                throw std::runtime_error("Steam executable hint count exceeds the bound");
            }
            if (app.executableHints.empty()) continue;
            for (const auto& executable : app.executableHints) {
                revision = hashWide(revision, lowerPath(executable));
            }
            app.candidate.providerId = "steam";
            app.candidate.providerAppId = app.appId;
            app.candidate.title = app.title;
            app.candidate.installRoot = app.installRoot;
            app.candidate.executableCandidates = app.executableHints;
            app.candidate.localVersion = app.buildId;
            app.candidate.origin = profile::GameOrigin::Discovered;
            app.candidate.localIconSource = app.executableHints.front();
            app.candidate.staleness = catalog::CatalogStaleness::Current;

            catalog::LocalGameCatalog catalog;
            const auto catalogResult = catalog::buildLocalGameCatalog(
                std::span<const catalog::GameCatalogCandidate>(&app.candidate, 1u), catalog);
            if (!catalogResult.succeeded() || catalog.entries.size() != 1u) {
                throw std::runtime_error("Steam candidate failed catalog validation: " +
                                         catalogResult.message);
            }
            app.gameId = catalog.entries.front().game.gameId;
            apps.push_back(std::move(app));
        }
        std::sort(apps.begin(), apps.end(), [](const auto& left, const auto& right) {
            return left.appId < right.appId;
        });
        if (revision == 0u) revision = 1u;
        descriptor.availability = ProviderAvailability::Available;
        descriptor.metadataRevision = revision;
        descriptor.capabilities = {true, false, true, false, true};
        descriptor_ = descriptor;
        apps_ = std::move(apps);
        snapshotResult_ = ProviderResult::Success;
        snapshotDiagnostic_.clear();
        return {};
    } catch (const std::exception& exception) {
        descriptor_.providerId = "steam";
        descriptor_.availability = ProviderAvailability::Offline;
        descriptor_.metadataRevision = 1u;
        descriptor_.capabilities = {true, false, true, false, true};
        apps_.clear();
        snapshotResult_ = ProviderResult::ProviderFailure;
        snapshotDiagnostic_ = exception.what();
        if (snapshotDiagnostic_.size() > kMaximumProviderDiagnosticBytes) {
            snapshotDiagnostic_.resize(kMaximumProviderDiagnosticBytes);
        }
        return {snapshotResult_, snapshotDiagnostic_};
    } catch (...) {
        descriptor_.providerId = "steam";
        descriptor_.availability = ProviderAvailability::Offline;
        descriptor_.metadataRevision = 1u;
        descriptor_.capabilities = {true, false, true, false, true};
        apps_.clear();
        snapshotResult_ = ProviderResult::ProviderFailure;
        snapshotDiagnostic_ = "Steam metadata refresh failed";
        return {snapshotResult_, snapshotDiagnostic_};
    }
}

ProviderDescriptor SteamProviderAdapter::descriptor() const noexcept {
    return descriptor_;
}

DiscoveryResponse SteamProviderAdapter::discoverInstalledGames() noexcept {
    try {
        DiscoveryResponse response;
        response.result = snapshotResult_;
        response.metadataRevision = descriptor_.metadataRevision;
        response.diagnostic = snapshotDiagnostic_;
        if (snapshotResult_ != ProviderResult::Success) return response;
        response.candidates.reserve(apps_.size());
        for (const auto& app : apps_) response.candidates.push_back(app.candidate);
        return response;
    } catch (...) {
        return {ProviderResult::ProviderFailure, descriptor_.metadataRevision, {},
                "Steam discovery response allocation failed"};
    }
}

AccountReferenceResponse SteamProviderAdapter::listAccountReferences() noexcept {
    return {ProviderResult::UnsupportedOperation, descriptor_.metadataRevision, {},
            "Steam authentication/account selection remains owned by the Steam client"};
}

LaunchResponse SteamProviderAdapter::buildLaunchRequest(
    const LaunchSelection& selection) noexcept {
    try {
        if (snapshotResult_ != ProviderResult::Success) {
            return {snapshotResult_, {}, snapshotDiagnostic_};
        }
        if (!selection.providerAppId) {
            return {ProviderResult::InvalidRequest, {},
                    "Steam launch requires a provider appid"};
        }
        const auto* app = findApp(*selection.providerAppId);
        if (!app || app->gameId != selection.gameId) {
            return {ProviderResult::InvalidRequest, {},
                    "Steam launch selection does not match the discovered app"};
        }
        if (selection.accountRef) {
            return {ProviderResult::UnsupportedOperation, {},
                    "Steam account selection is not exposed by this adapter"};
        }
        if (!selection.instanceArguments.empty()) {
            return {ProviderResult::UnsupportedOperation, {},
                    "Steam instance arguments are not enabled without an exact supported profile"};
        }
        ProviderLaunchRequest request;
        request.providerId = "steam";
        request.gameId = app->gameId;
        request.providerAppId = app->appId;
        request.metadataRevision = descriptor_.metadataRevision;
        request.targetKind = LaunchTargetKind::ProviderUri;
        std::wstring wideAppId;
        if (!utf8ToWide(app->appId, wideAppId)) {
            return {ProviderResult::InvalidMetadata, {}, "Steam appid encoding failed"};
        }
        request.target = L"steam://run/" + wideAppId;
        request.launchCorrelationId =
            "steam-" + app->appId + "-" + decimal(descriptor_.metadataRevision);
        return {ProviderResult::Success, std::move(request), {}};
    } catch (...) {
        return {ProviderResult::ProviderFailure, {}, "Steam launch request construction failed"};
    }
}

ProcessIdentificationResponse SteamProviderAdapter::identifyProcesses(
    const ProcessIdentificationQuery& query) noexcept {
    try {
        ProcessIdentificationResponse response;
        response.result = snapshotResult_;
        response.metadataRevision = descriptor_.metadataRevision;
        response.diagnostic = snapshotDiagnostic_;
        if (snapshotResult_ != ProviderResult::Success) return response;
        if (!query.providerAppId) {
            response.result = ProviderResult::InvalidRequest;
            response.diagnostic = "Steam process query requires a provider appid";
            return response;
        }
        const auto* app = findApp(*query.providerAppId);
        if (!app || app->gameId != query.gameId) {
            response.result = ProviderResult::InvalidRequest;
            response.diagnostic = "Steam process query does not match the discovered app";
            return response;
        }
        std::string error;
        if (!source_->observeProcesses(app->executableHints, response.processes, error)) {
            response.result = ProviderResult::ProviderFailure;
            response.diagnostic = error;
            response.processes.clear();
            return response;
        }
        for (auto& process : response.processes) {
            // Exact path/PID/creation identity is useful candidate evidence, but
            // local path matching alone is not proof that Steam launched it.
            process.providerRelationshipVerified = false;
        }
        response.result = ProviderResult::Success;
        return response;
    } catch (...) {
        return {ProviderResult::ProviderFailure, descriptor_.metadataRevision, {},
                "Steam process observation failed"};
    }
}

const SteamProviderAdapter::AppSnapshot* SteamProviderAdapter::findApp(
    std::string_view appId) const noexcept {
    const auto found = std::lower_bound(
        apps_.begin(), apps_.end(), appId,
        [](const auto& app, std::string_view expected) { return app.appId < expected; });
    return found != apps_.end() && found->appId == appId ? &*found : nullptr;
}

std::string_view steamSourceResultName(SteamSourceResult result) noexcept {
    switch (result) {
    case SteamSourceResult::Success: return "success";
    case SteamSourceResult::NotInstalled: return "not-installed";
    case SteamSourceResult::Failure: return "failure";
    }
    return "unknown";
}

} // namespace hydra::provider::steam
