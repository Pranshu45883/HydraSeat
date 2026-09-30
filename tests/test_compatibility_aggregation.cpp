#include "hydra/compatibility_aggregation.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace hydra::compat;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

CompatibilityResult result(std::string id) {
    CompatibilityResult value;
    value.resultId = std::move(id);
    value.timestampClass = TimestampClass::DayBucket;
    value.timestampBucket = "2026-08-20";
    value.gameId = "game:cohort";
    value.providerId = "steam";
    value.providerAppId = "100";
    value.gameVersion = "1.0";
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "build-a";
    value.windowsBuildClass = "win10-2009";
    value.architecture = "x64";
    value.scenario = Scenario::SameGameTwoInstance;
    value.setupRevision = 4u;
    value.backends = {
        {"raw-input", "1", EvidenceStatus::Pass},
        {"xinput", "1", EvidenceStatus::Pass},
    };
    value.launch = EvidenceStatus::Pass;
    value.secondInstance = EvidenceStatus::Pass;
    value.inputIsolation = EvidenceStatus::Pass;
    value.controller = EvidenceStatus::Pass;
    value.audio = EvidenceStatus::Pass;
    value.cleanExit = EvidenceStatus::Pass;
    value.rollback = EvidenceStatus::Pass;
    value.measurements.launchDurationMicros = 100u;
    value.measurements.stopDurationMicros = 50u;
    value.measurements.rollbackDurationMicros = 20u;
    value.measurements.observedInputEvents = 1000u;
    value.measurements.verifiedCrossSeatEvents = 0u;
    value.measurements.inputLatencyP95Micros = 900u;
    value.origin = ResultOrigin::ControlledProcess;
    value.provenanceId = "fixture";
    value.provenanceRevision = 1u;
    return value;
}

AggregationPolicy policy() {
    return {"2026-08", 3u};
}

void testDeterministicCohortCountsAndUntestedSeparation() {
    auto pass = result("result-pass");
    auto fail = result("result-fail");
    fail.inputIsolation = EvidenceStatus::Fail;
    auto untested = result("result-untested");
    untested.inputIsolation = EvidenceStatus::NotMeasured;
    untested.measurements.observedInputEvents.reset();
    untested.measurements.verifiedCrossSeatEvents.reset();
    untested.measurements.inputLatencyP95Micros.reset();

    std::vector<CompatibilityResult> input{pass, fail, untested};
    std::vector<CohortStatistics> first;
    check(aggregateCompatibilityResults(input, policy(), first).succeeded() && first.size() == 1u,
          "materially identical environments aggregate into one cohort");
    if (!first.empty()) {
        check(first[0].sampleSize == 3u && first[0].session.success == 1u &&
                  first[0].session.failure == 1u && first[0].session.untested == 1u,
              "session Success/Failure/Untested are counted separately");
        check(first[0].inputIsolation.pass == 1u && first[0].inputIsolation.fail == 1u &&
                  first[0].inputIsolation.notMeasured == 1u,
              "sub-result statistics preserve missing evidence instead of treating it as zero/failure");
        check(first[0].sampleSize == first[0].session.total() &&
                  first[0].sampleSize == first[0].inputIsolation.total(),
              "cohort sample size matches every sub-result denominator");
    }

    std::reverse(input.begin(), input.end());
    std::vector<CohortStatistics> second;
    check(aggregateCompatibilityResults(input, policy(), second).succeeded() && second == first,
          "raw dataset order does not change displayed cohort statistics");
}

void testMaterialDifferencesNeverBlindlyMerge() {
    std::vector<CompatibilityResult> input;
    input.push_back(result("base"));

    auto gameVersion = result("game-version");
    gameVersion.gameVersion = "1.1";
    input.push_back(gameVersion);

    auto hydraVersion = result("hydra-version");
    hydraVersion.hydraSeatVersion = "0.2.0";
    input.push_back(hydraVersion);

    auto windows = result("windows-version");
    windows.windowsBuildClass = "win11-24h2";
    input.push_back(windows);

    auto architecture = result("architecture");
    architecture.architecture = "x86";
    input.push_back(architecture);

    auto setup = result("setup-revision");
    setup.setupRevision = 5u;
    input.push_back(setup);

    auto backend = result("backend-version");
    backend.backends[0].version = "2";
    input.push_back(backend);

    std::vector<CohortStatistics> output;
    check(aggregateCompatibilityResults(input, policy(), output).succeeded() &&
              output.size() == input.size(),
          "game/HydraSeat/Windows/arch/setup/backend material differences stay segmented");
}

