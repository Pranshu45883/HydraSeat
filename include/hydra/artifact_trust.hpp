#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::trust {

inline constexpr std::uint32_t kArtifactTrustManifestVersion = 1u;
inline constexpr std::size_t kMaximumArtifactCapabilities = 32u;
inline constexpr std::size_t kMaximumTrustedPublisherIdentities = 8u;
inline constexpr std::size_t kMaximumPublisherIdentityBytes = 128u;

enum class ArtifactClass : std::uint8_t {
    DataCatalog = 0,
    SetupPackage = 1,
    Executable = 2,
    Driver = 3,
    ProviderHelper = 4,
};

enum class ArtifactArchitecture : std::uint8_t {
    Any = 0,
    X86 = 1,
    X64 = 2,
    Arm64 = 3,
};

enum class SignatureState : std::uint8_t {
    NotApplicable = 0,
    Missing = 1,
    ValidTrustedPublisher = 2,
    Invalid = 3,
    UnknownPublisher = 4,
};

struct ArtifactManifest {
    std::uint32_t schemaVersion{kArtifactTrustManifestVersion};
    std::string artifactId;
    ArtifactClass artifactClass{ArtifactClass::DataCatalog};
    std::string artifactVersion;
    ArtifactArchitecture architecture{ArtifactArchitecture::Any};
    std::string expectedSha256;
    std::string sourceId;
    std::string licenseId;
    bool redistributionAllowed{false};
    bool optional{true};
    bool developmentBuild{false};
    bool requiresInstall{false};
    bool requiresRestart{false};
    bool requiresRecoveryPlan{false};
    std::vector<std::string> capabilityScope;

    bool operator==(const ArtifactManifest&) const = default;
};

struct ArtifactObservation {
    bool present{false};
    std::string artifactVersion;
    ArtifactArchitecture architecture{ArtifactArchitecture::Any};
    std::string observedSha256;
    SignatureState signature{SignatureState::NotApplicable};
    // Canonical publisher/signing identity observed by the native verifier.
    // For the current release pipeline this is the exact selected certificate
    // identity, not a display name and not a caller assertion.
    std::string publisherIdentity;

    bool operator==(const ArtifactObservation&) const = default;
};

struct TrustPolicy {
    ArtifactArchitecture hostArchitecture{ArtifactArchitecture::X64};
    bool allowUnsignedDevelopmentExecutables{false};
    bool requireRedistributionPermission{false};
    std::vector<std::string> allowedSourceIds;
    std::vector<std::string> allowedCapabilities;
    // Exact canonical signing identities accepted by policy. Signature validity
    // alone never authorizes executable/helper/driver code.
    std::vector<std::string> trustedPublisherIdentities;
};

enum class TrustDecision : std::uint8_t {
    Accept = 0,
    OptionalAbsent = 1,
    Reject = 2,
};

enum class TrustCode : std::uint8_t {
    Success = 0,
    OptionalArtifactAbsent,
    RequiredArtifactMissing,
    InvalidManifest,
    UnsupportedManifestVersion,
    InvalidHash,
    HashMismatch,
    VersionMismatch,
    ArchitectureMismatch,
    SignatureRequired,
    SignatureInvalid,
    SignatureMetadataMalformed,
    PublisherIdentityRequired,
    PublisherIdentityMismatch,
    SourceNotAllowed,
    CapabilityNotAllowed,
    LicenseMissing,
    RedistributionNotAllowed,
    RecoveryPlanRequired,
};

struct TrustEvaluation {
    TrustDecision decision{TrustDecision::Reject};
    TrustCode code{TrustCode::InvalidManifest};
    std::string message;

    bool accepted() const noexcept {
        return decision == TrustDecision::Accept || decision == TrustDecision::OptionalAbsent;
    }
};

TrustEvaluation evaluateArtifact(const ArtifactManifest& manifest,
                                 const ArtifactObservation& observation,
                                 const TrustPolicy& policy);

bool artifactClassCanExecute(ArtifactClass value) noexcept;
std::string_view artifactClassName(ArtifactClass value) noexcept;
std::string_view artifactArchitectureName(ArtifactArchitecture value) noexcept;
std::string_view signatureStateName(SignatureState value) noexcept;
std::string_view trustCodeName(TrustCode value) noexcept;

} // namespace hydra::trust
