#include "hydra/installer_bootstrap.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#endif

namespace hydra::installer {
namespace {

constexpr std::array<std::wstring_view, 9> kArchitectureFiles{
    L"HydraSeatSetup.exe",
    L"HydraSeat.exe",
    L"hydra_host.exe",
    L"hydra_seat_ui.exe",
    L"hydra_watchdog.exe",
    L"hydra_reset.exe",
    L"hydraseat_profilectl.exe",
    L"hydraseat_community_validate.exe",
    L"install_hydraseat.ps1",
};

constexpr std::array<std::wstring_view, 3> kPackageRootEntries{
    L"x64",
    L"signing-provenance.json",
    L"signing-provenance.json.p7s",
};

void setError(std::wstring* error, std::wstring message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::wstring lower(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    });
    return result;
}

template <std::size_t N>
bool exactNames(const std::vector<std::wstring>& actual,
                const std::array<std::wstring_view, N>& expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    std::vector<std::wstring> normalizedActual;
    normalizedActual.reserve(actual.size());
    for (const auto& entry : actual) {
        normalizedActual.push_back(lower(entry));
    }
    std::sort(normalizedActual.begin(), normalizedActual.end());
    if (std::adjacent_find(normalizedActual.begin(), normalizedActual.end()) !=
        normalizedActual.end()) {
        return false;
    }

    std::vector<std::wstring> normalizedExpected;
    normalizedExpected.reserve(expected.size());
    for (const auto entry : expected) {
        normalizedExpected.push_back(lower(entry));
    }
    std::sort(normalizedExpected.begin(), normalizedExpected.end());
    return normalizedActual == normalizedExpected;
}

bool isRequiredFileType(BootstrapPathType type) noexcept {
    return type == BootstrapPathType::RegularFile;
}

bool isReparse(BootstrapPathType type) noexcept {
    return type == BootstrapPathType::ReparsePoint;
}

bool containsUnsafeCommandPathCharacter(std::wstring_view value) noexcept {
    return value.find(L'\0') != std::wstring_view::npos ||
           value.find(L'\r') != std::wstring_view::npos ||
           value.find(L'\n') != std::wstring_view::npos;
}

bool isSafeReleaseVersion(std::wstring_view value) noexcept {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    for (const wchar_t c : value) {
        const bool allowed = (c >= L'a' && c <= L'z') ||
                             (c >= L'A' && c <= L'Z') ||
                             (c >= L'0' && c <= L'9') ||
                             c == L'.' || c == L'_' || c == L'+' || c == L'-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

#ifdef _WIN32

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : m_value(value) {}
    ~UniqueHandle() {
        if (m_value != nullptr && m_value != INVALID_HANDLE_VALUE) {
            CloseHandle(m_value);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : m_value(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    HANDLE get() const noexcept { return m_value; }
    HANDLE release() noexcept {
        const auto value = m_value;
        m_value = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (m_value != nullptr && m_value != INVALID_HANDLE_VALUE) {
            CloseHandle(m_value);
        }
        m_value = value;
    }

private:
    HANDLE m_value{nullptr};
};

class UniqueRegistryKey {
public:
    explicit UniqueRegistryKey(HKEY value = nullptr) : m_value(value) {}
    ~UniqueRegistryKey() {
        if (m_value != nullptr) {
            RegCloseKey(m_value);
        }
    }
    UniqueRegistryKey(const UniqueRegistryKey&) = delete;
    UniqueRegistryKey& operator=(const UniqueRegistryKey&) = delete;
    HKEY get() const noexcept { return m_value; }

private:
    HKEY m_value{nullptr};
};

BootstrapPathType pathType(const std::filesystem::path& path) noexcept {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return BootstrapPathType::Missing;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return BootstrapPathType::ReparsePoint;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return BootstrapPathType::Directory;
    }
    if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0) {
        return BootstrapPathType::Other;
    }
    return BootstrapPathType::RegularFile;
}

bool trustedAuthenticodeSignerThumbprint(
    const std::filesystem::path& path,
    std::array<std::uint8_t, 20>& thumbprint) noexcept {
    if (pathType(path) != BootstrapPathType::RegularFile) {
        return false;
    }

    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();

    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto status = WinVerifyTrust(nullptr, &policy, &trustData);
    bool success = false;
    if (status == ERROR_SUCCESS && trustData.hWVTStateData != nullptr) {
        auto* providerData = WTHelperProvDataFromStateData(trustData.hWVTStateData);
        auto* signer = providerData == nullptr
            ? nullptr
            : WTHelperGetProvSignerFromChain(providerData, 0, FALSE, 0);
        if (signer != nullptr && signer->csCertChain > 0 &&
            signer->pasCertChain != nullptr && signer->pasCertChain[0].pCert != nullptr) {
            DWORD bytes = static_cast<DWORD>(thumbprint.size());
            success = CertGetCertificateContextProperty(
                          signer->pasCertChain[0].pCert, CERT_SHA1_HASH_PROP_ID,
                          thumbprint.data(), &bytes) != FALSE &&
                      bytes == thumbprint.size();
        }
    }

    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(nullptr, &policy, &trustData);
    return success;
}

bool bootstrapAndInstallerShareTrustedSigner(
    const std::filesystem::path& bootstrap,
    const std::filesystem::path& installerScript) noexcept {
    std::array<std::uint8_t, 20> bootstrapSigner{};
    std::array<std::uint8_t, 20> installerSigner{};
    return trustedAuthenticodeSignerThumbprint(bootstrap, bootstrapSigner) &&
           trustedAuthenticodeSignerThumbprint(installerScript, installerSigner) &&
           bootstrapSigner == installerSigner;
}

bool collectDirectoryEntries(const std::filesystem::path& directory,
                             std::vector<std::wstring>& entries) {
    entries.clear();
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return false;
    }
    constexpr std::size_t kMaximumEntries = 32;
    for (const auto& entry : iterator) {
        if (entries.size() >= kMaximumEntries) {
            return false;
        }
        entries.push_back(entry.path().filename().wstring());
    }
    return true;
}

std::optional<std::wstring> readRegistryString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t) ||
        bytes > 4096) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type,
                        reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

