#include "hydra/compatibility_result.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::compat;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

CompatibilityResult fixture() {
    CompatibilityResult value;
    value.resultId = "result-20260829-a";
    value.timestampClass = TimestampClass::DayBucket;
    value.timestampBucket = "2026-08-29";
    value.gameId = "game:0123456789abcdef";
    value.providerId = "steam";
    value.providerAppId = "123456";
    value.gameVersion = "1.4.2-build7";
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "770cac0";
    value.windowsBuildClass = "win10-2009";
    value.architecture = "x64";
    value.scenario = Scenario::SameGameTwoInstance;
    value.setupRevision = 12u;
    value.backends = {
        {"xinput", "1", EvidenceStatus::Pass},
        {"raw-input", "v1", EvidenceStatus::Pass},
    };
    value.launch = EvidenceStatus::Pass;
    value.secondInstance = EvidenceStatus::Pass;
    value.inputIsolation = EvidenceStatus::Pass;
    value.controller = EvidenceStatus::Pass;
    value.audio = EvidenceStatus::Unsupported;
    value.cleanExit = EvidenceStatus::Pass;
    value.rollback = EvidenceStatus::Pass;
    value.measurements.launchDurationMicros = 120000u;
    value.measurements.stopDurationMicros = 80000u;
    value.measurements.rollbackDurationMicros = 25000u;
    value.measurements.observedInputEvents = 1200u;
    value.measurements.verifiedCrossSeatEvents = 0u;
    value.measurements.inputLatencyP95Micros = 2400u;
    value.origin = ResultOrigin::ControlledProcess;
    value.provenanceId = "phase5-session-metrics";
    value.provenanceRevision = 3u;
    return value;
}

void testCanonicalRoundTripAndOrder() {
    auto first = fixture();
    auto second = first;
    std::reverse(second.backends.begin(), second.backends.end());

    std::string firstJson;
    std::string secondJson;
    check(encodeCompatibilityResultJson(first, firstJson).succeeded() &&
              encodeCompatibilityResultJson(second, secondJson).succeeded(),
          "valid compatibility result encodes from either backend input order");
    check(firstJson == secondJson,
          "backend input order cannot change canonical public JSON");
    check(firstJson.find("certified") == std::string::npos,
          "public schema contains no certification field");
    check(firstJson.find("verified_cross_seat_events\":0") != std::string::npos,
          "a measured zero remains distinguishable from an unmeasured null");

    CompatibilityResult decoded;
    check(decodeCompatibilityResultJson(firstJson, decoded).succeeded(),
          "canonical public JSON decodes");
    auto canonical = first;
    check(canonicalizeCompatibilityResult(canonical).succeeded() && decoded == canonical,
          "decode round-trips to canonical bounded result");
}

void testFutureUnknownAndMalformedFailTransactionally() {
    std::string encoded;
    encodeCompatibilityResultJson(fixture(), encoded);
    CompatibilityResult output;
    output.resultId = "sentinel";
    const auto sentinel = output;

    auto future = encoded;
    const auto schema = future.find("\"schema_version\":1");
    if (schema != std::string::npos) {
        future.replace(schema, std::string("\"schema_version\":1").size(),
                       "\"schema_version\":2");
    }
    check(decodeCompatibilityResultJson(future, output).code ==
              CompatibilityResultCode::UnsupportedSchema &&
              output == sentinel,
          "future schema version fails closed without replacing caller state");

    auto unknown = encoded;
    unknown.insert(1u, "\"certified\":true,");
    check(decodeCompatibilityResultJson(unknown, output).code ==
              CompatibilityResultCode::UnknownField &&
              output == sentinel,
          "unknown future/certification semantics fail closed");

    auto trailing = encoded + " trailing";
    check(decodeCompatibilityResultJson(trailing, output).code ==
              CompatibilityResultCode::ParseError &&
              output == sentinel,
          "trailing malformed data fails transactionally");

    std::string oversized(kMaximumCompatibilityResultBytes + 1u, 'x');
    check(decodeCompatibilityResultJson(oversized, output).code ==
              CompatibilityResultCode::TooLarge &&
              output == sentinel,
          "oversized untrusted result is rejected before parsing");
}

void testPrivacyAndMeasurementContracts() {
    auto value = fixture();
    value.gameVersion = "C:\\Users\\Alice\\private";
    check(validateCompatibilityResult(value).code == CompatibilityResultCode::InvalidVersion,
          "personal absolute path cannot occupy public version field");

    value = fixture();
    value.redaction.playerNamesExcluded = false;
    check(validateCompatibilityResult(value).code == CompatibilityResultCode::InvalidRedaction,
          "public result cannot weaken required Player-name redaction declaration");

    value = fixture();
    value.launch = EvidenceStatus::NotMeasured;
    check(validateCompatibilityResult(value).code == CompatibilityResultCode::InvalidMeasurement,
          "numeric launch duration cannot masquerade as missing measurement");

    value = fixture();
    value.measurements.launchDurationMicros.reset();
    value.launch = EvidenceStatus::NotMeasured;
    std::string json;
    check(encodeCompatibilityResultJson(value, json).succeeded() &&
              json.find("launch_duration_us\":null") != std::string::npos,
          "missing measurement is encoded as null rather than zero");

    value = fixture();
    value.scenario = Scenario::ProtectedExperiment;
    value.protectedExperimental = false;
    check(validateCompatibilityResult(value).code ==
              CompatibilityResultCode::InvalidLocalEvidence,
          "protected experiment cannot lose its explicit warning classification");
}

