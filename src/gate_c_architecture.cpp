#include "hydra/gate_c_architecture.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>
#include <tuple>

namespace hydra::gatec {
namespace {

constexpr std::uint16_t kImageFileMachineUnknown = 0x0000;
constexpr std::uint16_t kImageFileMachineI386 = 0x014c;
constexpr std::uint16_t kImageFileMachineAmd64 = 0x8664;
constexpr std::uint16_t kProcessorArchitectureIntel = 0;
constexpr std::uint16_t kProcessorArchitectureAmd64 = 9;

ArchitectureDetectionResult success(ProcessArchitecture architecture,
                                    bool usedFallback = false) {
    ArchitectureDetectionResult result;
    result.status = ArchitectureDetectionStatus::Success;
    result.architecture = architecture;
    result.usedLegacyFallback = usedFallback;
    return result;
}

ArchitectureDetectionResult failure(ArchitectureDetectionStatus status,
                                    std::string error,
                                    std::uint32_t systemError = 0,
                                    bool usedFallback = false) {
    ArchitectureDetectionResult result;
    result.status = status;
    result.usedLegacyFallback = usedFallback;
    result.systemError = systemError;
    result.error = std::move(error);
    return result;
}

std::optional<ProcessArchitecture> architectureFromMachine(
    std::uint16_t machine) noexcept {
    if (machine == kImageFileMachineI386) return ProcessArchitecture::X86;
    if (machine == kImageFileMachineAmd64) return ProcessArchitecture::X64;
    return std::nullopt;
}

bool knownArtifactKind(GateCArtifactKind kind) noexcept {
    return kind == GateCArtifactKind::ControlledTarget ||
           kind == GateCArtifactKind::AdapterLibrary ||
           kind == GateCArtifactKind::ApiProbe ||
           kind == GateCArtifactKind::PollingShim;
}

bool safeRelativeArtifactPath(std::string_view path) noexcept {
    if (path.empty() || path.size() > kMaximumGateCArtifactPathBytes ||
        path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t segmentStart = 0;
    while (segmentStart < path.size()) {
        const auto separator = path.find('/', segmentStart);
        const auto segmentEnd = separator == std::string_view::npos
                                    ? path.size()
                                    : separator;
        const auto segment = path.substr(segmentStart,
                                         segmentEnd - segmentStart);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        for (const char rawCharacter : segment) {
            const auto character = static_cast<unsigned char>(rawCharacter);
            const bool alphaNumeric =
                (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9');
            if (!alphaNumeric && character != '_' && character != '-' &&
                character != '.') {
                return false;
            }
        }
        if (separator == std::string_view::npos) break;
        segmentStart = separator + 1;
    }
    return true;
}

std::optional<std::string> validateManifest(
    const GateCArtifactManifest& manifest) {
    if (manifest.schemaVersion != kGateCArtifactManifestVersion) {
        return "unsupported Gate C artifact manifest version";
    }
    if (manifest.entries.empty() ||
        manifest.entries.size() > kMaximumGateCArtifactEntries) {
        return "Gate C artifact manifest entry count is invalid";
    }

    std::set<std::pair<std::uint16_t, std::uint16_t>> identities;
    std::set<std::string> paths;
    std::optional<std::pair<std::uint16_t, std::uint16_t>> previous;
    for (const auto& entry : manifest.entries) {
        if (entry.architecture != ProcessArchitecture::X86 &&
            entry.architecture != ProcessArchitecture::X64) {
            return "Gate C artifact manifest contains an unknown architecture";
        }
        if (!knownArtifactKind(entry.kind)) {
            return "Gate C artifact manifest contains an unknown artifact kind";
        }
        if (!safeRelativeArtifactPath(entry.relativePath)) {
            return "Gate C artifact manifest contains an unsafe relative path";
        }
        const auto identity = std::pair{
            static_cast<std::uint16_t>(entry.architecture),
            static_cast<std::uint16_t>(entry.kind)};
        if (previous && identity <= *previous) {
            return "Gate C artifact manifest order is not canonical";
        }
        previous = identity;
        if (!identities.insert(identity).second ||
            !paths.insert(entry.relativePath).second) {
            return "Gate C artifact manifest contains a duplicate entry";
        }
    }
    return std::nullopt;
}

GateCArtifactSelectionResult selectionFailure(
    ArtifactSelectionStatus status, std::string error) {
    GateCArtifactSelectionResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

std::uint16_t readU16(std::span<const std::byte> bytes,
                      std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8;
}

std::uint32_t readU32(std::span<const std::byte> bytes,
                      std::size_t offset) noexcept {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        result |= static_cast<std::uint32_t>(
                      std::to_integer<std::uint8_t>(
                          bytes[offset + shift / 8])) << shift;
    }
    return result;
}

} // namespace

ArchitectureDetectionResult resolveProcessArchitecture(
    const ProcessArchitectureObservation& observation) noexcept {
    if (observation.modernApiAvailable) {
        if (!observation.modernApiSucceeded) {
            return failure(ArchitectureDetectionStatus::SystemError,
                           "IsWow64Process2 failed", observation.systemError);
        }
        const auto machine =
            observation.processMachine != kImageFileMachineUnknown
                ? observation.processMachine
                : observation.nativeMachine;
        if (const auto architecture = architectureFromMachine(machine)) {
            return success(*architecture);
        }
        return failure(ArchitectureDetectionStatus::Unsupported,
                       "IsWow64Process2 reported an unsupported architecture");
    }

    if (!observation.legacyApiSucceeded) {
        return failure(ArchitectureDetectionStatus::SystemError,
                       "IsWow64Process fallback failed",
                       observation.systemError, true);
    }
    if (observation.legacyWow64) {
        return success(ProcessArchitecture::X86, true);
    }
    if (observation.nativeProcessorArchitecture ==
        kProcessorArchitectureIntel) {
        return success(ProcessArchitecture::X86, true);
    }
    if (observation.nativeProcessorArchitecture ==
        kProcessorArchitectureAmd64) {
        return success(ProcessArchitecture::X64, true);
    }
    return failure(ArchitectureDetectionStatus::Unsupported,
                   "legacy fallback reported an unsupported architecture",
                   0, true);
}

#ifdef _WIN32
ArchitectureDetectionResult detectProcessArchitecture(HANDLE process) noexcept {
    if (process == nullptr) {
        return failure(ArchitectureDetectionStatus::SystemError,
                       "process handle is null", ERROR_INVALID_HANDLE);
    }

    // GetCurrentProcess() is a valid pseudo-handle whose value is -1, the
    // same bit pattern used by INVALID_HANDLE_VALUE. Process APIs accept that
    // pseudo-handle, so rejecting INVALID_HANDLE_VALUE here would incorrectly
    // reject every current-process architecture query.

    ProcessArchitectureObservation observation;
    using IsWow64Process2Function = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto modern = kernel32 == nullptr
        ? nullptr
        : reinterpret_cast<IsWow64Process2Function>(
              GetProcAddress(kernel32, "IsWow64Process2"));
    observation.modernApiAvailable = modern != nullptr;
    if (modern != nullptr) {
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;
        observation.modernApiSucceeded =
            modern(process, &processMachine, &nativeMachine) != FALSE;
        observation.processMachine = processMachine;
        observation.nativeMachine = nativeMachine;
        if (!observation.modernApiSucceeded) {
            observation.systemError = GetLastError();
            return resolveProcessArchitecture(observation);
        }

        auto resolved = resolveProcessArchitecture(observation);
        if (resolved ||
            resolved.status != ArchitectureDetectionStatus::Unsupported) {
            return resolved;
        }

        // Modern Windows can report a host-relative machine value such as
        // IMAGE_FILE_MACHINE_UNKNOWN or IMAGE_FILE_MACHINE_TARGET_HOST rather
        // than the actual x86/x64 executable machine. When the API call itself
        // succeeded but its machine values cannot identify a supported target,
        // inspect the process image PE header before declaring it unsupported.
        std::array<wchar_t, 32768> imagePath{};
        DWORD imagePathLength = static_cast<DWORD>(imagePath.size());
        if (QueryFullProcessImageNameW(
                process, 0, imagePath.data(), &imagePathLength) == FALSE) {
            return failure(ArchitectureDetectionStatus::SystemError,
                           "process image path query failed", GetLastError());
        }
        const auto imageArchitecture = detectPortableExecutableArchitecture(
            std::filesystem::path(
                std::wstring_view(imagePath.data(), imagePathLength)));
        if (!imageArchitecture) {
            return failure(
                imageArchitecture.status,
                "process image architecture detection failed: " +
                    imageArchitecture.error,
                imageArchitecture.systemError);
        }
        return imageArchitecture;
    }

    BOOL wow64 = FALSE;
    observation.legacyApiSucceeded =
        IsWow64Process(process, &wow64) != FALSE;
    observation.legacyWow64 = wow64 != FALSE;
    if (!observation.legacyApiSucceeded) {
        observation.systemError = GetLastError();
        return resolveProcessArchitecture(observation);
    }
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    observation.nativeProcessorArchitecture =
        systemInfo.wProcessorArchitecture;
    return resolveProcessArchitecture(observation);
}
#endif

ArchitectureDetectionResult detectPortableExecutableArchitecture(
    std::span<const std::byte> headerBytes) noexcept {
    constexpr std::size_t kDosPeOffsetField = 0x3c;
    constexpr std::size_t kPeMachineOffset = 4;
    constexpr std::size_t kRequiredMachineBytes = 2;
    if (headerBytes.size() < kDosPeOffsetField + 4 ||
        headerBytes[0] != std::byte{'M'} ||
        headerBytes[1] != std::byte{'Z'}) {
        return failure(ArchitectureDetectionStatus::MalformedImage,
                       "portable executable DOS header is invalid");
    }
    const auto peOffset = static_cast<std::size_t>(
        readU32(headerBytes, kDosPeOffsetField));
    if (peOffset > kMaximumPortableExecutableHeaderBytes ||
        peOffset > headerBytes.size() ||
        headerBytes.size() - peOffset <
            kPeMachineOffset + kRequiredMachineBytes) {
        return failure(ArchitectureDetectionStatus::MalformedImage,
                       "portable executable header offset is invalid");
    }
    if (headerBytes[peOffset] != std::byte{'P'} ||
        headerBytes[peOffset + 1] != std::byte{'E'} ||
        headerBytes[peOffset + 2] != std::byte{0} ||
        headerBytes[peOffset + 3] != std::byte{0}) {
        return failure(ArchitectureDetectionStatus::MalformedImage,
                       "portable executable signature is invalid");
    }
    const auto machine = readU16(headerBytes, peOffset + kPeMachineOffset);
    if (const auto architecture = architectureFromMachine(machine)) {
        return success(*architecture);
    }
    return failure(ArchitectureDetectionStatus::Unsupported,
                   "portable executable machine is unsupported");
}

ArchitectureDetectionResult detectPortableExecutableArchitecture(
    const std::filesystem::path& path) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return failure(ArchitectureDetectionStatus::SystemError,
                           "portable executable could not be opened");
        }
        std::array<std::byte,
                   kMaximumPortableExecutableHeaderBytes + 6> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        const auto count = input.gcount();
        if (count <= 0) {
            return failure(ArchitectureDetectionStatus::MalformedImage,
                           "portable executable is empty");
        }
        return detectPortableExecutableArchitecture(
            std::span<const std::byte>(
                bytes.data(), static_cast<std::size_t>(count)));
    } catch (...) {
        return failure(ArchitectureDetectionStatus::SystemError,
                       "portable executable read failed");
    }
}

