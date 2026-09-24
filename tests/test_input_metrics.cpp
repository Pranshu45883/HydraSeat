#include "hydra/input_metrics.hpp"
#include "hydra/input_router.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace hydra;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

InputMetricSample sample(std::uint64_t correlation,
                         InputMetricStage stage,
                         std::uint64_t timestamp,
                         std::uint32_t seat = 1u,
                         std::uint32_t process = 100u) {
    InputMetricSample value;
    value.correlationId = correlation;
    value.stage = stage;
    value.timestampMicros = timestamp;
    value.expectedSeatId = seat;
    value.receivingSeatId = seat;
    value.targetProcessId = process;
    value.receivingProcessId = process;
    value.eventClass = InputMetricEventClass::Key;
    value.detailCode = 0x41u;
    return value;
}

void appendCompleteEvent(InputMetricsSnapshot& snapshot,
                         std::uint64_t correlation,
                         std::uint64_t base,
                         std::uint64_t endToEndLatency,
                         InputMetricEventClass eventClass =
                             InputMetricEventClass::Key) {
    const std::uint64_t enqueue = base + 10u;
    const std::uint64_t dequeue = base + 20u;
    const std::uint64_t write = base + 30u;
    const std::uint64_t apply = base + 40u;
    const std::uint64_t query = base + endToEndLatency;
    auto add = [&](InputMetricStage stage, std::uint64_t timestamp) {
        auto value = sample(correlation, stage, timestamp);
        value.eventClass = eventClass;
        snapshot.samples.push_back(value);
    };
    add(InputMetricStage::PhysicalObserved, base);
    add(InputMetricStage::RouteEnqueued, enqueue);
    add(InputMetricStage::RouteDequeued, dequeue);
    add(InputMetricStage::RouteWritten, write);
    add(InputMetricStage::TargetApplied, apply);
    add(InputMetricStage::TargetQueried, query);
}

void testRecorderBoundsRotationAndPrivacy() {
    InputMetricsRecorder recorder(3u, InputMetricsPrivacyMode::Redacted);
    for (std::uint64_t index = 1u; index <= 5u; ++index) {
        auto value = sample(index, InputMetricStage::PhysicalObserved,
                            100u + index);
        value.detailCode = 0x40u + static_cast<std::uint32_t>(index);
        check(recorder.tryRecord(value), "bounded recorder accepts valid sample");
    }
    const auto snapshot = recorder.snapshot();
    check(snapshot.capacity == 3u && snapshot.samples.size() == 3u,
          "recorder keeps a fixed-capacity trace");
    check(snapshot.acceptedSamples == 5u && snapshot.rotationDrops == 2u,
          "rotated samples are counted explicitly");
    check(snapshot.samples.front().correlationId == 3u &&
              snapshot.samples.back().correlationId == 5u,
          "ring rotation retains the newest samples in insertion order");
    check(snapshot.samples.front().detailCode == 0u &&
              snapshot.samples.back().detailCode == 0u,
          "redacted privacy mode removes key/button identity before storage");

    InputMetricsRecorder diagnostic(2u, InputMetricsPrivacyMode::Diagnostic);
    auto diagnosticValue = sample(9u, InputMetricStage::PhysicalObserved, 900u);
    diagnosticValue.detailCode = 0x5au;
    check(diagnostic.tryRecord(diagnosticValue) &&
              diagnostic.snapshot().samples.front().detailCode == 0x5au,
          "diagnostic privacy mode may retain explicit input identity");

    auto invalid = sample(10u, InputMetricStage::PhysicalObserved, 0u);
    check(!diagnostic.tryRecord(invalid) &&
              diagnostic.snapshot().invalidSamples == 1u,
          "zero monotonic timestamp is rejected and counted");
}

