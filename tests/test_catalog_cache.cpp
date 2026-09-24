#include "hydra/catalog_cache.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace hydra::data;
using namespace hydra::trust;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string hash(char ch) {
    return std::string(64u, ch);
}

CatalogArtifact artifact(std::uint64_t revision = 1u,
                         std::string version = "1.0.0",
                         char hashChar = 'a') {
    CatalogArtifact value;
    value.catalogId = "community-catalog";
    value.revision = revision;
    value.version = std::move(version);
    value.expectedSha256 = hash(hashChar);
    value.observedSha256 = hash(hashChar);
    value.sourceId = "community-reviewed";
    value.licenseId = "CC0-1.0";
    value.redistributionAllowed = true;
    return value;
}

TrustPolicy trustPolicy() {
    TrustPolicy value;
    value.hostArchitecture = ArtifactArchitecture::X64;
    value.allowedSourceIds = {"community-reviewed"};
    value.allowedCapabilities = {"compatibility-data", "two-player-setup"};
    return value;
}

void testFirstDownloadOfflineAndDisabledRemainIndependent() {
    CatalogCacheModel cache;
    CatalogRefreshPolicy policy;
    auto first = artifact();
    const CatalogRefreshInput online{true, first};
    check(cache.refresh(online, policy, trustPolicy()).state == CatalogRefreshState::Applied &&
              cache.current() && cache.current()->revision == 1u,
          "first trusted data-only catalog download becomes the local cache");

    const CatalogRefreshInput offline{false, std::nullopt};
    const auto offlineResult = cache.refresh(offline, policy, trustPolicy());
    check(offlineResult.state == CatalogRefreshState::OfflineUsingCache &&
              cache.current()->revision == 1u,
          "network-off operation uses last valid local cache without bricking local features");

    policy.refreshChecksEnabled = false;
    auto newer = artifact(2u, "1.1.0", 'b');
    const auto disabled = cache.refresh({true, newer}, policy, trustPolicy());
    check(disabled.state == CatalogRefreshState::Disabled &&
              cache.current()->revision == 1u,
          "disabled refresh does not inspect/apply supplied remote data");

    policy.refreshChecksEnabled = true;
    policy.downloadsEnabled = false;
    const auto noDownload = cache.refresh({true, newer}, policy, trustPolicy());
    check(noDownload.state == CatalogRefreshState::DownloadDisabled &&
              cache.current()->revision == 1u,
          "download-disabled setting preserves current offline cache");
}

void testTamperAndSourceFailurePreserveLastValidCache() {
    CatalogCacheModel cache;
    CatalogRefreshPolicy policy;
    auto first = artifact();
    check(cache.refresh({true, first}, policy, trustPolicy()).succeeded(),
          "baseline trusted cache applies");
    const auto before = *cache.current();

    auto tampered = artifact(2u, "1.1.0", 'b');
    tampered.observedSha256 = hash('c');
    const auto rejected = cache.refresh({true, tampered}, policy, trustPolicy());
    check(rejected.state == CatalogRefreshState::Rejected &&
              rejected.code == CatalogRefreshCode::TrustRejected &&
              *cache.current() == before,
          "tampered catalog download cannot replace last valid cache");

    auto untrusted = artifact(2u, "1.1.0", 'b');
    untrusted.sourceId = "unknown-source";
    check(cache.refresh({true, untrusted}, policy, trustPolicy()).code ==
              CatalogRefreshCode::TrustRejected &&
              *cache.current() == before,
          "untrusted provenance cannot replace cache");

    policy.sourceConfigured = false;
    const auto noSource = cache.refresh({true, artifact(2u, "1.1.0", 'b')},
                                        policy, trustPolicy());
    check(noSource.state == CatalogRefreshState::NoSourceConfigured &&
              *cache.current() == before,
          "missing optional source is non-fatal and preserves offline cache");
}