std::filesystem::path programFilesRoot(std::wstring* error) {
    PWSTR value = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT,
                                             nullptr, &value);
    if (FAILED(result) || value == nullptr) {
        setError(error, L"Could not resolve the Windows Program Files folder.");
        return {};
    }
    std::filesystem::path path(value);
    CoTaskMemFree(value);
    return path;
}

std::optional<std::string> extractJsonString(const std::string& text,
                                             std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto keyPosition = text.find(needle);
    if (keyPosition == std::string::npos) {
        return std::nullopt;
    }
    const auto colon = text.find(':', keyPosition + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    const auto quote = text.find('"', colon + 1);
    if (quote == std::string::npos) {
        return std::nullopt;
    }
    const auto endQuote = text.find('"', quote + 1);
    if (endQuote == std::string::npos || endQuote - quote - 1 > 64) {
        return std::nullopt;
    }
    return text.substr(quote + 1, endQuote - quote - 1);
}

std::optional<std::uint64_t> extractJsonUnsigned(const std::string& text,
                                                 std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto keyPosition = text.find(needle);
    if (keyPosition == std::string::npos) {
        return std::nullopt;
    }
    const auto colon = text.find(':', keyPosition + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    std::size_t start = colon + 1;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t' ||
                                   text[start] == '\r' || text[start] == '\n')) {
        ++start;
    }
    std::size_t end = start;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
        ++end;
    }
    if (start == end || end - start > 20) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto conversion = std::from_chars(text.data() + start, text.data() + end, value);
    if (conversion.ec != std::errc{} || conversion.ptr != text.data() + end || value == 0) {
        return std::nullopt;
    }
    return value;
}

BootstrapProcessResult waitForProcess(HANDLE process,
                                      std::uint32_t* processExitCode,
                                      std::uint32_t* systemError) {
    if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) {
        if (systemError != nullptr) {
            *systemError = GetLastError();
        }
        return BootstrapProcessResult::Failed;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process, &exitCode)) {
        if (systemError != nullptr) {
            *systemError = GetLastError();
        }
        return BootstrapProcessResult::Failed;
    }
    if (processExitCode != nullptr) {
        *processExitCode = exitCode;
    }
    return mapBootstrapProcessResult(exitCode);
}

#endif

} // namespace