metrics::SessionMetricsReport localMetrics() {
    metrics::SessionMetricsReport report;
    report.planFingerprint = 99u;
    report.origin = metrics::EvidenceOrigin::ControlledProcess;
    report.isolationVerdict = metrics::EvidenceVerdict::Pass;
    report.sessionVerdict = metrics::EvidenceVerdict::Pass;
    report.receiverEvidenceComplete = true;
    report.lossFreeEvidence = true;
    report.seats = {
        {1u, 110000u, 50000u, 20000u, true, true, true, true,
         metrics::CapabilityOutcome::Success, metrics::CapabilityOutcome::Unsupported},
        {2u, 125000u, 55000u, 18000u, true, true, true, true,
         metrics::CapabilityOutcome::Success, metrics::CapabilityOutcome::Unsupported},
    };
    report.finalState = metrics::SessionFinalState::ReturnedToWindows;
    report.rollbackAttempted = true;
    report.rollbackVerified = true;
    report.maximumLaunchDurationMicros = 125000u;
    report.maximumStopDurationMicros = 55000u;
    report.maximumRollbackDurationMicros = 20000u;
    report.input.uniqueInputEvents = 500u;
    report.input.receiverVerifiedEvents = 500u;
    report.input.crossSeatEvents = 0u;
    report.input.endToEnd.count = 500u;
    report.input.endToEnd.p95Micros = 1800u;
    return report;
}

LocalEvidenceContext localContext() {
    LocalEvidenceContext context;
    context.resultId = "result-local-1";
    context.timestampClass = TimestampClass::MonthBucket;
    context.timestampBucket = "2026-08";
    context.gameId = "game:local";
    context.providerId = "custom";
    context.providerAppId = "local-app";
    context.gameVersion = "2.0";
    context.hydraSeatVersion = "0.1.0";
    context.hydraSeatBuild = "770cac0";
    context.windowsBuildClass = "win10-2009";
    context.architecture = "x64";
    context.scenario = Scenario::SameGameTwoInstance;
    context.setupRevision = 12u;
    context.backends = {
        {"raw-input", "v1", EvidenceStatus::Pass},
        {"xinput", "1", EvidenceStatus::Pass},
    };
    context.provenanceId = "phase5-session-metrics";
    context.provenanceRevision = 4u;
    return context;
}

void testPhase5SessionMetricsAdapter() {
    CompatibilityResult result;
    check(buildCompatibilityResultFromSessionMetrics(localContext(), localMetrics(), result)
              .succeeded(),
          "privacy-safe Phase 5 session metrics convert into public compatibility evidence");
    check(result.launch == EvidenceStatus::Pass &&
              result.secondInstance == EvidenceStatus::Pass &&
              result.inputIsolation == EvidenceStatus::Pass &&
              result.controller == EvidenceStatus::Pass &&
              result.audio == EvidenceStatus::Unsupported &&
              result.cleanExit == EvidenceStatus::Pass &&
              result.rollback == EvidenceStatus::Pass,
          "local technical outcomes retain typed measured semantics");
    check(result.measurements.observedInputEvents == 500u &&
              result.measurements.verifiedCrossSeatEvents == 0u &&
              result.measurements.inputLatencyP95Micros == 1800u,
          "receiver/input count and latency evidence are retained without raw input data");

    std::string json;
    check(encodeCompatibilityResultJson(result, json).succeeded(),
          "converted local evidence encodes as public result");
    check(json.find("Alice") == std::string::npos &&
              json.find("C:\\\\") == std::string::npos &&
              json.find("opaque-account") == std::string::npos &&
              json.find("player_names_excluded\":true") != std::string::npos &&
              json.find("raw_typed_text_excluded\":true") != std::string::npos,
          "public JSON declares redaction while carrying no sampled Player/path/account content values");

    auto running = localMetrics();
    running.finalState = metrics::SessionFinalState::Running;
    running.rollbackAttempted = false;
    running.rollbackVerified = false;
    running.maximumStopDurationMicros = 0u;
    running.maximumRollbackDurationMicros = 0u;
    CompatibilityResult incomplete;
    check(buildCompatibilityResultFromSessionMetrics(localContext(), running, incomplete)
              .succeeded() &&
              incomplete.cleanExit == EvidenceStatus::NotMeasured &&
              !incomplete.measurements.stopDurationMicros &&
              incomplete.rollback == EvidenceStatus::NotMeasured &&
              !incomplete.measurements.rollbackDurationMicros,
          "unperformed exit/rollback remain explicitly NotMeasured/null");
}

void testInvalidLocalEvidenceDoesNotReplaceOutput() {
    auto report = localMetrics();
    report.planFingerprint = 0u;
    CompatibilityResult output;
    output.resultId = "sentinel";
    const auto sentinel = output;
    check(buildCompatibilityResultFromSessionMetrics(localContext(), report, output).code ==
              CompatibilityResultCode::InvalidLocalEvidence &&
              output == sentinel,
          "invalid local evidence source leaves previous public result unchanged");
}

} // namespace

int main() {
    testCanonicalRoundTripAndOrder();
    testFutureUnknownAndMalformedFailTransactionally();
    testPrivacyAndMeasurementContracts();
    testPhase5SessionMetricsAdapter();
    testInvalidLocalEvidenceDoesNotReplaceOutput();
    if (failures != 0) {
        std::cerr << failures << " compatibility result test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Compatibility result schema tests passed.\n";
    return EXIT_SUCCESS;
}
