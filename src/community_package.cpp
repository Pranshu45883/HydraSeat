#include "hydra/community_package.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace hydra::community {
namespace {

PackageDiagnostic fail(PackageCode code, std::string message) {
    return {code, std::move(message)};
}

bool validKind(EntryKind kind) noexcept {
    return kind == EntryKind::CompatibilityResult ||
           kind == EntryKind::TwoPlayerSetup ||
           kind == EntryKind::CatalogSnapshot;
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

bool validVersion(std::string_view value) noexcept {
    return validId(value);
}

bool validSha256(std::string_view value) noexcept {
    if (value.size() != 64u) return false;
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

bool validContentSchema(EntryKind kind, std::uint32_t version) noexcept {
    switch (kind) {
        case EntryKind::CompatibilityResult:
            return version == compat::kCompatibilityResultSchemaVersion;
        case EntryKind::TwoPlayerSetup:
            return portable::isSupportedSetupPackageVersion(version);
        case EntryKind::CatalogSnapshot:
            return version == profile::kProfileSchemaVersion;
    }
    return false;
}

std::vector<std::string> capabilityScopeFor(const CommunityPackageManifest& manifest) {
    std::set<std::string> capabilities;
    for (const auto& entry : manifest.entries) {
        switch (entry.kind) {
            case EntryKind::CompatibilityResult:
            case EntryKind::CatalogSnapshot:
                capabilities.insert("compatibility-data");
                break;
            case EntryKind::TwoPlayerSetup:
                capabilities.insert("two-player-setup");
                break;
        }
    }
    return {capabilities.begin(), capabilities.end()};
}

trust::ArtifactManifest trustManifestFor(const CommunityPackageManifest& manifest) {
    trust::ArtifactManifest artifact;
    artifact.artifactId = manifest.packageId;
    artifact.artifactClass = trust::ArtifactClass::DataCatalog;
    artifact.artifactVersion = manifest.packageVersion;
    artifact.architecture = trust::ArtifactArchitecture::Any;
    artifact.expectedSha256 = manifest.packageSha256;
    artifact.sourceId = manifest.sourceId;
    artifact.licenseId = manifest.licenseId;
    artifact.redistributionAllowed = manifest.redistributionAllowed;
    artifact.optional = true;
    artifact.capabilityScope = capabilityScopeFor(manifest);
    return artifact;
}

} // namespace

PackageDiagnostic validateCommunityPackageManifest(const CommunityPackageManifest& manifest) {
    if (manifest.schemaVersion != kCommunityPackageManifestVersion) {
        return fail(PackageCode::UnsupportedManifestVersion,
                    "unsupported community package manifest version");
    }
    if (!validId(manifest.packageId) || manifest.packageRevision == 0u ||
        !validVersion(manifest.packageVersion) || !validSha256(manifest.packageSha256) ||
        !validId(manifest.sourceId) || !validId(manifest.licenseId)) {
        return fail(PackageCode::InvalidManifest,
                    "community package manifest has invalid bounded identity/version/hash/provenance fields");
    }
    if (manifest.minimumProfileSchema == 0u ||
        manifest.maximumProfileSchema < manifest.minimumProfileSchema ||
        profile::kProfileSchemaVersion < manifest.minimumProfileSchema ||
        profile::kProfileSchemaVersion > manifest.maximumProfileSchema) {
        return fail(PackageCode::UnsupportedSchemaRange,
                    "community package profile schema range does not include the current profile schema");
    }
    if (manifest.entries.size() > kMaximumCommunityPackageEntries) {
        return fail(PackageCode::TooManyEntries,
                    "community package entry count exceeds the bounded maximum");
    }

    std::set<std::string> entryIds;
    for (const auto& entry : manifest.entries) {
        if (!validKind(entry.kind) || !validId(entry.entryId) ||
            !validSha256(entry.expectedSha256) ||
            !validContentSchema(entry.kind, entry.contentSchemaVersion)) {
            return fail(PackageCode::InvalidEntry,
                        "community package entry has invalid kind/id/hash/schema version");
        }
        if (!entryIds.insert(entry.entryId).second) {
            return fail(PackageCode::DuplicateEntry,
                        "community package contains duplicate entry identity");
        }
        const auto& selector = entry.selector;
        if (!validId(selector.gameId) || !validId(selector.providerId) ||
            (selector.providerAppId && !validId(*selector.providerAppId)) ||
            (selector.gameVersion && !validVersion(*selector.gameVersion))) {
            return fail(PackageCode::InvalidEntry,
                        "community package game selector is invalid or path-like");
        }
    }
    return {};
}

PackageDiagnostic CommunityPackageStore::installOrUpdate(
    const CommunityPackageManifest& manifest,
    std::string_view observedPackageSha256,
    std::span<const EntryObservation> entries,
    const trust::TrustPolicy& policy) {
    const auto manifestValidation = validateCommunityPackageManifest(manifest);
    if (!manifestValidation.succeeded()) return manifestValidation;
    if (!validSha256(observedPackageSha256)) {
        return fail(PackageCode::PackageTrustRejected,
                    "observed community package hash is malformed");
    }
    if (current_ && current_->packageId == manifest.packageId &&
        manifest.packageRevision <= current_->packageRevision) {
        return fail(PackageCode::StaleRevision,
                    "community package update revision must increase monotonically");
    }

    const auto artifact = trustManifestFor(manifest);
    trust::ArtifactObservation observation;
    observation.present = true;
    observation.artifactVersion = manifest.packageVersion;
    observation.architecture = trust::ArtifactArchitecture::Any;
    observation.observedSha256 = std::string(observedPackageSha256);
    observation.signature = trust::SignatureState::NotApplicable;
    const auto trustEvaluation = trust::evaluateArtifact(artifact, observation, policy);
    if (!trustEvaluation.accepted() || trustEvaluation.decision != trust::TrustDecision::Accept) {
        return fail(PackageCode::PackageTrustRejected,
                    "community package failed trust policy: " + trustEvaluation.message);
    }

    if (entries.size() != manifest.entries.size()) {
        return fail(entries.size() < manifest.entries.size()
                        ? PackageCode::MissingEntry
                        : PackageCode::UnexpectedEntry,
                    "community package observed entry count does not match the manifest");
    }
    std::map<std::string, std::string> observed;
    for (const auto& entry : entries) {
        if (!validId(entry.entryId) || !validSha256(entry.observedSha256)) {
            return fail(PackageCode::InvalidEntry,
                        "observed community entry identity/hash is malformed");
        }
        if (!observed.emplace(entry.entryId, entry.observedSha256).second) {
            return fail(PackageCode::DuplicateEntry,
                        "observed community package entries contain duplicate identity");
        }
    }
    for (const auto& expected : manifest.entries) {
        const auto found = observed.find(expected.entryId);
        if (found == observed.end()) {
            return fail(PackageCode::MissingEntry,
                        "community package is missing a manifest-declared entry");
        }
        if (found->second != expected.expectedSha256) {
            return fail(PackageCode::EntryHashMismatch,
                        "community package entry hash does not match its manifest");
        }
    }
    for (const auto& [entryId, hash] : observed) {
        (void)hash;
        const auto declared = std::find_if(manifest.entries.begin(), manifest.entries.end(),
                                           [&](const auto& expected) {
                                               return expected.entryId == entryId;
                                           });
        if (declared == manifest.entries.end()) {
            return fail(PackageCode::UnexpectedEntry,
                        "community package contains an undeclared observed entry");
        }
    }

    try {
        auto nextHistory = history_;
        if (current_) {
            nextHistory.push_back(*current_);
            if (nextHistory.size() > kMaximumCommunityPackageHistory) {
                nextHistory.erase(nextHistory.begin(),
                                  nextHistory.begin() +
                                      static_cast<std::ptrdiff_t>(nextHistory.size() -
                                                                  kMaximumCommunityPackageHistory));
            }
        }
        current_ = manifest;
        history_ = std::move(nextHistory);
        return {};
    } catch (...) {
        return fail(PackageCode::InvalidManifest,
                    "community package lifecycle allocation failed before commit");
    }
}

PackageDiagnostic CommunityPackageStore::rollback() {
    if (!current_) {
        return fail(PackageCode::NoInstalledPackage,
                    "no community package is installed");
    }
    if (history_.empty()) {
        return fail(PackageCode::NoRollbackVersion,
                    "no previous community package revision is available");
    }
    try {
        auto history = history_;
        auto previous = history.back();
        history.pop_back();
        current_ = std::move(previous);
        history_ = std::move(history);
        return {};
    } catch (...) {
        return fail(PackageCode::InvalidManifest,
                    "community package rollback allocation failed before commit");
    }
}

void CommunityPackageStore::clear() noexcept {
    current_.reset();
    history_.clear();
}

std::string_view entryKindName(EntryKind kind) noexcept {
    switch (kind) {
        case EntryKind::CompatibilityResult: return "CompatibilityResult";
        case EntryKind::TwoPlayerSetup: return "TwoPlayerSetup";
        case EntryKind::CatalogSnapshot: return "CatalogSnapshot";
    }
    return "Unknown";
}

std::string_view packageCodeName(PackageCode code) noexcept {
    switch (code) {
        case PackageCode::Success: return "Success";
        case PackageCode::InvalidManifest: return "InvalidManifest";
        case PackageCode::UnsupportedManifestVersion: return "UnsupportedManifestVersion";
        case PackageCode::UnsupportedSchemaRange: return "UnsupportedSchemaRange";
        case PackageCode::TooManyEntries: return "TooManyEntries";
        case PackageCode::DuplicateEntry: return "DuplicateEntry";
        case PackageCode::InvalidEntry: return "InvalidEntry";
        case PackageCode::MissingEntry: return "MissingEntry";
        case PackageCode::UnexpectedEntry: return "UnexpectedEntry";
        case PackageCode::EntryHashMismatch: return "EntryHashMismatch";
        case PackageCode::PackageTrustRejected: return "PackageTrustRejected";
        case PackageCode::StaleRevision: return "StaleRevision";
        case PackageCode::NoInstalledPackage: return "NoInstalledPackage";
        case PackageCode::NoRollbackVersion: return "NoRollbackVersion";
    }
    return "Unknown";
}

} // namespace hydra::community