std::wstring_view bootstrapPowerShellMode(BootstrapOperation operation) noexcept {
    switch (operation) {
    case BootstrapOperation::Install: return L"Install";
    case BootstrapOperation::Repair: return L"Repair";
    case BootstrapOperation::Uninstall: return L"Uninstall";
    case BootstrapOperation::Validate: return L"Validate";
    }
    return {};
}

std::wstring quoteWindowsCommandLineArgument(std::wstring_view argument) {
    if (argument.empty()) {
        return L"\"\"";
    }

    const bool needsQuotes = argument.find_first_of(L" \t\"") != std::wstring_view::npos;
    if (!needsQuotes) {
        return std::wstring(argument);
    }

    std::wstring result;
    result.reserve(argument.size() + 2);
    result.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(c);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

BootstrapProcessResult mapBootstrapProcessResult(std::uint32_t exitCode) noexcept {
    if (exitCode == 0) {
        return BootstrapProcessResult::Success;
    }
    if (exitCode == 1223u || exitCode == 1602u) {
        return BootstrapProcessResult::Cancelled;
    }
    return BootstrapProcessResult::Failed;
}

std::vector<std::wstring> expectedBootstrapArchitectureFiles() {
    std::vector<std::wstring> result;
    result.reserve(kArchitectureFiles.size());
    for (const auto file : kArchitectureFiles) {
        result.emplace_back(file);
    }
    return result;
}

std::vector<std::wstring> expectedBootstrapPackageRootEntries() {
    std::vector<std::wstring> result;
    result.reserve(kPackageRootEntries.size());
    for (const auto entry : kPackageRootEntries) {
        result.emplace_back(entry);
    }
    return result;
}

BootstrapPackageAssessment assessBootstrapPackageFacts(
    const BootstrapPackageFacts& facts) {
    BootstrapPackageAssessment assessment;
    if (lower(facts.architectureDirectoryName) != L"x64") {
        assessment.status = BootstrapPackageStatus::NotReleaseLayout;
        assessment.diagnostic = L"HydraSeatSetup.exe must run from the reviewed x64 package directory.";
        return assessment;
    }

    for (const auto type : {facts.packageRoot, facts.architectureDirectory,
                            facts.setupExecutable, facts.installerScript,
                            facts.signingProvenance, facts.signingProvenanceSignature}) {
        if (isReparse(type)) {
            assessment.status = BootstrapPackageStatus::ReparsePointRejected;
            assessment.diagnostic = L"The setup package contains a reparse point in an authoritative path.";
            return assessment;
        }
    }

    if (facts.packageRoot != BootstrapPathType::Directory ||
        facts.architectureDirectory != BootstrapPathType::Directory ||
        !isRequiredFileType(facts.setupExecutable) ||
        !isRequiredFileType(facts.installerScript) ||
        !isRequiredFileType(facts.signingProvenance) ||
        !isRequiredFileType(facts.signingProvenanceSignature)) {
        assessment.status = BootstrapPackageStatus::MissingRequiredPath;
        assessment.diagnostic = L"The setup package is missing a required signed package path.";
        return assessment;
    }

    if (!exactNames(facts.packageRootEntries, kPackageRootEntries) ||
        !exactNames(facts.architectureEntries, kArchitectureFiles)) {
        assessment.status = BootstrapPackageStatus::UnexpectedLayout;
        assessment.diagnostic = L"The setup package contains missing, duplicate, or unexpected files.";
        return assessment;
    }

    assessment.status = BootstrapPackageStatus::Valid;
    assessment.diagnostic = L"The setup package layout is exact.";
    return assessment;
}

bool makeBootstrapPowerShellInvocation(
    BootstrapOperation operation,
    const std::filesystem::path& powershellExecutable,
    const std::filesystem::path& installerScript,
    const std::optional<std::filesystem::path>& packageRoot,
    const std::optional<BootstrapReleaseIdentity>& expectedRelease,
    bool removeHydraSeatUserData,
    BootstrapPowerShellInvocation& invocation,
    std::wstring* error) {
    const auto mode = bootstrapPowerShellMode(operation);
    if (mode.empty()) {
        setError(error, L"Unsupported installer operation.");
        return false;
    }
    if (!powershellExecutable.is_absolute() || !installerScript.is_absolute()) {
        setError(error, L"PowerShell and installer script paths must be absolute.");
        return false;
    }
    const auto powershellText = powershellExecutable.wstring();
    const auto installerText = installerScript.wstring();
    if (containsUnsafeCommandPathCharacter(powershellText) ||
        containsUnsafeCommandPathCharacter(installerText)) {
        setError(error, L"Installer command paths contain unsupported control characters.");
        return false;
    }

    const bool needsPackage = operation == BootstrapOperation::Install ||
                              operation == BootstrapOperation::Repair ||
                              operation == BootstrapOperation::Validate;
    if (needsPackage) {
        if (!packageRoot || !packageRoot->is_absolute() ||
            containsUnsafeCommandPathCharacter(packageRoot->wstring())) {
            setError(error, L"This installer operation requires one exact absolute package root.");
            return false;
        }
    }
    const bool needsExpectedRelease = operation == BootstrapOperation::Install ||
                                      operation == BootstrapOperation::Repair;
    if (needsExpectedRelease) {
        if (!expectedRelease || !isSafeReleaseVersion(expectedRelease->version) ||
            expectedRelease->revision == 0) {
            setError(error, L"Install/Repair require the exact user-confirmed release identity.");
            return false;
        }
    } else if (expectedRelease) {
        setError(error, L"Expected release identity is valid only for Install/Repair.");
        return false;
    }
    if (removeHydraSeatUserData && operation != BootstrapOperation::Uninstall) {
        setError(error, L"Per-user data removal is valid only for Uninstall.");
        return false;
    }

    invocation = {};
    invocation.executable = powershellExecutable;
    invocation.requestElevation = operation != BootstrapOperation::Validate;
    invocation.parameters = L"-NoLogo -NoProfile -NonInteractive -ExecutionPolicy AllSigned -File ";
    invocation.parameters += quoteWindowsCommandLineArgument(installerText);
    invocation.parameters += L" -Mode ";
    invocation.parameters += mode;
    if (needsPackage) {
        invocation.parameters += L" -PackageRoot ";
        invocation.parameters += quoteWindowsCommandLineArgument(packageRoot->wstring());
    }
    if (expectedRelease) {
        invocation.parameters += L" -ExpectedReleaseVersion ";
        invocation.parameters += quoteWindowsCommandLineArgument(expectedRelease->version);
        invocation.parameters += L" -ExpectedReleaseRevision ";
        invocation.parameters += std::to_wstring(expectedRelease->revision);
    }
    if (removeHydraSeatUserData) {
        invocation.parameters += L" -RemoveHydraSeatUserData";
    }
    return true;
}

#ifdef _WIN32

bool inspectBootstrapPackageLayout(
    const std::filesystem::path& setupExecutable,
    BootstrapPackageLayout& layout,
    BootstrapPackageAssessment& assessment) {
    layout = {};
    std::error_code canonicalError;
    const auto absoluteSetup = std::filesystem::absolute(setupExecutable, canonicalError);
    if (canonicalError || absoluteSetup.empty()) {
        assessment.status = BootstrapPackageStatus::MissingRequiredPath;
        assessment.diagnostic = L"The setup executable path could not be resolved.";
        return false;
    }

    layout.setupExecutable = absoluteSetup.lexically_normal();
    layout.architectureDirectory = layout.setupExecutable.parent_path();
    layout.packageRoot = layout.architectureDirectory.parent_path();
    layout.installerScript = layout.architectureDirectory / L"install_hydraseat.ps1";
    layout.signingProvenance = layout.packageRoot / L"signing-provenance.json";
    layout.signingProvenanceSignature = layout.packageRoot / L"signing-provenance.json.p7s";

    BootstrapPackageFacts facts;
    facts.architectureDirectoryName = layout.architectureDirectory.filename().wstring();
    // Fail early on raw build outputs such as ...\Release\HydraSeatSetup.exe.
    // Enumerating the parent build tree first can hit the bounded-entry guard and
    // produce the misleading "could not be enumerated safely" diagnostic even
    // though the actual problem is simply that this is not a release package.
    if (lower(facts.architectureDirectoryName) != L"x64") {
        assessment = assessBootstrapPackageFacts(facts);
        return false;
    }
    facts.packageRoot = pathType(layout.packageRoot);
    facts.architectureDirectory = pathType(layout.architectureDirectory);
    facts.setupExecutable = pathType(layout.setupExecutable);
    facts.installerScript = pathType(layout.installerScript);
    facts.signingProvenance = pathType(layout.signingProvenance);
    facts.signingProvenanceSignature = pathType(layout.signingProvenanceSignature);
    if (facts.packageRoot == BootstrapPathType::Directory) {
        if (!collectDirectoryEntries(layout.packageRoot, facts.packageRootEntries)) {
            assessment.status = BootstrapPackageStatus::UnexpectedLayout;
            assessment.diagnostic = L"The package root could not be enumerated safely.";
            return false;
        }
    }
    if (facts.architectureDirectory == BootstrapPathType::Directory) {
        if (!collectDirectoryEntries(layout.architectureDirectory, facts.architectureEntries)) {
            assessment.status = BootstrapPackageStatus::UnexpectedLayout;
            assessment.diagnostic = L"The x64 package directory could not be enumerated safely.";
            return false;
        }
    }

    assessment = assessBootstrapPackageFacts(facts);
    if (!assessment.valid()) {
        return false;
    }
    if (!bootstrapAndInstallerShareTrustedSigner(
            layout.setupExecutable, layout.installerScript)) {
        assessment.status = BootstrapPackageStatus::SignatureRejected;
        assessment.diagnostic =
            L"HydraSeatSetup.exe and install_hydraseat.ps1 must both have valid Authenticode signatures from the same publisher certificate.";
        return false;
    }
    return true;
}

std::filesystem::path systemWindowsPowerShellPath(std::wstring* error) {
    std::array<wchar_t, MAX_PATH + 1> buffer{};
    const auto length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        setError(error, L"Could not resolve the Windows system directory.");
        return {};
    }
    std::filesystem::path path(std::wstring(buffer.data(), length));
    path /= L"WindowsPowerShell";
    path /= L"v1.0";
    path /= L"powershell.exe";
    if (pathType(path) != BootstrapPathType::RegularFile) {
        setError(error, L"The exact system Windows PowerShell executable is missing or unsafe.");
        return {};
    }
    return path;
}

