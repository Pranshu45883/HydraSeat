#include "hydra/custom_executable_provider.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
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

namespace hydra::provider::custom {
namespace {

namespace fs = std::filesystem;

bool validText(std::wstring_view value, std::size_t maximum, bool allowEmpty = false) {
    if ((!allowEmpty && value.empty()) || value.size() > maximum) return false;
    for (const wchar_t ch : value) {
        const auto unit = static_cast<std::uint32_t>(ch);
        if (unit == 0u || unit == L'\r' || unit == L'\n') return false;
    }
    return true;
}

bool absoluteWindowsPath(std::wstring_view value) {
    if (!validText(value, profile::kMaximumPathCodeUnits)) return false;
    const bool drive = value.size() >= 3u &&
                       ((value[0] >= L'A' && value[0] <= L'Z') ||
                        (value[0] >= L'a' && value[0] <= L'z')) &&
                       value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
    const bool unc = value.size() >= 3u &&
                     ((value[0] == L'\\' && value[1] == L'\\') ||
                      (value[0] == L'/' && value[1] == L'/'));
    if (!drive && !unc) return false;
    std::size_t componentStart = drive ? 3u : 2u;
    for (std::size_t index = componentStart; index <= value.size(); ++index) {
        const bool boundary = index == value.size() || value[index] == L'\\' ||
                              value[index] == L'/';
        if (!boundary) {
            const wchar_t ch = value[index];
            if (ch < L' ' || ch == L'"' || ch == L'<' || ch == L'>' ||
                ch == L'|' || ch == L'?' || ch == L'*' || ch == L':') {
                return false;
            }
            continue;
        }
        if (index > componentStart &&
            (value[index - 1u] == L' ' || value[index - 1u] == L'.')) {
            return false;
        }
        componentStart = index + 1u;
    }
    return true;
}

std::wstring lowerPath(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    for (wchar_t& ch : value) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return value;
}

std::wstring normalizedPath(const fs::path& value) {
    std::error_code error;
    const auto absolute = fs::absolute(value, error);
    return (error ? value : absolute).lexically_normal().wstring();
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

std::string hex64(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(16u, '0');
    for (std::size_t index = 0u; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((output.size() - index - 1u) * 4u);
        output[index] = digits[(value >> shift) & 0x0fu];
    }
    return output;
}

ProviderDiagnostic validateDefinition(const CustomExecutableDefinition& definition) {
    if (!validText(definition.title, profile::kMaximumTitleCodeUnits) ||
        !absoluteWindowsPath(definition.executablePath)) {
        return {ProviderResult::InvalidRequest,
                "custom executable title/path is invalid or unbounded"};
    }
    if (lowerPath(fs::path(definition.executablePath).extension().wstring()) != L".exe") {
        return {ProviderResult::InvalidRequest,
                "custom executable path must name an .exe file"};
    }
    if (definition.arguments.size() > kMaximumLaunchArguments) {
        return {ProviderResult::InvalidRequest,
                "custom executable argument count exceeds the bound"};
    }
    for (const auto& argument : definition.arguments) {
        if (!validText(argument, kMaximumLaunchArgumentCodeUnits, true)) {
            return {ProviderResult::InvalidRequest,
                    "custom executable contains an invalid argument"};
        }
    }
    if (definition.workingDirectory &&
        !absoluteWindowsPath(*definition.workingDirectory)) {
        return {ProviderResult::InvalidRequest,
                "custom executable working directory must be absolute"};
    }
    if (definition.localIconSource &&
        !absoluteWindowsPath(*definition.localIconSource)) {
        return {ProviderResult::InvalidRequest,
                "custom executable icon source must be absolute"};
    }
    return {};
}

#ifdef _WIN32

class NativeCustomExecutableSource final : public CustomExecutableSource {
public:
    CustomExecutableSourceResult inspect(
        const CustomExecutableDefinition& definition,
        CustomExecutableObservation& observation,
        std::string& error) noexcept override {
        try {
            std::error_code fsError;
            const auto executable = fs::canonical(fs::path(definition.executablePath), fsError);
            if (fsError || !fs::is_regular_file(executable, fsError) || fsError) {
                error = "custom executable does not exist as a regular file";
                return CustomExecutableSourceResult::NotFound;
            }
            const auto workingDirectory = definition.workingDirectory
                ? fs::canonical(fs::path(*definition.workingDirectory), fsError)
                : executable.parent_path();
            if (fsError || !fs::is_directory(workingDirectory, fsError) || fsError) {
                error = "custom executable working directory does not exist";
                return CustomExecutableSourceResult::NotFound;
            }
            const auto icon = definition.localIconSource
                ? fs::canonical(fs::path(*definition.localIconSource), fsError)
                : executable;
            if (fsError || !fs::is_regular_file(icon, fsError) || fsError) {
                error = "custom executable icon source does not exist";
                return CustomExecutableSourceResult::NotFound;
            }

            std::ifstream input(executable, std::ios::binary);
            if (!input) {
                error = "custom executable could not be opened read-only";
                return CustomExecutableSourceResult::Failure;
            }
            IMAGE_DOS_HEADER dos{};
            input.read(reinterpret_cast<char*>(&dos), sizeof(dos));
            if (!input || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
                error = "custom executable has no valid PE DOS header";
                return CustomExecutableSourceResult::InvalidExecutable;
            }
            input.seekg(dos.e_lfanew, std::ios::beg);
            DWORD signature = 0u;
            IMAGE_FILE_HEADER fileHeader{};
            input.read(reinterpret_cast<char*>(&signature), sizeof(signature));
            input.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
            if (!input || signature != IMAGE_NT_SIGNATURE) {
                error = "custom executable has no valid PE signature";
                return CustomExecutableSourceResult::InvalidExecutable;
            }
            catalog::ExecutableArchitecture architecture =
                catalog::ExecutableArchitecture::Unknown;
            switch (fileHeader.Machine) {
            case IMAGE_FILE_MACHINE_I386:
                architecture = catalog::ExecutableArchitecture::X86;
                break;
            case IMAGE_FILE_MACHINE_AMD64:
                architecture = catalog::ExecutableArchitecture::X64;
                break;
            case IMAGE_FILE_MACHINE_ARM64:
                architecture = catalog::ExecutableArchitecture::Arm64;
                break;
            default:
                error = "custom executable PE architecture is unsupported";
                return CustomExecutableSourceResult::InvalidExecutable;
            }
            const auto size = fs::file_size(executable, fsError);
            if (fsError || size == 0u) {
                error = "custom executable size could not be observed";
                return CustomExecutableSourceResult::Failure;
            }
            const auto writeTime = fs::last_write_time(executable, fsError);
            if (fsError) {
                error = "custom executable write time could not be observed";
                return CustomExecutableSourceResult::Failure;
            }
            CustomExecutableObservation candidate;
            candidate.canonicalExecutablePath = normalizedPath(executable);
            candidate.canonicalWorkingDirectory = normalizedPath(workingDirectory);
            candidate.canonicalIconSource = normalizedPath(icon);
            candidate.executableSize = size;
            candidate.executableWriteTime =
                static_cast<std::uint64_t>(writeTime.time_since_epoch().count());
            candidate.architecture = architecture;
            observation = std::move(candidate);
            return CustomExecutableSourceResult::Success;
        } catch (...) {
            error = "custom executable inspection failed";
            return CustomExecutableSourceResult::Failure;
        }
    }

    bool observeProcesses(const std::wstring& canonicalExecutablePath,
                          std::vector<ProviderProcessEvidence>& processes,
                          std::string& error) noexcept override {
        try {
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0u);
            if (snapshot == INVALID_HANDLE_VALUE) {
                error = "custom executable process snapshot failed";
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
                    DWORD size = static_cast<DWORD>(path.size());
                    if (!QueryFullProcessImageNameW(process, 0u, path.data(), &size)) continue;
                    path.resize(size);
                    if (lowerPath(path) != lowerPath(canonicalExecutablePath)) continue;
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
            processes = std::move(candidate);
            return true;
        } catch (...) {
            error = "custom executable process observation failed";
            return false;
        }
    }
};

#else

class NativeCustomExecutableSource final : public CustomExecutableSource {
public:
    CustomExecutableSourceResult inspect(const CustomExecutableDefinition&,
                                         CustomExecutableObservation&,
                                         std::string& error) noexcept override {
        error = "native custom executable inspection is available only on Windows";
        return CustomExecutableSourceResult::NotFound;
    }
    bool observeProcesses(const std::wstring&,
                          std::vector<ProviderProcessEvidence>&,
                          std::string& error) noexcept override {
        error = "native custom executable process observation is unavailable";
        return false;
    }
};

#endif

} // namespace

std::shared_ptr<CustomExecutableSource> makeNativeCustomExecutableSource() {
    return std::make_shared<NativeCustomExecutableSource>();
}

CustomExecutableProviderAdapter::CustomExecutableProviderAdapter(
    std::shared_ptr<CustomExecutableSource> source,
    CustomExecutableDefinition definition)
    : source_(std::move(source)), definition_(std::move(definition)) {
    (void)refresh();
}

ProviderDiagnostic CustomExecutableProviderAdapter::refresh() noexcept {
    try {
        descriptor_ = {};
        descriptor_.providerId = "custom";
        descriptor_.capabilities = {true, false, true, true, true};
        candidate_ = {};
        gameId_.clear();
        appId_.clear();
        const auto definitionResult = validateDefinition(definition_);
        if (!definitionResult.succeeded()) {
            descriptor_.availability = ProviderAvailability::Offline;
            descriptor_.metadataRevision = 1u;
            snapshotResult_ = definitionResult.result;
            snapshotDiagnostic_ = definitionResult.message;
            return definitionResult;
        }
        if (!source_) {
            descriptor_.availability = ProviderAvailability::Offline;
            descriptor_.metadataRevision = 1u;
            snapshotResult_ = ProviderResult::ProviderFailure;
            snapshotDiagnostic_ = "custom executable source is missing";
            return {snapshotResult_, snapshotDiagnostic_};
        }
        CustomExecutableObservation observation;
        std::string error;
        const auto sourceResult = source_->inspect(definition_, observation, error);
        if (sourceResult != CustomExecutableSourceResult::Success) {
            descriptor_.availability = sourceResult == CustomExecutableSourceResult::NotFound
                                           ? ProviderAvailability::Absent
                                           : ProviderAvailability::Offline;
            descriptor_.metadataRevision =
                descriptor_.availability == ProviderAvailability::Absent ? 0u : 1u;
            snapshotResult_ = sourceResult == CustomExecutableSourceResult::NotFound
                                  ? ProviderResult::ProviderAbsent
                                  : sourceResult == CustomExecutableSourceResult::InvalidExecutable
                                        ? ProviderResult::InvalidMetadata
                                        : ProviderResult::ProviderFailure;
            snapshotDiagnostic_ = error;
            return {snapshotResult_, snapshotDiagnostic_};
        }
        if (!absoluteWindowsPath(observation.canonicalExecutablePath) ||
            !absoluteWindowsPath(observation.canonicalWorkingDirectory) ||
            !absoluteWindowsPath(observation.canonicalIconSource) ||
            observation.executableSize == 0u || observation.executableWriteTime == 0u) {
            throw std::runtime_error("custom executable source returned malformed identity");
        }
        const auto pathKey = lowerPath(observation.canonicalExecutablePath);
        const std::uint64_t identityHash = hashWide(14695981039346656037ull, pathKey);
        appId_ = "exe:" + hex64(identityHash);
        std::uint64_t revision = hashWide(identityHash, definition_.title);
        revision = hashWide(revision, observation.canonicalWorkingDirectory);
        revision = hashWide(revision, observation.canonicalIconSource);
        for (const auto& argument : definition_.arguments) revision = hashWide(revision, argument);
        revision ^= observation.executableSize;
        revision *= 1099511628211ull;
        revision ^= observation.executableWriteTime;
        revision *= 1099511628211ull;
        if (revision == 0u) revision = 1u;

        catalog::GameCatalogCandidate candidate;
        candidate.providerId = "custom";
        candidate.providerAppId = appId_;
        candidate.title = definition_.title;
        candidate.installRoot = observation.canonicalWorkingDirectory;
        candidate.executableCandidates = {observation.canonicalExecutablePath};
        candidate.origin = profile::GameOrigin::Manual;
        candidate.localIconSource = observation.canonicalIconSource;
        candidate.architecture = observation.architecture;
        candidate.staleness = catalog::CatalogStaleness::Current;
        catalog::LocalGameCatalog catalog;
        const auto catalogResult = catalog::buildLocalGameCatalog(
            std::span<const catalog::GameCatalogCandidate>(&candidate, 1u), catalog);
        if (!catalogResult.succeeded() || catalog.entries.size() != 1u) {
            throw std::runtime_error("custom executable candidate failed catalog validation: " +
                                     catalogResult.message);
        }
        descriptor_.availability = ProviderAvailability::Available;
        descriptor_.metadataRevision = revision;
        descriptor_.capabilities = {true, false, true, true, true};
        observation_ = std::move(observation);
        candidate_ = std::move(candidate);
        gameId_ = catalog.entries.front().game.gameId;
        snapshotResult_ = ProviderResult::Success;
        snapshotDiagnostic_.clear();
        return {};
    } catch (const std::exception& exception) {
        descriptor_.providerId = "custom";
        descriptor_.availability = ProviderAvailability::Offline;
        descriptor_.metadataRevision = 1u;
        descriptor_.capabilities = {true, false, true, true, true};
        snapshotResult_ = ProviderResult::ProviderFailure;
        snapshotDiagnostic_ = exception.what();
        return {snapshotResult_, snapshotDiagnostic_};
    } catch (...) {
        descriptor_.providerId = "custom";
        descriptor_.availability = ProviderAvailability::Offline;
        descriptor_.metadataRevision = 1u;
        descriptor_.capabilities = {true, false, true, true, true};
        snapshotResult_ = ProviderResult::ProviderFailure;
        snapshotDiagnostic_ = "custom executable refresh failed";
        return {snapshotResult_, snapshotDiagnostic_};
    }
}

ProviderDescriptor CustomExecutableProviderAdapter::descriptor() const noexcept {
    return descriptor_;
}

DiscoveryResponse CustomExecutableProviderAdapter::discoverInstalledGames() noexcept {
    if (snapshotResult_ != ProviderResult::Success) {
        return {snapshotResult_, descriptor_.metadataRevision, {}, snapshotDiagnostic_};
    }
    try {
        return {ProviderResult::Success, descriptor_.metadataRevision, {candidate_}, {}};
    } catch (...) {
        return {ProviderResult::ProviderFailure, descriptor_.metadataRevision, {},
                "custom executable discovery allocation failed"};
    }
}

AccountReferenceResponse CustomExecutableProviderAdapter::listAccountReferences() noexcept {
    return {ProviderResult::UnsupportedOperation, descriptor_.metadataRevision, {},
            "custom executables do not expose provider accounts"};
}

LaunchResponse CustomExecutableProviderAdapter::buildLaunchRequest(
    const LaunchSelection& selection) noexcept {
    if (snapshotResult_ != ProviderResult::Success) {
        return {snapshotResult_, {}, snapshotDiagnostic_};
    }
    if (!selection.providerAppId || *selection.providerAppId != appId_ ||
        selection.gameId != gameId_ || selection.accountRef ||
        selection.instanceArguments != definition_.arguments) {
        return {ProviderResult::InvalidRequest, {},
                "custom executable launch selection does not exactly match the validated definition"};
    }
    try {
        ProviderLaunchRequest request;
        request.providerId = "custom";
        request.gameId = gameId_;
        request.providerAppId = appId_;
        request.metadataRevision = descriptor_.metadataRevision;
        request.targetKind = LaunchTargetKind::Executable;
        request.target = observation_.canonicalExecutablePath;
        request.arguments = definition_.arguments;
        request.workingDirectory = observation_.canonicalWorkingDirectory;
        request.launchCorrelationId = "custom-" + hex64(descriptor_.metadataRevision);
        return {ProviderResult::Success, std::move(request), {}};
    } catch (...) {
        return {ProviderResult::ProviderFailure, {},
                "custom executable launch request allocation failed"};
    }
}

ProcessIdentificationResponse CustomExecutableProviderAdapter::identifyProcesses(
    const ProcessIdentificationQuery& query) noexcept {
    ProcessIdentificationResponse response;
    response.result = snapshotResult_;
    response.metadataRevision = descriptor_.metadataRevision;
    response.diagnostic = snapshotDiagnostic_;
    if (snapshotResult_ != ProviderResult::Success) return response;
    if (!query.providerAppId || *query.providerAppId != appId_ || query.gameId != gameId_) {
        response.result = ProviderResult::InvalidRequest;
        response.diagnostic = "custom executable process query does not match the definition";
        return response;
    }
    std::string error;
    if (!source_->observeProcesses(observation_.canonicalExecutablePath,
                                   response.processes, error)) {
        response.result = ProviderResult::ProviderFailure;
        response.diagnostic = error;
        response.processes.clear();
        return response;
    }
    for (auto& process : response.processes) process.providerRelationshipVerified = false;
    response.result = ProviderResult::Success;
    return response;
}

std::string_view customExecutableSourceResultName(
    CustomExecutableSourceResult result) noexcept {
    switch (result) {
    case CustomExecutableSourceResult::Success: return "success";
    case CustomExecutableSourceResult::NotFound: return "not-found";
    case CustomExecutableSourceResult::InvalidExecutable: return "invalid-executable";
    case CustomExecutableSourceResult::Failure: return "failure";
    }
    return "unknown";
}

} // namespace hydra::provider::custom
