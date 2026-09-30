#include "hydra/local_compatibility_evidence.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace hydra;
using namespace hydra::compat;
using namespace hydra::community;
using namespace hydra::metrics;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class TempRoot final {
public:
    TempRoot() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
        const auto process = static_cast<unsigned long long>(GetCurrentProcessId());
#else
        const auto process = 0ull;
#endif
        path_ = std::filesystem::temp_directory_path() /
                ("hydra-local-compat-evidence-" + std::to_string(process) + "-" +
                 std::to_string(static_cast<unsigned long long>(stamp)));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        check(!error, "temporary local compatibility root can be created");
    }

    ~TempRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

InputMetricSample sample(std::uint64_t correlation,
                         InputMetricStage stage,
                         std::uint64_t timestamp,
                         std::uint32_t expectedSeat,
                         std::uint32_t expectedProcess) {
    InputMetricSample value;
    value.correlationId = correlation;
    value.stage = stage;
    value.timestampMicros = timestamp;
    value.expectedSeatId = expectedSeat;
    value.targetProcessId = expectedProcess;
    value.eventClass = InputMetricEventClass::Key;
    if (stage == InputMetricStage::TargetApplied ||
        stage == InputMetricStage::TargetQueried) {
        value.receivingSeatId = expectedSeat;
        value.receivingProcessId = expectedProcess;
    }
    return value;
}

void appendCompleteEvent(InputMetricsSnapshot& snapshot,
                         std::uint64_t correlation,
                         std::uint64_t base,
                         std::uint32_t seat,
                         std::uint32_t process) {
    snapshot.samples.push_back(sample(correlation, InputMetricStage::PhysicalObserved,
                                      base, seat, process));
    snapshot.samples.push_back(sample(correlation, InputMetricStage::RouteEnqueued,
                                      base + 10u, seat, process));
    snapshot.samples.push_back(sample(correlation, InputMetricStage::RouteDequeued,
                                      base + 20u, seat, process));
    snapshot.samples.push_back(sample(correlation, InputMetricStage::RouteWritten,
                                      base + 30u, seat, process));
    snapshot.samples.push_back(sample(correlation, InputMetricStage::TargetApplied,
                                      base + 40u, seat, process));
    snapshot.samples.push_back(sample(correlation, InputMetricStage::TargetQueried,
                                      base + 60u, seat, process));
}

SeatSessionMetrics seat(SeatId seatId, std::uint64_t base) {
    SeatSessionMetrics result;
    result.seatId = seatId;
    result.launchDurationMicros = base + 100u;
    result.stopDurationMicros = base + 200u;
    result.rollbackDurationMicros = base + 300u;
    result.processStarted = true;
    result.windowOwnershipVerified = true;
    result.displayPlacementVerified = true;
    result.inputRouteReady = true;
    result.controller = CapabilityOutcome::Success;
    result.audio = CapabilityOutcome::Success;
    return result;
}

SessionMetricsReport completedReport(EvidenceOrigin origin) {
    SessionMetricsBuildInput input;
    input.planFingerprint = 0xA501u;
    input.origin = origin;
    appendCompleteEvent(input.input, 1u, 1000u, 1u, 101u);
    appendCompleteEvent(input.input, 2u, 2000u, 2u, 202u);
    input.seats = {seat(2u, 2000u), seat(1u, 1000u)};
    input.finalState = SessionFinalState::ReturnedToWindows;
    input.rollbackAttempted = true;
    input.rollbackVerified = true;

    SessionMetricsReport report;
    check(buildSessionMetricsReport(input, report) == SessionMetricsResult::Success,
          "completed session metrics fixture builds through the canonical metrics builder");
    return report;
}