BootstrapProcessResult runBootstrapPowerShell(
    const BootstrapPowerShellInvocation& invocation,
    std::uint32_t* processExitCode,
    std::uint32_t* systemError) {
    if (processExitCode != nullptr) {
        *processExitCode = std::numeric_limits<std::uint32_t>::max();
    }
    if (systemError != nullptr) {
        *systemError = 0;
    }

    if (invocation.requestElevation) {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas";
        info.lpFile = invocation.executable.c_str();
        info.lpParameters = invocation.parameters.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            const auto error = GetLastError();
            if (systemError != nullptr) {
                *systemError = error;
            }
            return error == ERROR_CANCELLED ? BootstrapProcessResult::Cancelled
                                            : BootstrapProcessResult::Failed;
        }
        UniqueHandle process(info.hProcess);
        if (process.get() == nullptr) {
            if (systemError != nullptr) {
                *systemError = ERROR_INVALID_HANDLE;
            }
            return BootstrapProcessResult::Failed;
        }
        return waitForProcess(process.get(), processExitCode, systemError);
    }

    std::wstring commandLine = quoteWindowsCommandLineArgument(invocation.executable.wstring());
    commandLine.push_back(L' ');
    commandLine += invocation.parameters;
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(invocation.executable.c_str(), mutableCommandLine.data(),
                        nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &processInfo)) {
        if (systemError != nullptr) {
            *systemError = GetLastError();
        }
        return BootstrapProcessResult::Failed;
    }
    UniqueHandle process(processInfo.hProcess);
    UniqueHandle thread(processInfo.hThread);
    return waitForProcess(process.get(), processExitCode, systemError);
}