GateCArtifactManifest defaultGateCArtifactManifest() {
    GateCArtifactManifest manifest;
    manifest.entries = {
        {ProcessArchitecture::X86, GateCArtifactKind::ControlledTarget,
         "x86/hydra_gate_c_target.exe"},
        {ProcessArchitecture::X86, GateCArtifactKind::AdapterLibrary,
         "x86/hydra_gate_c_adapter.dll"},
        {ProcessArchitecture::X86, GateCArtifactKind::ApiProbe,
         "x86/hydra_gate_c_api_probe.exe"},
        {ProcessArchitecture::X86, GateCArtifactKind::PollingShim,
         "x86/hydra_gate_c_shim.dll"},
        {ProcessArchitecture::X64, GateCArtifactKind::ControlledTarget,
         "x64/hydra_gate_c_target.exe"},
        {ProcessArchitecture::X64, GateCArtifactKind::AdapterLibrary,
         "x64/hydra_gate_c_adapter.dll"},
        {ProcessArchitecture::X64, GateCArtifactKind::ApiProbe,
         "x64/hydra_gate_c_api_probe.exe"},
        {ProcessArchitecture::X64, GateCArtifactKind::PollingShim,
         "x64/hydra_gate_c_shim.dll"},
    };
    return manifest;
}

