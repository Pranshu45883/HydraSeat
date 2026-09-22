#include "hydra/compatibility_aggregation.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace hydra::compat {
namespace {

bool parseMonth(std::string_view value, std::int64_t& output) noexcept {
    if (value.size() != 7u || value[4] != '-') return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4u) continue;
        if (value[index] < '0' || value[index] > '9') return false;
    }
    const auto year = static_cast<std::int64_t>(value[0] - '0') * 1000 +
                      static_cast<std::int64_t>(value[1] - '0') * 100 +
                      static_cast<std::int64_t>(value[2] - '0') * 10 +
                      static_cast<std::int64_t>(value[3] - '0');
    const auto month = static_cast<std::int64_t>(value[5] - '0') * 10 +
                       static_cast<std::int64_t>(value[6] - '0');
    if (year < 2000 || year > 9999 || month < 1 || month > 12) return false;
    output = year * 12 + (month - 1);
    return true;
}

bool resultMonth(const CompatibilityResult& result, std::int64_t& output) noexcept {
    if (result.timestampBucket.size() < 7u) return false;
    return parseMonth(std::string_view(result.timestampBucket).substr(0u, 7u), output);
}

CohortKey makeKey(const CompatibilityResult& result, FreshnessClass freshness) {
    CohortKey key;
    key.gameId = result.gameId;
    key.providerId = result.providerId;
    key.providerAppId = result.providerAppId;
    key.gameVersion = result.gameVersion;
    key.hydraSeatVersion = result.hydraSeatVersion;
    key.windowsBuildClass = result.windowsBuildClass;
    key.architecture = result.architecture;
    key.scenario = result.scenario;
    key.protectedExperimental = result.protectedExperimental;
    key.setupRevision = result.setupRevision;
    key.freshness = freshness;
    key.backends.reserve(result.backends.size());
    for (const auto& backend : result.backends) {
        key.backends.push_back({backend.backendId, backend.version});
    }
    std::sort(key.backends.begin(), key.backends.end());
    return key;
}

void countOutcome(OutcomeCounts& counts, EvidenceStatus status) noexcept {
    switch (status) {
        case EvidenceStatus::NotMeasured: ++counts.notMeasured; break;
        case EvidenceStatus::Pass: ++counts.pass; break;
        case EvidenceStatus::Fail: ++counts.fail; break;
        case EvidenceStatus::Unsupported: ++counts.unsupported; break;
    }
}

void countSession(SessionDispositionCounts& counts, SessionDisposition value) noexcept {
    switch (value) {
        case SessionDisposition::Untested: ++counts.untested; break;
        case SessionDisposition::Success: ++counts.success; break;
        case SessionDisposition::Failure: ++counts.failure; break;
    }
}

void countOrigin(OriginCounts& counts, ResultOrigin value) noexcept {
    switch (value) {
        case ResultOrigin::Synthetic: ++counts.synthetic; break;
        case ResultOrigin::ControlledProcess: ++counts.controlledProcess; break;
        case ResultOrigin::Physical: ++counts.physical; break;
        case ResultOrigin::ImportedCommunity: ++counts.importedCommunity; break;
    }
}

} // namespace

SessionDisposition classifySessionDisposition(const CompatibilityResult& result) noexcept {
    std::array<EvidenceStatus, 4> required{
        result.launch,
        result.inputIsolation,
        result.cleanExit,
        result.scenario == Scenario::DifferentGames
            ? EvidenceStatus::Pass
            : result.secondInstance,
    };
    bool untested = false;
    for (const auto status : required) {
        if (status == EvidenceStatus::Fail || status == EvidenceStatus::Unsupported) {
            return SessionDisposition::Failure;
        }
        if (status == EvidenceStatus::NotMeasured) untested = true;
    }
    return untested ? SessionDisposition::Untested : SessionDisposition::Success;
}