BootstrapInstalledInfo queryBootstrapInstalledInfo() {
    BootstrapInstalledInfo info;
    std::wstring rootError;
    const auto programFiles = programFilesRoot(&rootError);
    if (programFiles.empty()) {
        info.state = BootstrapInstalledState::Inconsistent;
        info.diagnostic = rootError;
        return info;
    }
    info.installRoot = programFiles / L"HydraSeat";

    HKEY rawKey = nullptr;
    const auto openResult = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HydraSeat",
        0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &rawKey);
    const bool installRootExists = pathType(info.installRoot) != BootstrapPathType::Missing;
    if (openResult == ERROR_FILE_NOT_FOUND) {
        info.state = installRootExists ? BootstrapInstalledState::Inconsistent
                                       : BootstrapInstalledState::NotInstalled;
        info.diagnostic = installRootExists
            ? L"HydraSeat files exist without the reviewed uninstall registration."
            : L"HydraSeat is not installed.";
        return info;
    }
    if (openResult != ERROR_SUCCESS || rawKey == nullptr) {
        info.state = BootstrapInstalledState::Inconsistent;
        info.diagnostic = L"The installed-state registration could not be read.";
        return info;
    }
    UniqueRegistryKey keyHandle(rawKey);
    const auto version = readRegistryString(keyHandle.get(), L"DisplayVersion");
    const auto installLocation = readRegistryString(keyHandle.get(), L"InstallLocation");
    if (!version || !installLocation || !isSafeReleaseVersion(*version)) {
        info.state = BootstrapInstalledState::Inconsistent;
        info.diagnostic = L"The installed-state registration is incomplete or malformed.";
        return info;
    }
    const std::filesystem::path registeredRoot(*installLocation);
    if (lower(registeredRoot.lexically_normal().wstring()) !=
        lower(info.installRoot.lexically_normal().wstring())) {
        info.state = BootstrapInstalledState::Inconsistent;
        info.diagnostic = L"The registered install location differs from the fixed HydraSeat destination.";
        return info;
    }
    if (pathType(info.installRoot) != BootstrapPathType::Directory ||
        pathType(info.installRoot / L"HydraSeat.exe") != BootstrapPathType::RegularFile ||
        pathType(info.installRoot / L"install_hydraseat.ps1") != BootstrapPathType::RegularFile) {
        info.state = BootstrapInstalledState::Inconsistent;
        info.diagnostic = L"The registered HydraSeat install is missing required normal files.";
        return info;
    }

    info.state = BootstrapInstalledState::Installed;
    info.version = *version;
    info.diagnostic = L"HydraSeat is installed.";
    return info;
}

