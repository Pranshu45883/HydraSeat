#include "hydra/catalog_cache.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace hydra::data {
namespace {

CatalogRefreshResult result(CatalogRefreshState state,
                            CatalogRefreshCode code,
                            std::string message) {
    return {state, code, std::move(message)};
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

bool validHash(std::string_view value) noexcept {
    if (value.size() != 64u) return false;
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

bool validArtifact(const CatalogArtifact& artifact) noexcept {
    return validId(artifact.catalogId) && artifact.revision != 0u &&
           validId(artifact.version) && validHash(artifact.expectedSha256) &&
           validHash(artifact.observedSha256) && validId(artifact.sourceId) &&
           validId(artifact.licenseId);
}

} // namespace

CatalogRefreshResult CatalogCacheModel::refresh(const CatalogRefreshInput& input,
                                                const CatalogRefreshPolicy& policy,
                                                const trust::TrustPolicy& trustPolicy) {
    if (!policy.refreshChecksEnabled) {
        return result(CatalogRefreshState::Disabled, CatalogRefreshCode::Success,
                      "catalog refresh checks are disabled; current cache remains authoritative");
    }
    if (!policy.sourceConfigured) {
        return result(CatalogRefreshState::NoSourceConfigured, CatalogRefreshCode::Success,
                      "no optional catalog source is configured");
    }
    if (!input.networkAvailable) {
        return result(current_ ? CatalogRefreshState::OfflineUsingCache
                               : CatalogRefreshState::OfflineNoCache,
                      CatalogRefreshCode::Success,
                      current_ ? "network unavailable; using last valid local catalog cache"
                               : "network unavailable; no optional catalog cache is installed");
    }
    if (!policy.downloadsEnabled) {
        return result(CatalogRefreshState::DownloadDisabled, CatalogRefreshCode::Success,
                      "catalog download is disabled; current cache remains unchanged");
    }
    if (!input.downloaded) {
        return result(current_ ? CatalogRefreshState::UpToDate : CatalogRefreshState::OfflineNoCache,
                      CatalogRefreshCode::Success,
                      current_ ? "no newer catalog artifact was supplied"
                               : "no catalog artifact was supplied for first install");
    }
    const auto& artifact = *input.downloaded;
    if (!validArtifact(artifact)) {
        return result(CatalogRefreshState::Rejected, CatalogRefreshCode::InvalidArtifact,
                      "downloaded catalog artifact metadata is invalid");
    }
    if (current_ && artifact.revision <= current_->revision &&
        current_->catalogId == artifact.catalogId) {
        if (artifact == *current_) {
            return result(CatalogRefreshState::UpToDate, CatalogRefreshCode::Success,
                          "catalog cache already contains this exact trusted revision");
        }
        return result(CatalogRefreshState::Rejected, CatalogRefreshCode::StaleRevision,
                      "downloaded catalog revision is stale or conflicts with the installed revision metadata");
    }

    trust::ArtifactManifest manifest;
    manifest.artifactId = artifact.catalogId;
    manifest.artifactClass = trust::ArtifactClass::DataCatalog;
    manifest.artifactVersion = artifact.version;
    manifest.architecture = trust::ArtifactArchitecture::Any;
    manifest.expectedSha256 = artifact.expectedSha256;
    manifest.sourceId = artifact.sourceId;
    manifest.licenseId = artifact.licenseId;
    manifest.redistributionAllowed = artifact.redistributionAllowed;
    manifest.optional = true;
    manifest.capabilityScope = {"compatibility-data", "two-player-setup"};

    trust::ArtifactObservation observation;
    observation.present = true;
    observation.artifactVersion = artifact.version;
    observation.architecture = trust::ArtifactArchitecture::Any;
    observation.observedSha256 = artifact.observedSha256;
    observation.signature = trust::SignatureState::NotApplicable;

    auto appliedPolicy = trustPolicy;
    appliedPolicy.requireRedistributionPermission = policy.requireRedistributionPermission;
    const auto trustEvaluation = trust::evaluateArtifact(manifest, observation, appliedPolicy);
    if (!trustEvaluation.accepted() || trustEvaluation.decision != trust::TrustDecision::Accept) {
        return result(CatalogRefreshState::Rejected, CatalogRefreshCode::TrustRejected,
                      "downloaded catalog failed trust validation: " + trustEvaluation.message);
    }
    if (current_ && (current_->catalogId != artifact.catalogId ||
                     current_->sourceId != artifact.sourceId)) {
        return result(CatalogRefreshState::Rejected, CatalogRefreshCode::InvalidArtifact,
                      "downloaded catalog identity/provenance does not match the installed cache lineage");
    }

    try {
        auto nextCurrent = current_;
        auto nextHistory = history_;
        if (nextCurrent) {
            nextHistory.push_back(*nextCurrent);
            if (nextHistory.size() > kMaximumCatalogCacheHistory) {
                nextHistory.erase(nextHistory.begin(),
                                  nextHistory.begin() +
                                      static_cast<std::ptrdiff_t>(nextHistory.size() -
                                                                  kMaximumCatalogCacheHistory));
            }
        }
        nextCurrent = artifact;
        current_.swap(nextCurrent);
        history_.swap(nextHistory);
        return result(CatalogRefreshState::Applied, CatalogRefreshCode::Success,
                      "trusted data-only catalog revision applied to the local cache");
    } catch (...) {
        return result(CatalogRefreshState::Rejected, CatalogRefreshCode::InvalidArtifact,
                      "catalog cache allocation failed before commit");
    }
}

CatalogRefreshResult CatalogCacheModel::rollback() {
    if (!current_) {
        return result(CatalogRefreshState::Rejected, CatalogRefreshCode::NoCurrentCache,
                      "no current catalog cache exists");
    }
    if (history_.empty()) {
        return result(CatalogRefreshState::Rejected, CatalogRefreshCode::NoRollbackVersion,
                      "no previous valid catalog cache revision exists");
    }
    try {
        auto nextCurrent = current_;
        auto nextHistory = history_;
        auto previous = nextHistory.back();
        nextHistory.pop_back();
        nextCurrent = std::move(previous);
        current_.swap(nextCurrent);
        history_.swap(nextHistory);
        return result(CatalogRefreshState::RolledBack, CatalogRefreshCode::Success,
                      "previous valid catalog cache revision restored");
    } catch (...) {
        return result(CatalogRefreshState::Rejected, CatalogRefreshCode::InvalidArtifact,
                      "catalog rollback allocation failed before commit");
    }
}

void CatalogCacheModel::clear() noexcept {
    current_.reset();
    history_.clear();
}

std::string_view catalogRefreshStateName(CatalogRefreshState value) noexcept {
    switch (value) {
        case CatalogRefreshState::Applied: return "Applied";
        case CatalogRefreshState::UpToDate: return "UpToDate";
        case CatalogRefreshState::Disabled: return "Disabled";
        case CatalogRefreshState::OfflineUsingCache: return "OfflineUsingCache";
        case CatalogRefreshState::OfflineNoCache: return "OfflineNoCache";
        case CatalogRefreshState::NoSourceConfigured: return "NoSourceConfigured";
        case CatalogRefreshState::DownloadDisabled: return "DownloadDisabled";
        case CatalogRefreshState::Rejected: return "Rejected";
        case CatalogRefreshState::RolledBack: return "RolledBack";
    }
    return "Unknown";
}

std::string_view catalogRefreshCodeName(CatalogRefreshCode value) noexcept {
    switch (value) {
        case CatalogRefreshCode::Success: return "Success";
        case CatalogRefreshCode::InvalidArtifact: return "InvalidArtifact";
        case CatalogRefreshCode::StaleRevision: return "StaleRevision";
        case CatalogRefreshCode::TrustRejected: return "TrustRejected";
        case CatalogRefreshCode::NoCurrentCache: return "NoCurrentCache";
        case CatalogRefreshCode::NoRollbackVersion: return "NoRollbackVersion";
    }
    return "Unknown";
}

} // namespace hydra::data
