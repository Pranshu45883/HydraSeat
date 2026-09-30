#include "hydra/compatibility_catalog.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace hydra::community {
namespace {

CatalogDiagnostic fail(CatalogCode code, std::string message) {
    return {code, std::move(message)};
}

bool selectorEqual(const GameSelector& left, const GameSelector& right) noexcept {
    return left.gameId == right.gameId && left.providerId == right.providerId &&
           left.providerAppId == right.providerAppId && left.gameVersion == right.gameVersion;
}

GameSelector selectorFor(const compat::CompatibilityResult& result) {
    return {result.gameId, result.providerId, result.providerAppId, result.gameVersion};
}

const PackageEntry* findEntry(const CommunityPackageManifest& manifest,
                              std::string_view entryId) noexcept {
    for (const auto& entry : manifest.entries) {
        if (entry.entryId == entryId) return &entry;
    }
    return nullptr;
}

} // namespace

CatalogDiagnostic buildLocalCatalogSnapshot(
    const CommunityPackageManifest& manifest,
    std::span<const compat::CompatibilityResult> results,
    std::span<const CommunitySetupEntry> setups,
    LocalCatalogSnapshot& output) {
    const auto packageValidation = validateCommunityPackageManifest(manifest);
    if (!packageValidation.succeeded()) {
        return fail(CatalogCode::InvalidPackage,
                    "catalog manifest is invalid: " + packageValidation.message);
    }
    if (results.size() > kMaximumCatalogResults || setups.size() > kMaximumCatalogSetups) {
        return fail(CatalogCode::TooManyPayloads,
                    "decoded catalog payload count exceeds the bounded maximum");
    }

    try {
        LocalCatalogSnapshot candidate;
        candidate.manifest = manifest;
        std::set<std::string> payloadIds;

        candidate.results.reserve(results.size());
        for (const auto& source : results) {
            compat::CompatibilityResult result = source;
            const auto validation = compat::canonicalizeCompatibilityResult(result);
            if (!validation.succeeded()) {
                return fail(CatalogCode::InvalidResult,
                            "invalid compatibility result payload: " + validation.message);
            }
            if (!payloadIds.insert(result.resultId).second) {
                return fail(CatalogCode::DuplicatePayload,
                            "decoded catalog contains duplicate payload identity");
            }
            const auto* entry = findEntry(manifest, result.resultId);
            if (entry == nullptr) {
                return fail(CatalogCode::MissingManifestEntry,
                            "compatibility result payload is not declared by the package manifest");
            }
            if (entry->kind != EntryKind::CompatibilityResult) {
                return fail(CatalogCode::KindMismatch,
                            "package manifest kind does not match decoded compatibility result");
            }
            if (!selectorEqual(entry->selector, selectorFor(result))) {
                return fail(CatalogCode::SelectorMismatch,
                            "compatibility result selector differs from the package manifest");
            }
            candidate.results.push_back(std::move(result));
        }

        candidate.setups.reserve(setups.size());
        for (const auto& setup : setups) {
            const auto validation = validateCommunitySetupEntry(setup);
            if (!validation.succeeded()) {
                return fail(CatalogCode::InvalidSetup,
                            "invalid community setup payload: " + validation.message);
            }
            if (!payloadIds.insert(setup.entryId).second) {
                return fail(CatalogCode::DuplicatePayload,
                            "decoded catalog contains duplicate payload identity");
            }
            const auto* entry = findEntry(manifest, setup.entryId);
            if (entry == nullptr) {
                return fail(CatalogCode::MissingManifestEntry,
                            "community setup payload is not declared by the package manifest");
            }
            if (entry->kind != EntryKind::TwoPlayerSetup) {
                return fail(CatalogCode::KindMismatch,
                            "package manifest kind does not match decoded community setup");
            }
            if (!selectorEqual(entry->selector, setup.selector)) {
                return fail(CatalogCode::SelectorMismatch,
                            "community setup selector differs from the package manifest");
            }
            if (setup.packageId != manifest.packageId ||
                setup.packageRevision != manifest.packageRevision) {
                return fail(CatalogCode::InvalidSetup,
                            "community setup package identity/revision differs from its catalog");
            }
            candidate.setups.push_back(setup);
        }

        std::sort(candidate.results.begin(), candidate.results.end(),
                  [](const auto& left, const auto& right) {
                      return left.resultId < right.resultId;
                  });
        std::sort(candidate.setups.begin(), candidate.setups.end(),
                  [](const auto& left, const auto& right) {
                      return left.entryId < right.entryId;
                  });
        output = std::move(candidate);
        return {};
    } catch (...) {
        return fail(CatalogCode::InvalidPackage,
                    "local catalog snapshot allocation failed before commit");
    }
}

CatalogDiagnostic queryGameEvidence(
    const LocalCatalogSnapshot& snapshot,
    const GameSelector& selector,
    const compat::AggregationPolicy& policy,
    GameEvidenceView& output) {
    const auto packageValidation = validateCommunityPackageManifest(snapshot.manifest);
    if (!packageValidation.succeeded()) {
        return fail(CatalogCode::InvalidPackage,
                    "local catalog snapshot manifest is no longer valid");
    }
    try {
        std::vector<compat::CompatibilityResult> matchingResults;
        matchingResults.reserve(snapshot.results.size());
        for (const auto& result : snapshot.results) {
            if (selectorEqual(selector, selectorFor(result))) matchingResults.push_back(result);
        }
        std::vector<compat::CohortStatistics> cohorts;
        const auto aggregation = compat::aggregateCompatibilityResults(
            matchingResults, policy, cohorts);
        if (!aggregation.succeeded()) {
            return fail(CatalogCode::AggregationFailed,
                        "local catalog cohort aggregation failed: " + aggregation.message);
        }

        GameEvidenceView candidate;
        candidate.selector = selector;
        candidate.cohorts = std::move(cohorts);
        for (const auto& setup : snapshot.setups) {
            if (selectorEqual(selector, setup.selector)) candidate.setups.push_back(setup);
        }
        std::sort(candidate.setups.begin(), candidate.setups.end(),
                  [](const auto& left, const auto& right) {
                      return left.entryId < right.entryId;
                  });
        for (const auto& cohort : candidate.cohorts) {
            candidate.totalResultSamples += cohort.sampleSize;
            if (cohort.key.freshness == compat::FreshnessClass::Current) {
                candidate.currentResultSamples += cohort.sampleSize;
            } else {
                candidate.staleResultSamples += cohort.sampleSize;
            }
        }
        output = std::move(candidate);
        return {};
    } catch (...) {
        return fail(CatalogCode::AggregationFailed,
                    "local catalog query allocation failed before commit");
    }
}

std::string_view catalogCodeName(CatalogCode code) noexcept {
    switch (code) {
        case CatalogCode::Success: return "Success";
        case CatalogCode::InvalidPackage: return "InvalidPackage";
        case CatalogCode::TooManyPayloads: return "TooManyPayloads";
        case CatalogCode::MissingManifestEntry: return "MissingManifestEntry";
        case CatalogCode::DuplicatePayload: return "DuplicatePayload";
        case CatalogCode::KindMismatch: return "KindMismatch";
        case CatalogCode::SelectorMismatch: return "SelectorMismatch";
        case CatalogCode::InvalidResult: return "InvalidResult";
        case CatalogCode::InvalidSetup: return "InvalidSetup";
        case CatalogCode::AggregationFailed: return "AggregationFailed";
    }
    return "Unknown";
}

} // namespace hydra::community