LocalEvidenceContext context(std::string resultId) {
    LocalEvidenceContext value;
    value.resultId = std::move(resultId);
    value.timestampClass = TimestampClass::MonthBucket;
    value.timestampBucket = "2026-09";
    value.gameId = "game:local-check";
    value.providerId = "custom";
    value.providerAppId = "local-app";
    value.gameVersion = "1.0";
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "test-build";
    value.windowsBuildClass = "win10-2009";
    value.architecture = "x64";
    value.scenario = Scenario::SameGameTwoInstance;
    value.setupRevision = 7u;
    value.backends = {
        {"raw-input", "v1", EvidenceStatus::Pass},
        {"xinput", "v1", EvidenceStatus::Pass},
    };
    value.provenanceId = "local-session-metrics";
    value.provenanceRevision = 3u;
    return value;
}

CompatibilityResult canonicalResult(std::string resultId,
                                    const SessionMetricsReport& report) {
    CompatibilityResult result;
    const auto converted = buildCompatibilityResultFromSessionMetrics(
        context(std::move(resultId)), report, result);
    check(converted.succeeded(), "seed compatibility result builds through canonical conversion");
    return result;
}

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "compatibility history fixture can be opened");
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void writeBytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "compatibility history fixture can be opened for raw corruption test");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    check(static_cast<bool>(output), "compatibility history corruption fixture can be written");
}

CompatibilityShareModel loadHistory(const std::filesystem::path& path) {
    CompatibilityShareModel model;
    CompatibilityLocalStore store(path);
    const auto loaded = store.load(model);
    check(loaded.succeeded() && loaded.found(), "persisted compatibility history reloads through existing store");
    return model;
}

void testControlledResultIsCanonicalAndPersistedAtExactInjectedPath() {
    TempRoot root;
    const auto storePath = root.path() / "nested" / "chosen-history.jsonl";
    const auto report = completedReport(EvidenceOrigin::ControlledProcess);
    check(!report.physicalValidationEligible,
          "controlled metrics fixture never claims physical validation eligibility");

    const auto written = writeLocalCompatibilityEvidence(
        context("controlled-1"), report, storePath);
    if (!written.succeeded()) {
        std::cerr << "controlled writer diagnostic: "
                  << localCompatibilityEvidenceWriteCodeName(written.diagnostic.code)
                  << ": " << written.diagnostic.message << '\n';
    }
    check(written.succeeded() && written.result.has_value(),
          "controlled session evidence persists successfully");
    check(written.result && written.result->origin == ResultOrigin::ControlledProcess,
          "ControlledProcess metrics stay ControlledProcess after canonical conversion");
    check(written.result && written.result->launch == EvidenceStatus::Pass &&
              written.result->secondInstance == EvidenceStatus::Pass &&
              written.result->inputIsolation == EvidenceStatus::Pass &&
              written.result->controller == EvidenceStatus::Pass &&
              written.result->audio == EvidenceStatus::Pass &&
              written.result->cleanExit == EvidenceStatus::Pass &&
              written.result->rollback == EvidenceStatus::Pass,
          "technical statuses are derived from session metrics without caller overrides");
    check(std::filesystem::exists(storePath),
          "writer uses the exact explicitly injected store path");
    check(!std::filesystem::exists(root.path() / "compatibility-results.jsonl"),
          "writer does not construct or escape to another store path around the injected path");

    const auto model = loadHistory(storePath);
    check(model.history().size() == 1u && model.active() != nullptr && written.result &&
              model.active()->result == *written.result,
          "persisted history contains the exact canonical result returned to the caller");
}

void testPhysicalResultStaysPhysical() {
    TempRoot root;
    const auto storePath = root.path() / "physical.jsonl";
    const auto report = completedReport(EvidenceOrigin::Physical);
    check(report.physicalValidationEligible,
          "physical fixture is genuinely eligible according to the existing metrics builder");

    const auto written = writeLocalCompatibilityEvidence(
        context("physical-1"), report, storePath);
    if (!written.succeeded()) {
        std::cerr << "physical writer diagnostic: "
                  << localCompatibilityEvidenceWriteCodeName(written.diagnostic.code)
                  << ": " << written.diagnostic.message << '\n';
    }
    check(written.succeeded() && written.result &&
              written.result->origin == ResultOrigin::Physical,
          "genuine Physical metrics remain Physical without rewriting their evidence class");
    const auto model = loadHistory(storePath);
    check(model.active() != nullptr && model.active()->result.origin == ResultOrigin::Physical,
          "persisted Physical evidence retains the canonical Physical origin");
}