std::optional<BootstrapReleaseIdentity> readValidatedBootstrapReleaseIdentity(
    const std::filesystem::path& signingProvenance,
    std::wstring* error) {
    if (pathType(signingProvenance) != BootstrapPathType::RegularFile) {
        setError(error, L"Validated signing provenance is missing or unsafe.");
        return std::nullopt;
    }
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(signingProvenance, sizeError);
    constexpr std::uintmax_t kMaximumProvenanceBytes = 262144;
    if (sizeError || size == 0 || size > kMaximumProvenanceBytes) {
        setError(error, L"Validated signing provenance size is invalid.");
        return std::nullopt;
    }
    std::ifstream stream(signingProvenance, std::ios::binary);
    if (!stream) {
        setError(error, L"Validated signing provenance could not be read.");
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(text.size())) {
        setError(error, L"Validated signing provenance could not be read completely.");
        return std::nullopt;
    }

    const auto version = extractJsonString(text, "releaseVersion");
    const auto revision = extractJsonUnsigned(text, "releaseRevision");
    if (!version || !revision) {
        setError(error, L"Validated signing provenance release identity is missing.");
        return std::nullopt;
    }
    std::wstring wideVersion(version->begin(), version->end());
    if (!isSafeReleaseVersion(wideVersion)) {
        setError(error, L"Validated signing provenance release version is invalid.");
        return std::nullopt;
    }
    return BootstrapReleaseIdentity{std::move(wideVersion), *revision};
}

bool launchInstalledHydraSeatNormally(
    const BootstrapInstalledInfo& installed,
    std::uint32_t* systemError) {
    if (systemError != nullptr) {
        *systemError = 0;
    }
    if (installed.state != BootstrapInstalledState::Installed) {
        if (systemError != nullptr) {
            *systemError = ERROR_INVALID_STATE;
        }
        return false;
    }
    const auto executable = installed.installRoot / L"HydraSeat.exe";
    if (pathType(executable) != BootstrapPathType::RegularFile) {
        if (systemError != nullptr) {
            *systemError = ERROR_FILE_NOT_FOUND;
        }
        return false;
    }
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", executable.c_str(), nullptr,
                      installed.installRoot.c_str(), SW_SHOWNORMAL));
    if (result <= 32) {
        if (systemError != nullptr) {
            *systemError = static_cast<std::uint32_t>(result);
        }
        return false;
    }
    return true;
}

#endif

} // namespace hydra::installer