void testEventClassification() {
    RawInputEvent event;
    event.vkey = 0x41u;
    event.mouseButtons = 0x0001u;
    event.deltaX = 4;
    event.deltaY = -2;
    event.messageType = 0x020Au; // WM_MOUSEWHEEL
    const auto classes = classifyInputMetricEvent(event);
    check(hasInputMetricEventClass(classes, InputMetricEventClass::Key) &&
              hasInputMetricEventClass(classes, InputMetricEventClass::Button) &&
              hasInputMetricEventClass(classes, InputMetricEventClass::Movement) &&
              hasInputMetricEventClass(classes, InputMetricEventClass::Wheel),
          "one raw event preserves every applicable event class bit");
    check(inputMetricDetailCode(event) == 0x41u,
          "key identity is the diagnostic detail when a key transition exists");
}

void testDeterministicPercentileReport() {
    InputMetricsSnapshot snapshot;
    appendCompleteEvent(snapshot, 1u, 1000u, 50u,
                        InputMetricEventClass::Key);
    appendCompleteEvent(snapshot, 2u, 2000u, 100u,
                        InputMetricEventClass::Button);
    appendCompleteEvent(snapshot, 3u, 3000u, 150u,
                        InputMetricEventClass::Movement);
    appendCompleteEvent(snapshot, 4u, 4000u, 200u,
                        InputMetricEventClass::Wheel);

    snapshot.samples[1].queueDepth = 2u;
    snapshot.samples[1].queueHighWater = 5u;
    snapshot.samples[1].queueDroppedCount = 3u;
    snapshot.rotationDrops = 7u;
    snapshot.contentionDrops = 2u;

    InputMetricSample rollbackStart;
    rollbackStart.correlationId = 99u;
    rollbackStart.stage = InputMetricStage::RollbackStarted;
    rollbackStart.timestampMicros = 5000u;
    snapshot.samples.push_back(rollbackStart);
    auto rollbackDone = rollbackStart;
    rollbackDone.stage = InputMetricStage::RollbackCompleted;
    rollbackDone.timestampMicros = 5250u;
    snapshot.samples.push_back(rollbackDone);

    InputMetricSample resource;
    resource.stage = InputMetricStage::HostResourceSample;
    resource.timestampMicros = 5300u;
    resource.hostCpuTimeMicros = 12345u;
    resource.hostWorkingSetBytes = 64u * 1024u * 1024u;
    snapshot.samples.push_back(resource);

    InputMetricsReport report;
    check(buildInputMetricsReport(snapshot, report) ==
              InputMetricsReportResult::Success,
          "deterministic fixture builds a report");
    check(report.uniqueInputEvents == 4u &&
              report.completeInputEvents == 4u &&
              report.missingStageEvents == 0u &&
              report.receiverVerifiedEvents == 4u &&
              report.missingReceiverEvidenceEvents == 0u,
          "complete fixture counts four correlated input events with verified receiver identity");
    check(report.endToEnd.count == 4u && report.endToEnd.p50Micros == 100u &&
              report.endToEnd.p95Micros == 200u &&
              report.endToEnd.p99Micros == 200u &&
              report.endToEnd.maxMicros == 200u,
          "nearest-rank p50/p95/p99/max are deterministic");
    check(report.keyEvents == 1u && report.buttonEvents == 1u &&
              report.movementEvents == 1u && report.wheelEvents == 1u,
          "event-class counters preserve key/button/movement/wheel classes");
    check(report.queueHighWater == 5u && report.queueDroppedFrames == 3u &&
              report.recorderRotationDrops == 7u &&
              report.recorderContentionDrops == 2u,
          "queue and recorder loss counters survive report generation");
    check(report.rollback.count == 1u && report.rollback.p50Micros == 250u,
          "rollback duration uses the same monotonic latency contract");
    check(report.resourceSamples == 1u &&
              report.lastHostCpuTimeMicros == 12345u &&
              report.maxHostWorkingSetBytes == 64u * 1024u * 1024u,
          "resource sample hooks are summarized without polling the OS");

    const auto json = encodeInputMetricsReportJson(report);
    check(json.find("\"p50_us\":100") != std::string::npos &&
              json.find("\"receiver_verified_events\":4") != std::string::npos &&
              json.find("\"missing_receiver_evidence_events\":0") != std::string::npos &&
              json.find("\"cross_seat_events\":0") != std::string::npos &&
              json.find("\"dropped_frames\":3") != std::string::npos,
          "machine-readable JSON report exposes receiver evidence and stable numeric fields");
}