void testRefreshCannotSilentlySwitchCacheLineage() {
    CatalogCacheModel cache;
    CatalogRefreshPolicy policy;
    const auto first = artifact();
    check(cache.refresh({true, first}, policy, trustPolicy()).succeeded(),
          "baseline catalog lineage applies before identity/provenance drift checks");
    const auto before = *cache.current();

    auto differentCatalog = artifact(2u, "1.1.0", 'b');
    differentCatalog.catalogId = "other-catalog";
    check(cache.refresh({true, differentCatalog}, policy, trustPolicy()).code ==
              CatalogRefreshCode::InvalidArtifact &&
              *cache.current() == before && cache.history().empty(),
          "refresh cannot silently replace the installed cache with a different catalog identity");

    auto differentSource = artifact(2u, "1.1.0", 'b');
    differentSource.sourceId = "secondary-reviewed";
    auto multipleAllowedSources = trustPolicy();
    multipleAllowedSources.allowedSourceIds.push_back("secondary-reviewed");
    check(cache.refresh({true, differentSource}, policy, multipleAllowedSources).code ==
              CatalogRefreshCode::InvalidArtifact &&
              *cache.current() == before && cache.history().empty(),
          "refresh cannot silently move an existing catalog lineage to another allowed provenance source");

    cache.clear();
    check(cache.refresh({true, differentCatalog}, policy, trustPolicy()).succeeded() &&
              cache.current() && cache.current()->catalogId == "other-catalog",
          "explicit cache clear permits a deliberate new catalog lineage on the next first download");
}

void testUpdateRollbackAndStaleConflict() {
    CatalogCacheModel cache;
    CatalogRefreshPolicy policy;
    auto first = artifact();
    auto second = artifact(2u, "1.1.0", 'b');
    check(cache.refresh({true, first}, policy, trustPolicy()).succeeded() &&
              cache.refresh({true, second}, policy, trustPolicy()).succeeded(),
          "sequential trusted catalog revisions apply");
    check(cache.current()->revision == 2u && cache.history().size() == 1u,
          "prior revision is retained for rollback");

    auto sameRevisionMetadataDrift = second;
    sameRevisionMetadataDrift.licenseId = "MIT";
    check(cache.refresh({true, sameRevisionMetadataDrift}, policy, trustPolicy()).code ==
              CatalogRefreshCode::StaleRevision &&
              cache.current() && *cache.current() == second && cache.history().size() == 1u,
          "same revision/hash cannot silently change catalog provenance or license metadata");

    auto stale = artifact(1u, "0.9.0", 'c');
    check(cache.refresh({true, stale}, policy, trustPolicy()).code ==
              CatalogRefreshCode::StaleRevision &&
              cache.current()->revision == 2u,
          "older/conflicting revision is rejected without rollback by accident");

    check(cache.rollback().state == CatalogRefreshState::RolledBack &&
              cache.current()->revision == 1u,
          "explicit rollback restores previous trusted catalog revision");
    check(cache.rollback().code == CatalogRefreshCode::NoRollbackVersion,
          "rollback history is consumed deterministically");
}

void testHistoryRemainsBoundedAcrossManyTrustedUpdates() {
    CatalogCacheModel cache;
    CatalogRefreshPolicy policy;
    for (std::uint64_t revision = 1u; revision <= 12u; ++revision) {
        const char hashChar = static_cast<char>('a' + static_cast<int>(revision % 6u));
        auto next = artifact(revision, "1.0." + std::to_string(revision), hashChar);
        check(cache.refresh({true, next}, policy, trustPolicy()).succeeded(),
              "trusted catalog update in bounded-history sequence applies");
    }
    check(cache.current() && cache.current()->revision == 12u &&
              cache.history().size() == kMaximumCatalogCacheHistory &&
              cache.history().front().revision == 4u &&
              cache.history().back().revision == 11u,
          "catalog cache keeps only the newest bounded rollback history");
}

void testNoCacheOfflineAndRedistributionPolicy() {
    CatalogCacheModel cache;
    CatalogRefreshPolicy policy;
    const auto offline = cache.refresh({false, std::nullopt}, policy, trustPolicy());
    check(offline.state == CatalogRefreshState::OfflineNoCache && offline.succeeded() &&
              !cache.current(),
          "network-off with no optional cache remains a valid core/offline state");

    auto restricted = artifact();
    restricted.redistributionAllowed = false;
    policy.requireRedistributionPermission = true;
    check(cache.refresh({true, restricted}, policy, trustPolicy()).code ==
              CatalogRefreshCode::TrustRejected &&
              !cache.current(),
          "distribution policy rejects catalog lacking required redistribution metadata");
}

} // namespace

int main() {
    testFirstDownloadOfflineAndDisabledRemainIndependent();
    testTamperAndSourceFailurePreserveLastValidCache();
    testRefreshCannotSilentlySwitchCacheLineage();
    testUpdateRollbackAndStaleConflict();
    testHistoryRemainsBoundedAcrossManyTrustedUpdates();
    testNoCacheOfflineAndRedistributionPolicy();
    if (failures != 0) {
        std::cerr << failures << " catalog cache test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Catalog cache tests passed.\n";
    return EXIT_SUCCESS;
}