GateCArtifactSelectionResult selectGateCArtifacts(
    const GateCArtifactManifest& manifest,
    ProcessArchitecture architecture,
    GateCArtifactKind executableKind) {
    if (const auto error = validateManifest(manifest)) {
        return selectionFailure(ArtifactSelectionStatus::InvalidManifest,
                                *error);
    }
    if (architecture != ProcessArchitecture::X86 &&
        architecture != ProcessArchitecture::X64) {
        return selectionFailure(ArtifactSelectionStatus::Unsupported,
                                "requested process architecture is unsupported");
    }
    if (executableKind != GateCArtifactKind::ControlledTarget &&
        executableKind != GateCArtifactKind::ApiProbe) {
        return selectionFailure(ArtifactSelectionStatus::InvalidManifest,
                                "requested executable artifact kind is invalid");
    }

    const GateCArtifactEntry* executable = nullptr;
    const GateCArtifactEntry* adapter = nullptr;
    const GateCArtifactEntry* shim = nullptr;
    bool architecturePresent = false;
    for (const auto& entry : manifest.entries) {
        if (entry.architecture != architecture) continue;
        architecturePresent = true;
        if (entry.kind == executableKind) executable = &entry;
        if (entry.kind == GateCArtifactKind::AdapterLibrary) adapter = &entry;
        if (entry.kind == GateCArtifactKind::PollingShim) shim = &entry;
    }
    if (!architecturePresent) {
        return selectionFailure(ArtifactSelectionStatus::Unsupported,
                                "requested architecture is not in the manifest");
    }
    if (executable == nullptr || adapter == nullptr ||
        (executableKind == GateCArtifactKind::ApiProbe && shim == nullptr)) {
        return selectionFailure(ArtifactSelectionStatus::MissingArtifact,
                                "manifest lacks the requested executable, adapter, or shim");
    }

    GateCArtifactSelectionResult result;
    result.status = ArtifactSelectionStatus::Success;
    result.selection = GateCArtifactSelection{
        architecture, executableKind,
        std::filesystem::path(executable->relativePath),
        std::filesystem::path(adapter->relativePath),
        executableKind != GateCArtifactKind::ApiProbe || shim == nullptr
            ? std::filesystem::path{}
            : std::filesystem::path(shim->relativePath)};
    return result;
}

