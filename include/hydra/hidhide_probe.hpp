#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra {

enum class HidHideAvailability {
    Unavailable,
    InstalledUnverified,
    VerifiedSupported
};

enum class HidHideProbeDiagnostic {
    None,
    UnsupportedPlatform,
    NotDetected,
    InstallationQueryFailed,
    ControlInterfaceAbsent,
    InterfaceQueryFailed,
    AccessDenied,
    VersionUnavailable,
    UnsupportedVersion,
    ControlOpenFailed,
    ActiveQueryFailed,
    InverseQueryFailed,
    MalformedResponse,
    ResponseTooLarge
};

struct HidHideVersion {
    std::uint16_t major{0};
    std::uint16_t minor{0};
    std::uint16_t revision{0};
    std::uint16_t build{0};

    bool operator==(const HidHideVersion&) const = default;
};

struct HidHideProbeReport {
    HidHideAvailability availability{HidHideAvailability::Unavailable};
    bool installedEvidence{false};
    bool controlInterfacePresent{false};
    bool controlInterfaceReadable{false};
    std::optional<HidHideVersion> driverVersion;
    std::optional<bool> active;
    std::optional<bool> inverseWhitelist;
    bool sessionBlacklistSupported{false};
    HidHideProbeDiagnostic diagnostic{HidHideProbeDiagnostic::NotDetected};
    std::uint32_t systemError{0};

    bool operator==(const HidHideProbeReport&) const = default;
};

enum class HidHidePlatformStatus {
    Success,
    NotFound,
    AccessDenied,
    Failed,
    Malformed,
    ResponseTooLarge,
    UnsupportedPlatform
};

struct HidHideEvidenceResult {
    HidHidePlatformStatus status{HidHidePlatformStatus::NotFound};
    std::uint32_t systemError{0};
};

struct HidHideVersionResult {
    HidHidePlatformStatus status{HidHidePlatformStatus::NotFound};
    std::optional<HidHideVersion> version;
    std::uint32_t systemError{0};
};

struct HidHideBooleanQueryResult {
    HidHidePlatformStatus status{HidHidePlatformStatus::Failed};
    std::vector<std::uint8_t> response;
    std::uint32_t systemError{0};
};

struct HidHideControlReadResult {
    HidHidePlatformStatus openStatus{HidHidePlatformStatus::NotFound};
    HidHideBooleanQueryResult active;
    HidHideBooleanQueryResult inverseWhitelist;
    std::uint32_t systemError{0};
};

// This interface intentionally exposes no state-changing operation. Instances
// are caller-owned, used synchronously, and need not be thread-safe.
class HidHideProbePlatform {
public:
    virtual ~HidHideProbePlatform() = default;

    virtual HidHideEvidenceResult installationEvidence() = 0;
    virtual HidHideEvidenceResult controlInterfaceEvidence() = 0;
    virtual HidHideVersionResult driverVersion() = 0;
    virtual HidHideControlReadResult queryControlStateReadOnly() = 0;
};

inline constexpr std::size_t kHidHideMaxControlResponseBytes = 16;

bool isKnownSupportedHidHideVersion(const HidHideVersion& version) noexcept;
HidHideProbeReport probeHidHide(HidHideProbePlatform& platform);
HidHideProbeReport probeHidHide();

std::string_view hidHideAvailabilityName(HidHideAvailability value) noexcept;
std::string_view hidHideProbeDiagnosticName(
    HidHideProbeDiagnostic value) noexcept;
std::string formatHidHideVersion(const HidHideVersion& version);
std::string formatHidHideProbeReport(const HidHideProbeReport& report);

} // namespace hydra
