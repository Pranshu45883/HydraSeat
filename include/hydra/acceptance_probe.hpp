#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::acceptance {

inline constexpr std::uint32_t kAcceptanceProbeSchemaVersion = 1u;
inline constexpr std::size_t kMaximumInstalledStateBytes = 65536u;
inline constexpr std::size_t kMaximumProbeOwnedFiles = 32u;
inline constexpr std::size_t kMaximumProbeSamples = 3600u;

struct InstalledFileClaim {
    std::string fileName;
    std::string sha256;

    bool operator==(const InstalledFileClaim&) const = default;
};

struct InstalledReleaseClaim {
    std::uint32_t schemaVersion{1u};
    std::string releaseVersion;
    std::uint64_t releaseRevision{0u};
    std::string commitSha;
    std::string architecture;
    std::filesystem::path installRoot;
    std::string startupMode;
    std::vector<InstalledFileClaim> ownedFiles;

    bool operator==(const InstalledReleaseClaim&) const = default;
};

struct ProbeProcessRecord {
    std::uint32_t processId{0u};
    std::uint64_t creationTime100ns{0u};
    std::string fileName;
    std::string executableSha256;
    std::uint64_t firstWorkingSetBytes{0u};
    std::uint64_t workingSetP50Bytes{0u};
    std::uint64_t workingSetP95Bytes{0u};
    std::uint64_t workingSetP99Bytes{0u};
    std::uint64_t maximumWorkingSetBytes{0u};
    std::uint64_t lastWorkingSetBytes{0u};
    std::uint32_t maximumHandleCount{0u};
    std::uint32_t maximumThreadCount{0u};
    std::uint64_t cpuTimeDelta100ns{0u};
    std::size_t samples{0u};

    bool operator==(const ProbeProcessRecord&) const = default;
};

struct ProbeInventory {
    bool displayQuerySucceeded{false};
    bool audioQuerySucceeded{false};
    bool controllerQuerySucceeded{false};
    std::string displayFingerprintSha256;
    std::string audioFingerprintSha256;
    std::string controllerFingerprintSha256;
    std::uint32_t activeDisplayCount{0u};
    std::uint32_t activeRenderEndpointCount{0u};
    std::uint32_t connectedControllerCount{0u};

    bool operator==(const ProbeInventory&) const = default;
};

struct AcceptanceProbeReport {
    std::uint32_t schemaVersion{kAcceptanceProbeSchemaVersion};
    std::string releaseVersion;
    std::uint64_t releaseRevision{0u};
    std::string commitSha;
    std::string architecture;
    std::string installStateSha256;
    bool developmentUnsignedAllowed{false};
    bool allOwnedFilesVerified{false};
    std::vector<ProbeProcessRecord> runningOwnedProcesses;
    ProbeInventory inventory;

    bool operator==(const AcceptanceProbeReport&) const = default;
};

enum class ProbeCode : std::uint8_t {
    Success = 0,
    UnsupportedPlatform,
    InvalidArgument,
    StateReadFailed,
    StateDecodeFailed,
    UnsafeInstallRoot,
    OwnedFileMissing,
    OwnedFileHashMismatch,
    OwnedFileSignatureInvalid,
    ProcessObservationFailed,
    InventoryObservationFailed,
};

struct ProbeDiagnostic {
    ProbeCode code{ProbeCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == ProbeCode::Success; }
};

ProbeDiagnostic loadInstalledReleaseClaim(const std::filesystem::path& path,
                                          InstalledReleaseClaim& output,
                                          std::string* exactStateSha256 = nullptr);
ProbeDiagnostic runAcceptanceProbe(const InstalledReleaseClaim& claim,
                                   std::string_view installStateSha256,
                                   bool allowUnsignedDevelopment,
                                   std::size_t sampleCount,
                                   std::uint32_t sampleIntervalMilliseconds,
                                   AcceptanceProbeReport& output);
std::string encodeAcceptanceProbeJson(const AcceptanceProbeReport& report);
std::string sha256FileHex(const std::filesystem::path& path);
std::string_view probeCodeName(ProbeCode value) noexcept;

} // namespace hydra::acceptance