GateCArtifactSelectionResult resolveGateCArtifacts(
    const std::filesystem::path& artifactRoot,
    const GateCArtifactManifest& manifest,
    ProcessArchitecture architecture,
    GateCArtifactKind executableKind) {
    auto result = selectGateCArtifacts(
        manifest, architecture, executableKind);
    if (!result) return result;

    try {
        std::error_code errorCode;
        auto root = std::filesystem::absolute(artifactRoot, errorCode);
        if (errorCode) {
            return selectionFailure(ArtifactSelectionStatus::IoError,
                                    "artifact root could not be resolved");
        }
        root = root.lexically_normal();
        auto executable =
            (root / result.selection->executablePath).lexically_normal();
        auto adapter =
            (root / result.selection->adapterPath).lexically_normal();
        auto shim = result.selection->shimPath.empty()
                        ? std::filesystem::path{}
                        : (root / result.selection->shimPath).lexically_normal();
        if (!std::filesystem::is_regular_file(executable, errorCode) ||
            errorCode) {
            return selectionFailure(ArtifactSelectionStatus::MissingArtifact,
                                    "selected executable artifact is missing");
        }
        errorCode.clear();
        if (!std::filesystem::is_regular_file(adapter, errorCode) ||
            errorCode) {
            return selectionFailure(ArtifactSelectionStatus::MissingArtifact,
                                    "selected adapter artifact is missing");
        }
        errorCode.clear();
        if (!shim.empty() &&
            (!std::filesystem::is_regular_file(shim, errorCode) ||
             errorCode)) {
            return selectionFailure(ArtifactSelectionStatus::MissingArtifact,
                                    "selected shim artifact is missing");
        }

        const auto executableArchitecture =
            detectPortableExecutableArchitecture(executable);
        const auto adapterArchitecture =
            detectPortableExecutableArchitecture(adapter);
        const auto shimArchitecture = shim.empty()
            ? success(architecture)
            : detectPortableExecutableArchitecture(shim);
        const auto mapFailure = [](const ArchitectureDetectionResult& value) {
            return value.status == ArchitectureDetectionStatus::MalformedImage
                       ? ArtifactSelectionStatus::MalformedImage
                       : ArtifactSelectionStatus::IoError;
        };
        if (!executableArchitecture) {
            return selectionFailure(mapFailure(executableArchitecture),
                                    "selected executable is not a supported PE: " +
                                        executableArchitecture.error);
        }
        if (!adapterArchitecture) {
            return selectionFailure(mapFailure(adapterArchitecture),
                                    "selected adapter is not a supported PE: " +
                                        adapterArchitecture.error);
        }
        if (!shimArchitecture) {
            return selectionFailure(mapFailure(shimArchitecture),
                                    "selected shim is not a supported PE: " +
                                        shimArchitecture.error);
        }
        if (executableArchitecture.architecture != architecture ||
            adapterArchitecture.architecture != architecture ||
            shimArchitecture.architecture != architecture) {
            return selectionFailure(
                ArtifactSelectionStatus::ArchitectureMismatch,
                "selected executable, adapter, or shim architecture does not match the manifest");
        }

        result.selection->executablePath = std::move(executable);
        result.selection->adapterPath = std::move(adapter);
        result.selection->shimPath = std::move(shim);
        return result;
    } catch (...) {
        return selectionFailure(ArtifactSelectionStatus::IoError,
                                "artifact selection failed while reading files");
    }
}