void testSyntheticAndInvalidEvidencePreserveExistingGoodHistory() {
    TempRoot root;
    const auto storePath = root.path() / "preserve.jsonl";
    const auto controlled = completedReport(EvidenceOrigin::ControlledProcess);
    check(writeLocalCompatibilityEvidence(context("baseline"), controlled, storePath).succeeded(),
          "baseline good history is persisted");
    const auto baselineBytes = readBytes(storePath);

    const auto synthetic = writeLocalCompatibilityEvidence(
        context("synthetic"), completedReport(EvidenceOrigin::Synthetic), storePath);
    check(synthetic.diagnostic.code == LocalCompatibilityEvidenceWriteCode::UnsupportedOrigin &&
              !synthetic.result,
          "Synthetic evidence is rejected before it can become a release-produced local result");
    check(readBytes(storePath) == baselineBytes,
          "Synthetic rejection preserves the prior good durable history exactly");

    auto invalidContext = context("invalid-context");
    invalidContext.gameId.clear();
    const auto invalidContextWrite = writeLocalCompatibilityEvidence(
        invalidContext, controlled, storePath);
    check(invalidContextWrite.diagnostic.code == LocalCompatibilityEvidenceWriteCode::ConversionFailed &&
              !invalidContextWrite.result,
          "invalid LocalEvidenceContext fails through canonical result conversion");
    check(readBytes(storePath) == baselineBytes,
          "invalid context conversion leaves prior durable history unchanged");

    SessionMetricsReport emptyReport;
    emptyReport.origin = EvidenceOrigin::ControlledProcess;
    emptyReport.finalState = SessionFinalState::ReturnedToWindows;
    const auto invalidReportWrite = writeLocalCompatibilityEvidence(
        context("invalid-report"), emptyReport, storePath);
    check(invalidReportWrite.diagnostic.code == LocalCompatibilityEvidenceWriteCode::ConversionFailed &&
              !invalidReportWrite.result,
          "invalid/empty SessionMetricsReport fails closed through canonical conversion");
    check(readBytes(storePath) == baselineBytes,
          "invalid session metrics leave prior durable history unchanged");
}

void testExistingHistorySupersessionAndDuplicateSemantics() {
    TempRoot root;
    const auto storePath = root.path() / "history.jsonl";
    const auto report = completedReport(EvidenceOrigin::ControlledProcess);

    check(writeLocalCompatibilityEvidence(context("history-old"), report, storePath).succeeded(),
          "first local result persists");
    check(writeLocalCompatibilityEvidence(context("history-new"), report, storePath).succeeded(),
          "second local result persists while preserving existing history");

    auto model = loadHistory(storePath);
    check(model.history().size() == 2u && model.active() != nullptr &&
              model.history()[0].result.resultId == "history-old" &&
              model.history()[0].state == CompatibilityShareState::Superseded &&
              model.history()[0].supersededByResultId == "history-new" &&
              model.active()->result.resultId == "history-new",
          "supersession is owned by CompatibilityShareModel and keeps the older technical result");

    const auto beforeDuplicate = readBytes(storePath);
    const auto duplicate = writeLocalCompatibilityEvidence(
        context("history-new"), report, storePath);
    check(duplicate.diagnostic.code == LocalCompatibilityEvidenceWriteCode::RecordFailed &&
              duplicate.diagnostic.shareModelCode == ShareModelCode::InvalidResult &&
              !duplicate.result,
          "repeated identical result ID follows the existing deterministic duplicate rejection");
    check(readBytes(storePath) == beforeDuplicate,
          "recordLocalResult failure does not mutate durable history");
}