AggregationDiagnostic aggregateCompatibilityResults(
    std::span<const CompatibilityResult> results,
    const AggregationPolicy& policy,
    std::vector<CohortStatistics>& output) {
    std::int64_t referenceMonth = 0;
    if (!parseMonth(policy.referenceMonth, referenceMonth) || policy.staleAfterMonths > 120u) {
        return {AggregationCode::InvalidPolicy,
                "aggregation reference month or staleness horizon is invalid"};
    }
    if (results.size() > kMaximumAggregationResults) {
        return {AggregationCode::TooManyResults,
                "compatibility aggregation input exceeds the bounded maximum"};
    }

    try {
        std::set<std::string> resultIds;
        std::map<CohortKey, CohortStatistics> cohorts;
        for (const auto& source : results) {
            CompatibilityResult result = source;
            const auto validation = canonicalizeCompatibilityResult(result);
            if (!validation.succeeded()) {
                return {AggregationCode::InvalidResult,
                        "invalid compatibility result " + source.resultId + ": " +
                            validation.message};
            }
            if (!resultIds.insert(result.resultId).second) {
                return {AggregationCode::DuplicateResultId,
                        "duplicate result_id would double-count one evidence sample"};
            }

            std::int64_t observedMonth = 0;
            if (!resultMonth(result, observedMonth)) {
                return {AggregationCode::InvalidResult,
                        "result timestamp bucket cannot be grouped by month"};
            }
            if (observedMonth > referenceMonth) {
                return {AggregationCode::FutureTimestamp,
                        "future evidence timestamp is not aggregated into current statistics"};
            }
            const auto ageMonths = static_cast<std::uint64_t>(referenceMonth - observedMonth);
            const auto freshness = ageMonths > policy.staleAfterMonths
                                       ? FreshnessClass::Stale
                                       : FreshnessClass::Current;
            auto key = makeKey(result, freshness);
            auto [iterator, inserted] = cohorts.try_emplace(key);
            auto& cohort = iterator->second;
            if (inserted) cohort.key = std::move(key);
            ++cohort.sampleSize;
            countSession(cohort.session, classifySessionDisposition(result));
            countOutcome(cohort.launch, result.launch);
            countOutcome(cohort.secondInstance, result.secondInstance);
            countOutcome(cohort.inputIsolation, result.inputIsolation);
            countOutcome(cohort.controller, result.controller);
            countOutcome(cohort.audio, result.audio);
            countOutcome(cohort.cleanExit, result.cleanExit);
            countOutcome(cohort.rollback, result.rollback);
            countOrigin(cohort.origins, result.origin);
        }

        std::vector<CohortStatistics> aggregated;
        aggregated.reserve(cohorts.size());
        for (auto& [key, cohort] : cohorts) {
            (void)key;
            aggregated.push_back(std::move(cohort));
        }
        output = std::move(aggregated);
        return {};
    } catch (...) {
        return {AggregationCode::InvalidResult,
                "compatibility aggregation failed while allocating bounded cohort state"};
    }
}

std::string_view freshnessClassName(FreshnessClass value) noexcept {
    switch (value) {
        case FreshnessClass::Current: return "Current";
        case FreshnessClass::Stale: return "Stale";
    }
    return "Unknown";
}

std::string_view sessionDispositionName(SessionDisposition value) noexcept {
    switch (value) {
        case SessionDisposition::Untested: return "Untested";
        case SessionDisposition::Success: return "Success";
        case SessionDisposition::Failure: return "Failure";
    }
    return "Unknown";
}

std::string_view aggregationCodeName(AggregationCode value) noexcept {
    switch (value) {
        case AggregationCode::Success: return "Success";
        case AggregationCode::InvalidPolicy: return "InvalidPolicy";
        case AggregationCode::TooManyResults: return "TooManyResults";
        case AggregationCode::InvalidResult: return "InvalidResult";
        case AggregationCode::DuplicateResultId: return "DuplicateResultId";
        case AggregationCode::FutureTimestamp: return "FutureTimestamp";
    }
    return "Unknown";
}

} // namespace hydra::compat
