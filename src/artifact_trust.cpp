#include "hydra/artifact_trust.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace hydra::trust {
namespace {

TrustEvaluation reject(TrustCode code, std::string message) {
    return {TrustDecision::Reject, code, std::move(message)};
}

bool validEnum(ArtifactClass value) noexcept {
    return value == ArtifactClass::DataCatalog || value == ArtifactClass::SetupPackage ||
           value == ArtifactClass::Executable || value == ArtifactClass::Driver ||
           value == ArtifactClass::ProviderHelper;
}

bool validArchitecture(ArtifactArchitecture value) noexcept {
    return value == ArtifactArchitecture::Any || value == ArtifactArchitecture::X86 ||
           value == ArtifactArchitecture::X64 || value == ArtifactArchitecture::Arm64;
}

bool validSignature(SignatureState value) noexcept {
    return value == SignatureState::NotApplicable || value == SignatureState::Missing ||
           value == SignatureState::ValidTrustedPublisher || value == SignatureState::Invalid ||
           value == SignatureState::UnknownPublisher;
}

bool validId(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128u) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
              ch == ':' || ch == '@' || ch == '+')) {
            return false;
        }
    }
    return true;
}

bool validSha256(std::string_view value) noexcept {
    if (value.size() != 64u) return false;
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

bool contains(const std::vector<std::string>& values, std::string_view expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

bool validPublisherPolicy(const TrustPolicy& policy) {
    if (policy.trustedPublisherIdentities.size() > kMaximumTrustedPublisherIdentities) return false;
    std::set<std::string> seen;
    for (const auto& identity : policy.trustedPublisherIdentities) {
        if (!validId(identity) || identity.size() > kMaximumPublisherIdentityBytes ||
            !seen.insert(identity).second) {
            return false;
        }
    }
    return true;
}

bool isDataOnly(ArtifactClass value) noexcept {
    return value == ArtifactClass::DataCatalog || value == ArtifactClass::SetupPackage;
}

} // namespace

TrustEvaluation evaluateArtifact(const ArtifactManifest& manifest,
                                 const ArtifactObservation& observation,
                                 const TrustPolicy& policy) {
    if (manifest.schemaVersion != kArtifactTrustManifestVersion) {
        return reject(TrustCode::UnsupportedManifestVersion,
                      "unsupported artifact trust manifest version");
    }
    if (!validEnum(manifest.artifactClass) || !validArchitecture(manifest.architecture) ||
        !validArchitecture(policy.hostArchitecture) || !validId(manifest.artifactId) ||
        !validId(manifest.artifactVersion) || !validId(manifest.sourceId) ||
        !validId(manifest.licenseId) || !validSha256(manifest.expectedSha256) ||
        manifest.capabilityScope.size() > kMaximumArtifactCapabilities ||
        !validPublisherPolicy(policy)) {
        return reject(TrustCode::InvalidManifest,
                      "artifact manifest or trust policy contains invalid bounded identity/version/hash fields");
    }
    if (artifactClassCanExecute(manifest.artifactClass) &&
        manifest.architecture == ArtifactArchitecture::Any) {
        return reject(TrustCode::InvalidManifest,
                      "executable artifact must declare an exact architecture");
    }
    if (isDataOnly(manifest.artifactClass) &&
        (manifest.requiresInstall || manifest.requiresRestart || manifest.requiresRecoveryPlan ||
         manifest.developmentBuild)) {
        return reject(TrustCode::InvalidManifest,
                      "data-only artifact cannot acquire install/restart/executable trust flags");
    }

    std::set<std::string> capabilities;
    for (const auto& capability : manifest.capabilityScope) {
        if (!validId(capability) || !capabilities.insert(capability).second) {
            return reject(TrustCode::InvalidManifest,
                          "artifact capability scope is malformed or duplicated");
        }
        if (!contains(policy.allowedCapabilities, capability)) {
            return reject(TrustCode::CapabilityNotAllowed,
                          "artifact requests a capability outside the selected trust policy");
        }
    }
    if (!contains(policy.allowedSourceIds, manifest.sourceId)) {
        return reject(TrustCode::SourceNotAllowed,
                      "artifact provenance source is not allowed by the selected trust policy");
    }
    if (manifest.licenseId.empty()) {
        return reject(TrustCode::LicenseMissing, "artifact license identity is required");
    }
    if (policy.requireRedistributionPermission && !manifest.redistributionAllowed) {
        return reject(TrustCode::RedistributionNotAllowed,
                      "artifact lacks the required redistribution permission marker");
    }
    if (artifactClassCanExecute(manifest.artifactClass) &&
        (manifest.requiresInstall || manifest.requiresRestart) && !manifest.requiresRecoveryPlan) {
        return reject(TrustCode::RecoveryPlanRequired,
                      "installing/restarting executable artifact requires an explicit recovery plan");
    }

    if (!observation.present) {
        if (manifest.optional) {
            return {TrustDecision::OptionalAbsent, TrustCode::OptionalArtifactAbsent,
                    "optional artifact is absent; core operation remains available"};
        }
        return reject(TrustCode::RequiredArtifactMissing, "required artifact is missing");
    }
    if (!validArchitecture(observation.architecture) || !validSignature(observation.signature) ||
        !validId(observation.artifactVersion) || !validSha256(observation.observedSha256)) {
        return reject(TrustCode::InvalidManifest,
                      "artifact observation contains invalid version/hash/architecture/signature state");
    }
    if (observation.signature == SignatureState::ValidTrustedPublisher) {
        if (!validId(observation.publisherIdentity) ||
            observation.publisherIdentity.size() > kMaximumPublisherIdentityBytes) {
            return reject(TrustCode::SignatureMetadataMalformed,
                          "valid signature metadata is missing a bounded canonical publisher identity");
        }
    } else if ((observation.signature == SignatureState::Missing ||
                observation.signature == SignatureState::NotApplicable) &&
               !observation.publisherIdentity.empty()) {
        return reject(TrustCode::SignatureMetadataMalformed,
                      "unsigned/not-applicable signature metadata cannot carry a publisher identity");
    } else if (!observation.publisherIdentity.empty() &&
               (!validId(observation.publisherIdentity) ||
                observation.publisherIdentity.size() > kMaximumPublisherIdentityBytes)) {
        return reject(TrustCode::SignatureMetadataMalformed,
                      "signature metadata contains a malformed publisher identity");
    }
    if (observation.artifactVersion != manifest.artifactVersion) {
        return reject(TrustCode::VersionMismatch,
                      "observed artifact version does not match the trusted manifest");
    }
    if (observation.observedSha256 != manifest.expectedSha256) {
        return reject(TrustCode::HashMismatch,
                      "observed artifact hash does not match the trusted manifest");
    }
    if (manifest.architecture != ArtifactArchitecture::Any &&
        observation.architecture != manifest.architecture) {
        return reject(TrustCode::ArchitectureMismatch,
                      "observed artifact architecture does not match the trusted manifest");
    }
    if (artifactClassCanExecute(manifest.artifactClass) &&
        manifest.architecture != policy.hostArchitecture) {
        return reject(TrustCode::ArchitectureMismatch,
                      "executable artifact architecture is incompatible with the host policy");
    }

    if (observation.signature == SignatureState::Invalid ||
        observation.signature == SignatureState::UnknownPublisher) {
        return reject(TrustCode::SignatureInvalid,
                      "artifact signature/publisher trust is invalid or unknown");
    }
    if (observation.signature == SignatureState::ValidTrustedPublisher) {
        if (policy.trustedPublisherIdentities.empty()) {
            return reject(TrustCode::PublisherIdentityRequired,
                          "signature validity is insufficient without an exact trusted publisher identity policy");
        }
        if (!contains(policy.trustedPublisherIdentities, observation.publisherIdentity)) {
            return reject(TrustCode::PublisherIdentityMismatch,
                          "artifact signer identity does not match the exact trusted publisher policy");
        }
    }
    if (artifactClassCanExecute(manifest.artifactClass)) {
        const bool signedTrusted = observation.signature == SignatureState::ValidTrustedPublisher;
        const bool explicitDevelopmentException =
            manifest.developmentBuild && policy.allowUnsignedDevelopmentExecutables &&
            observation.signature == SignatureState::Missing;
        if (!signedTrusted && !explicitDevelopmentException) {
            return reject(TrustCode::SignatureRequired,
                          "executable artifact requires trusted signing or explicit development policy");
        }
    } else if (observation.signature != SignatureState::NotApplicable &&
               observation.signature != SignatureState::Missing &&
               observation.signature != SignatureState::ValidTrustedPublisher) {
        return reject(TrustCode::SignatureInvalid,
                      "data artifact carries an invalid signature state");
    }

    return {TrustDecision::Accept, TrustCode::Success,
            isDataOnly(manifest.artifactClass)
                ? "data-only artifact passed bounded hash/provenance/license policy"
                : "executable artifact passed hash/architecture/signature/publisher/capability policy"};
}

bool artifactClassCanExecute(ArtifactClass value) noexcept {
    return value == ArtifactClass::Executable || value == ArtifactClass::Driver ||
           value == ArtifactClass::ProviderHelper;
}

std::string_view artifactClassName(ArtifactClass value) noexcept {
    switch (value) {
        case ArtifactClass::DataCatalog: return "DataCatalog";
        case ArtifactClass::SetupPackage: return "SetupPackage";
        case ArtifactClass::Executable: return "Executable";
        case ArtifactClass::Driver: return "Driver";
        case ArtifactClass::ProviderHelper: return "ProviderHelper";
    }
    return "Unknown";
}

std::string_view artifactArchitectureName(ArtifactArchitecture value) noexcept {
    switch (value) {
        case ArtifactArchitecture::Any: return "Any";
        case ArtifactArchitecture::X86: return "X86";
        case ArtifactArchitecture::X64: return "X64";
        case ArtifactArchitecture::Arm64: return "Arm64";
    }
    return "Unknown";
}

std::string_view signatureStateName(SignatureState value) noexcept {
    switch (value) {
        case SignatureState::NotApplicable: return "NotApplicable";
        case SignatureState::Missing: return "Missing";
        case SignatureState::ValidTrustedPublisher: return "ValidTrustedPublisher";
        case SignatureState::Invalid: return "Invalid";
        case SignatureState::UnknownPublisher: return "UnknownPublisher";
    }
    return "Unknown";
}

std::string_view trustCodeName(TrustCode value) noexcept {
    switch (value) {
        case TrustCode::Success: return "Success";
        case TrustCode::OptionalArtifactAbsent: return "OptionalArtifactAbsent";
        case TrustCode::RequiredArtifactMissing: return "RequiredArtifactMissing";
        case TrustCode::InvalidManifest: return "InvalidManifest";
        case TrustCode::UnsupportedManifestVersion: return "UnsupportedManifestVersion";
        case TrustCode::InvalidHash: return "InvalidHash";
        case TrustCode::HashMismatch: return "HashMismatch";
        case TrustCode::VersionMismatch: return "VersionMismatch";
        case TrustCode::ArchitectureMismatch: return "ArchitectureMismatch";
        case TrustCode::SignatureRequired: return "SignatureRequired";
        case TrustCode::SignatureInvalid: return "SignatureInvalid";
        case TrustCode::SignatureMetadataMalformed: return "SignatureMetadataMalformed";
        case TrustCode::PublisherIdentityRequired: return "PublisherIdentityRequired";
        case TrustCode::PublisherIdentityMismatch: return "PublisherIdentityMismatch";
        case TrustCode::SourceNotAllowed: return "SourceNotAllowed";
        case TrustCode::CapabilityNotAllowed: return "CapabilityNotAllowed";
        case TrustCode::LicenseMissing: return "LicenseMissing";
        case TrustCode::RedistributionNotAllowed: return "RedistributionNotAllowed";
        case TrustCode::RecoveryPlanRequired: return "RecoveryPlanRequired";
    }
    return "Unknown";
}

} // namespace hydra::trust