void testExistingModelRetentionRemainsAuthoritative() {
    TempRoot root;
    const auto storePath = root.path() / "retention.jsonl";
    const auto report = completedReport(EvidenceOrigin::ControlledProcess);

    CompatibilityShareModel seed;
    CompatibilityPrivacySettings settings;
    settings.retainedLocalResults = kMaximumRetainedCompatibilityResults;
    check(seed.setPrivacySettings(settings).succeeded(),
          "retention fixture can temporarily keep more than the default history size");
    for (std::size_t index = 0; index < 40u; ++index) {
        check(seed.recordLocalResult(canonicalResult(
                  "retain-" + std::to_string(index), report)).succeeded(),
              "retention fixture seeds canonical local results through existing model");
    }
    CompatibilityLocalStore store(storePath);
    check(store.save(seed).succeeded(),
          "large-but-bounded existing history is saved through existing store");

    check(writeLocalCompatibilityEvidence(
              context("retain-final"), report, storePath).succeeded(),
          "writer loads the existing history then records through the default model semantics");
    const auto restored = loadHistory(storePath);
    check(restored.history().size() == kDefaultRetainedCompatibilityResults &&
              restored.history().front().result.resultId == "retain-9" &&
              restored.active() != nullptr && restored.active()->result.resultId == "retain-final",
          "retention pruning comes from CompatibilityShareModel rather than writer-side reimplementation");
}

void testCorruptStoreAndSaveFailurePreserveDurableBytes() {
    TempRoot root;
    const auto corruptPath = root.path() / "corrupt.jsonl";
    const std::string corruptBytes = "not-a-valid-compatibility-history\n";
    writeBytes(corruptPath, corruptBytes);

    const auto corruptWrite = writeLocalCompatibilityEvidence(
        context("corrupt-new"), completedReport(EvidenceOrigin::ControlledProcess), corruptPath);
    check(corruptWrite.diagnostic.code == LocalCompatibilityEvidenceWriteCode::StoreLoadFailed &&
              !corruptWrite.result,
          "corrupt existing compatibility store fails closed during existing-store load");
    check(readBytes(corruptPath) == corruptBytes,
          "corrupt existing store is never overwritten by the writer");

    const auto storePath = root.path() / "save-failure.jsonl";
    const auto report = completedReport(EvidenceOrigin::ControlledProcess);
    check(writeLocalCompatibilityEvidence(context("save-good"), report, storePath).succeeded(),
          "save failure fixture first creates good durable history");
    const auto goodBytes = readBytes(storePath);

    auto stagingBlocker = storePath;
    stagingBlocker += L".tmp";
    std::error_code error;
    std::filesystem::create_directory(stagingBlocker, error);
    check(!error, "test can create a staging-path directory blocker");
    std::ofstream blocker(stagingBlocker / "keep.txt", std::ios::binary | std::ios::trunc);
    blocker << "keep directory non-empty";
    blocker.close();

    const auto failedSave = writeLocalCompatibilityEvidence(
        context("save-new"), report, storePath);
    check(failedSave.diagnostic.code == LocalCompatibilityEvidenceWriteCode::StoreSaveFailed &&
              failedSave.diagnostic.storeCode == CompatibilityLocalStoreCode::CleanupFailed &&
              !failedSave.result,
          "existing store staging precondition produces a typed save failure");
    check(readBytes(storePath) == goodBytes,
          "failed staged/atomic save preserves the exact previous good durable history");
}

} // namespace

int main() {
    testControlledResultIsCanonicalAndPersistedAtExactInjectedPath();
    testPhysicalResultStaysPhysical();
    testSyntheticAndInvalidEvidencePreserveExistingGoodHistory();
    testExistingHistorySupersessionAndDuplicateSemantics();
    testExistingModelRetentionRemainsAuthoritative();
    testCorruptStoreAndSaveFailurePreserveDurableBytes();

    if (failures != 0) {
        std::cerr << failures << " local compatibility evidence test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Local compatibility evidence writer tests passed.\n";
    return EXIT_SUCCESS;
}
