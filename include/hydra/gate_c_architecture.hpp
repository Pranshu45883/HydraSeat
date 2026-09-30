#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hydra::gatec {

inline constexpr std::uint16_t kGateCArtifactManifestVersion = 1;
inline constexpr std::size_t kMaximumGateCArtifactEntries = 8;
inline constexpr std::size_t kMaximumGateCArtifactPathBytes = 128;
inline constexpr std::size_t kMaximumPortableExecutableHeaderBytes = 4096;

enum class ProcessArchitecture : std::uint16_t {
    Unknown = 0,
    X86 = 32,
    X64 = 64
};

enum class ArchitectureDetectionStatus : std::uint8_t {
    Success,
    Unsupported,
    SystemError,
    MalformedImage
};

struct ProcessArchitectureObservation {
    bool modernApiAvailable{false};
    bool modernApiSucceeded{false};
    std::uint16_t processMachine{0};
    std::uint16_t nativeMachine{0};
    bool legacyApiSucceeded{false};
    bool legacyWow64{false};
    std::uint16_t nativeProcessorArchitecture{0xffffu};
    std::uint32_t systemError{0};
};

struct ArchitectureDetectionResult {
    ArchitectureDetectionStatus status{
        ArchitectureDetectionStatus::Unsupported};
    ProcessArchitecture architecture{ProcessArchitecture::Unknown};
    bool usedLegacyFallback{false};
    std::uint32_t systemError{0};
    std::string error;

    explicit operator bool() const noexcept {
        return status == ArchitectureDetectionStatus::Success;
    }
};

enum class GateCArtifactKind : std::uint16_t {
    ControlledTarget = 1,
    AdapterLibrary = 2,
    ApiProbe = 3,
    PollingShim = 4
};

struct GateCArtifactEntry {
    ProcessArchitecture architecture{ProcessArchitecture::Unknown};
    GateCArtifactKind kind{GateCArtifactKind::ControlledTarget};
    std::string relativePath;

    bool operator==(const GateCArtifactEntry&) const = default;
};

struct GateCArtifactManifest {
    std::uint16_t schemaVersion{kGateCArtifactManifestVersion};
    std::vector<GateCArtifactEntry> entries;
};

enum class ArtifactSelectionStatus : std::uint8_t {
    Success,
    InvalidManifest,
    Unsupported,
    MissingArtifact,
    MalformedImage,
    ArchitectureMismatch,
    IoError
};

struct GateCArtifactSelection {
    ProcessArchitecture architecture{ProcessArchitecture::Unknown};
    GateCArtifactKind executableKind{GateCArtifactKind::ControlledTarget};
    std::filesystem::path executablePath;
    std::filesystem::path adapterPath;
    std::filesystem::path shimPath;
};

struct GateCArtifactSelectionResult {
    ArtifactSelectionStatus status{ArtifactSelectionStatus::InvalidManifest};
    std::optional<GateCArtifactSelection> selection;
    std::string error;

    explicit operator bool() const noexcept {
        return status == ArtifactSelectionStatus::Success &&
               selection.has_value();
    }
};

ArchitectureDetectionResult resolveProcessArchitecture(
    const ProcessArchitectureObservation& observation) noexcept;

#ifdef _WIN32
ArchitectureDetectionResult detectProcessArchitecture(HANDLE process) noexcept;
#endif

ArchitectureDetectionResult detectPortableExecutableArchitecture(
    std::span<const std::byte> headerBytes) noexcept;
ArchitectureDetectionResult detectPortableExecutableArchitecture(
    const std::filesystem::path& path) noexcept;

GateCArtifactManifest defaultGateCArtifactManifest();
GateCArtifactSelectionResult selectGateCArtifacts(
    const GateCArtifactManifest& manifest,
    ProcessArchitecture architecture,
    GateCArtifactKind executableKind);
GateCArtifactSelectionResult resolveGateCArtifacts(
    const std::filesystem::path& artifactRoot,
    const GateCArtifactManifest& manifest,
    ProcessArchitecture architecture,
    GateCArtifactKind executableKind);

std::optional<ProcessArchitecture> parseProcessArchitecture(
    std::string_view text) noexcept;
std::string_view processArchitectureName(ProcessArchitecture value) noexcept;
std::string_view architectureDetectionStatusName(
    ArchitectureDetectionStatus value) noexcept;
std::string_view artifactSelectionStatusName(
    ArtifactSelectionStatus value) noexcept;

} // namespace hydra::gatec