std::optional<ProcessArchitecture> parseProcessArchitecture(
    std::string_view text) noexcept {
    if (text == "x86" || text == "X86" || text == "32") {
        return ProcessArchitecture::X86;
    }
    if (text == "x64" || text == "X64" || text == "64") {
        return ProcessArchitecture::X64;
    }
    return std::nullopt;
}

std::string_view processArchitectureName(ProcessArchitecture value) noexcept {
    switch (value) {
    case ProcessArchitecture::X86: return "x86";
    case ProcessArchitecture::X64: return "x64";
    case ProcessArchitecture::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view architectureDetectionStatusName(
    ArchitectureDetectionStatus value) noexcept {
    switch (value) {
    case ArchitectureDetectionStatus::Success: return "Success";
    case ArchitectureDetectionStatus::Unsupported: return "Unsupported";
    case ArchitectureDetectionStatus::SystemError: return "SystemError";
    case ArchitectureDetectionStatus::MalformedImage: return "MalformedImage";
    }
    return "Unknown";
}

std::string_view artifactSelectionStatusName(
    ArtifactSelectionStatus value) noexcept {
    switch (value) {
    case ArtifactSelectionStatus::Success: return "Success";
    case ArtifactSelectionStatus::InvalidManifest: return "InvalidManifest";
    case ArtifactSelectionStatus::Unsupported: return "Unsupported";
    case ArtifactSelectionStatus::MissingArtifact: return "MissingArtifact";
    case ArtifactSelectionStatus::MalformedImage: return "MalformedImage";
    case ArtifactSelectionStatus::ArchitectureMismatch:
        return "ArchitectureMismatch";
    case ArtifactSelectionStatus::IoError: return "IoError";
    }
    return "Unknown";
}

} // namespace hydra::gatec