void testBleedAndMissingStages() {
    InputMetricsSnapshot snapshot;
    auto physical = sample(11u, InputMetricStage::PhysicalObserved, 100u, 1u, 101u);
    physical.eventClass = InputMetricEventClass::Movement;
    snapshot.samples.push_back(physical);
    auto applied = sample(11u, InputMetricStage::TargetApplied, 130u, 1u, 101u);
    applied.receivingSeatId = 2u;
    applied.receivingProcessId = 202u;
    applied.eventClass = InputMetricEventClass::Movement;
    snapshot.samples.push_back(applied);

    InputMetricsReport report;
    check(buildInputMetricsReport(snapshot, report) ==
              InputMetricsReportResult::Success,
          "partial trace remains reportable instead of fabricating stages");
    check(report.uniqueInputEvents == 1u && report.completeInputEvents == 0u &&
              report.missingStageEvents == 1u &&
              report.receiverVerifiedEvents == 0u &&
              report.missingReceiverEvidenceEvents == 1u &&
              report.crossSeatEvents == 1u && report.crossProcessEvents == 1u,
          "missing target query evidence stays unverified while observed receiver bleed remains explicit");
    check(report.endToEnd.count == 0u,
          "missing target query never fabricates an end-to-end latency");
}

void testRouteIntentDoesNotMasqueradeAsReceiverEvidence() {
    InputMetricsSnapshot snapshot;
    auto observed = sample(12u, InputMetricStage::PhysicalObserved, 100u, 1u, 101u);
    observed.receivingSeatId = 0u;
    observed.receivingProcessId = 0u;
    snapshot.samples.push_back(observed);
    auto enqueued = sample(12u, InputMetricStage::RouteEnqueued, 110u, 1u, 101u);
    enqueued.receivingSeatId = 0u;
    enqueued.receivingProcessId = 0u;
    snapshot.samples.push_back(enqueued);
    auto written = sample(12u, InputMetricStage::RouteWritten, 120u, 1u, 101u);
    written.receivingSeatId = 0u;
    written.receivingProcessId = 0u;
    snapshot.samples.push_back(written);

    InputMetricsReport report;
    check(buildInputMetricsReport(snapshot, report) ==
              InputMetricsReportResult::Success &&
              report.uniqueInputEvents == 1u &&
              report.receiverVerifiedEvents == 0u &&
              report.missingReceiverEvidenceEvents == 1u &&
              report.crossSeatEvents == 0u && report.crossProcessEvents == 0u,
          "host routing intent remains unverified until target apply/query receiver identity is observed");
}

void testCompleteStagesStillRequireBothReceiverIdentities() {
    InputMetricsSnapshot snapshot;
    appendCompleteEvent(snapshot, 13u, 100u, 100u);
    auto& queried = snapshot.samples.back();
    queried.receivingSeatId = 0u;
    queried.receivingProcessId = 0u;

    InputMetricsReport report;
    check(buildInputMetricsReport(snapshot, report) ==
              InputMetricsReportResult::Success &&
              report.completeInputEvents == 1u &&
              report.receiverVerifiedEvents == 0u &&
              report.missingReceiverEvidenceEvents == 1u &&
              report.crossSeatEvents == 0u && report.crossProcessEvents == 0u,
          "complete stage timing cannot hide a missing target query receiver identity");
}