void testFreshnessIsSegmentedNotSilentlyWeighted() {
    auto current = result("current");
    auto stale = result("stale");
    stale.timestampBucket = "2026-04-30";

    std::vector<CohortStatistics> output;
    const std::vector<CompatibilityResult> input{current, stale};
    check(aggregateCompatibilityResults(input, policy(), output).succeeded() && output.size() == 2u,
          "stale and current evidence are separate cohorts rather than silently averaged");
    bool sawCurrent = false;
    bool sawStale = false;
    for (const auto& cohort : output) {
        sawCurrent = sawCurrent || cohort.key.freshness == FreshnessClass::Current;
        sawStale = sawStale || cohort.key.freshness == FreshnessClass::Stale;
    }
    check(sawCurrent && sawStale, "freshness labels remain explicit in cohort keys");
}

void testProtectedTechnicalSuccessRemainsProtectedSegment() {
    auto normal = result("normal");
    auto protectedResult = result("protected");
    protectedResult.scenario = Scenario::ProtectedExperiment;
    protectedResult.protectedExperimental = true;

    std::vector<CohortStatistics> output;
    const std::vector<CompatibilityResult> input{normal, protectedResult};
    check(aggregateCompatibilityResults(input, policy(), output).succeeded() && output.size() == 2u,
          "protected experiment never merges with ordinary same-game evidence");
    bool sawProtectedSuccess = false;
    for (const auto& cohort : output) {
        if (cohort.key.protectedExperimental) {
            sawProtectedSuccess = cohort.session.success == 1u &&
                                  cohort.key.scenario == Scenario::ProtectedExperiment;
        }
    }
    check(sawProtectedSuccess,
          "technical success may be counted while Protected/Experimental classification remains explicit");
}

void testDuplicateFutureAndInvalidPolicyAreTransactional() {
    CohortStatistics sentinel;
    sentinel.key.gameId = "sentinel";
    std::vector<CohortStatistics> output{sentinel};
    const auto before = output;

    auto duplicateA = result("duplicate");
    auto duplicateB = result("duplicate");
    const std::vector<CompatibilityResult> duplicates{duplicateA, duplicateB};
    check(aggregateCompatibilityResults(duplicates, policy(), output).code ==
              AggregationCode::DuplicateResultId &&
              output == before,
          "duplicate result identity cannot double-count a sample or replace previous output");

    auto future = result("future");
    future.timestampBucket = "2026-09-01";
    const std::vector<CompatibilityResult> futureInput{future};
    check(aggregateCompatibilityResults(futureInput, policy(), output).code ==
              AggregationCode::FutureTimestamp &&
              output == before,
          "future timestamp fails closed under deterministic reference-month policy");

    auto invalidPolicy = policy();
    invalidPolicy.referenceMonth = "2026-13";
    const std::vector<CompatibilityResult> one{result("one")};
    check(aggregateCompatibilityResults(one, invalidPolicy, output).code ==
              AggregationCode::InvalidPolicy &&
              output == before,
          "invalid aggregation policy cannot mutate existing statistics");
}

} // namespace

int main() {
    testDeterministicCohortCountsAndUntestedSeparation();
    testMaterialDifferencesNeverBlindlyMerge();
    testFreshnessIsSegmentedNotSilentlyWeighted();
    testProtectedTechnicalSuccessRemainsProtectedSegment();
    testDuplicateFutureAndInvalidPolicyAreTransactional();
    if (failures != 0) {
        std::cerr << failures << " compatibility aggregation test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Compatibility aggregation tests passed.\n";
    return EXIT_SUCCESS;
}