void testTimestampOrderingAndWrapFailClosed() {
    InputMetricsSnapshot outOfOrder;
    outOfOrder.samples.push_back(
        sample(1u, InputMetricStage::PhysicalObserved, 200u));
    outOfOrder.samples.push_back(
        sample(1u, InputMetricStage::RouteEnqueued, 199u));
    InputMetricsReport report;
    check(buildInputMetricsReport(outOfOrder, report) ==
              InputMetricsReportResult::TimestampOrderViolation,
          "negative stage latency is rejected instead of unsigned underflow");

    InputMetricsSnapshot wrapped;
    wrapped.samples.push_back(sample(
        2u, InputMetricStage::PhysicalObserved,
        (std::numeric_limits<std::uint64_t>::max)() - 2u));
    wrapped.samples.push_back(
        sample(2u, InputMetricStage::RouteEnqueued, 3u));
    check(buildInputMetricsReport(wrapped, report) ==
              InputMetricsReportResult::TimestampOrderViolation,
          "monotonic clock wrap/order discontinuity fails closed");

    InputMetricsSnapshot zeroTimestamp;
    zeroTimestamp.samples.push_back(
        sample(3u, InputMetricStage::PhysicalObserved, 0u));
    check(buildInputMetricsReport(zeroTimestamp, report) ==
              InputMetricsReportResult::InvalidTimestamp,
          "zero timestamp cannot enter a latency report");
}

void testDroppedRouteDoesNotFabricateEnqueueLatency() {
    InputMetricsSnapshot snapshot;
    snapshot.samples.push_back(
        sample(21u, InputMetricStage::PhysicalObserved, 100u, 1u, 101u));
    auto dropped = sample(21u, InputMetricStage::RouteDropped, 120u, 1u, 101u);
    dropped.queueDepth = 2048u;
    dropped.queueHighWater = 2048u;
    dropped.queueDroppedCount = 1u;
    snapshot.samples.push_back(dropped);

    InputMetricsReport report;
    check(buildInputMetricsReport(snapshot, report) ==
              InputMetricsReportResult::Success &&
              report.uniqueInputEvents == 1u &&
              report.missingStageEvents == 1u &&
              report.observationToEnqueue.count == 0u &&
              report.queueDroppedFrames == 1u &&
              report.queueHighWater == 2048u,
          "route drop is reported without pretending the event was enqueued");
}

void testQueueDropsAreSummedPerTargetQueueWithoutDoubleCounting() {
    InputMetricsSnapshot snapshot;
    auto seat1a = sample(1u, InputMetricStage::RouteEnqueued, 100u, 1u, 101u);
    seat1a.queueDroppedCount = 2u;
    seat1a.queueHighWater = 10u;
    snapshot.samples.push_back(seat1a);
    auto seat1b = sample(2u, InputMetricStage::RouteEnqueued, 110u, 1u, 101u);
    seat1b.queueDroppedCount = 5u;
    snapshot.samples.push_back(seat1b);
    auto seat1SecondProcess =
        sample(3u, InputMetricStage::RouteEnqueued, 115u, 1u, 102u);
    seat1SecondProcess.queueDroppedCount = 3u;
    snapshot.samples.push_back(seat1SecondProcess);
    auto seat2 = sample(4u, InputMetricStage::RouteEnqueued, 120u, 2u, 202u);
    seat2.queueDroppedCount = 4u;
    seat2.queueHighWater = 7u;
    snapshot.samples.push_back(seat2);

    InputMetricsReport report;
    check(buildInputMetricsReport(snapshot, report) ==
              InputMetricsReportResult::Success &&
              report.queueDroppedFrames == 12u &&
              report.queueHighWater == 10u,
          "cumulative queue drops use each Seat/target-process queue maximum exactly once");
}

} // namespace

int main() {
    testRecorderBoundsRotationAndPrivacy();
    testEventClassification();
    testDeterministicPercentileReport();
    testBleedAndMissingStages();
    testRouteIntentDoesNotMasqueradeAsReceiverEvidence();
    testCompleteStagesStillRequireBothReceiverIdentities();
    testTimestampOrderingAndWrapFailClosed();
    testDroppedRouteDoesNotFabricateEnqueueLatency();
    testQueueDropsAreSummedPerTargetQueueWithoutDoubleCounting();
    std::cout << "Input metrics tests passed\n";
    return EXIT_SUCCESS;
}
