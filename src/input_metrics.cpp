#include "hydra/input_metrics.hpp"

#include "hydra/input_router.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace hydra {
namespace {

constexpr std::size_t kPrimaryStageCount = 6;

std::optional<std::size_t> primaryStageIndex(InputMetricStage stage) noexcept {
    switch (stage) {
    case InputMetricStage::PhysicalObserved: return 0;
    case InputMetricStage::RouteEnqueued: return 1;
    case InputMetricStage::RouteDequeued: return 2;
    case InputMetricStage::RouteWritten: return 3;
    case InputMetricStage::TargetApplied: return 4;
    case InputMetricStage::TargetQueried: return 5;
    case InputMetricStage::RouteDropped:
    case InputMetricStage::RollbackStarted:
    case InputMetricStage::RollbackCompleted:
    case InputMetricStage::HostResourceSample:
        return std::nullopt;
    }
    return std::nullopt;
}

bool validStage(InputMetricStage stage) noexcept {
    return stage >= InputMetricStage::PhysicalObserved &&
           stage <= InputMetricStage::HostResourceSample;
}

struct EventAccumulator {
    std::array<std::optional<std::uint64_t>, kPrimaryStageCount> timestamps{};
    InputMetricEventClass eventClass{InputMetricEventClass::None};
    bool targetAppliedReceiverVerified{false};
    bool targetQueriedReceiverVerified{false};
    bool crossSeat{false};
    bool crossProcess{false};
    bool inputStageSeen{false};
};

struct RollbackAccumulator {
    std::optional<std::uint64_t> start;
    std::optional<std::uint64_t> complete;
};

void addLatency(std::vector<std::uint64_t>& values,
                std::uint64_t start,
                std::uint64_t end) {
    values.push_back(end - start);
}

InputLatencyPercentiles percentiles(std::vector<std::uint64_t> values) {
    InputLatencyPercentiles result;
    if (values.empty()) {
        return result;
    }
    std::sort(values.begin(), values.end());
    result.count = static_cast<std::uint64_t>(values.size());
    const auto nearestRankIndex = [&](std::size_t percent) {
        const std::size_t rank =
            (values.size() * percent + 99u) / 100u;
        return (std::max<std::size_t>)(rank, 1u) - 1u;
    };
    result.p50Micros = values[nearestRankIndex(50u)];
    result.p95Micros = values[nearestRankIndex(95u)];
    result.p99Micros = values[nearestRankIndex(99u)];
    result.maxMicros = values.back();
    return result;
}

void appendPercentilesJson(std::ostringstream& out,
                           std::string_view name,
                           const InputLatencyPercentiles& value) {
    out << '\"' << name << "\":{";
    out << "\"count\":" << value.count;
    out << ",\"p50_us\":" << value.p50Micros;
    out << ",\"p95_us\":" << value.p95Micros;
    out << ",\"p99_us\":" << value.p99Micros;
    out << ",\"max_us\":" << value.maxMicros << '}';
}

} // namespace

InputMetricsRecorder::InputMetricsRecorder(
    std::size_t capacity,
    InputMetricsPrivacyMode privacyMode)
    : m_privacyMode(privacyMode),
      m_ring((std::max<std::size_t>)(
          1u, (std::min)(capacity, kMaxInputMetricsCapacity))) {}

bool InputMetricsRecorder::tryLock() const noexcept {
    return !m_guard.test_and_set(std::memory_order_acquire);
}

void InputMetricsRecorder::unlock() const noexcept {
    m_guard.clear(std::memory_order_release);
}

bool InputMetricsRecorder::tryRecord(InputMetricSample sample) noexcept {
    if (sample.schemaVersion != kInputMetricsSchemaVersion ||
        !validStage(sample.stage) || sample.timestampMicros == 0u ||
        (sample.correlationId == 0u &&
         sample.stage != InputMetricStage::HostResourceSample)) {
        m_invalidSamples.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    if (!tryLock()) {
        m_contentionDrops.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    if (m_privacyMode == InputMetricsPrivacyMode::Redacted) {
        sample.detailCode = 0u;
    }

    if (m_size < m_ring.size()) {
        const std::size_t index = (m_start + m_size) % m_ring.size();
        m_ring[index] = sample;
        ++m_size;
    } else {
        m_ring[m_start] = sample;
        m_start = (m_start + 1u) % m_ring.size();
        m_rotationDrops.fetch_add(1u, std::memory_order_relaxed);
    }
    ++m_acceptedSamples;
    unlock();
    return true;
}

InputMetricsSnapshot InputMetricsRecorder::snapshot() const {
    while (!tryLock()) {
        std::this_thread::yield();
    }

    InputMetricsSnapshot result;
    result.privacyMode = m_privacyMode;
    result.capacity = m_ring.size();
    result.acceptedSamples = m_acceptedSamples;
    result.rotationDrops = m_rotationDrops.load(std::memory_order_relaxed);
    result.contentionDrops = m_contentionDrops.load(std::memory_order_relaxed);
    result.invalidSamples = m_invalidSamples.load(std::memory_order_relaxed);
    result.samples.reserve(m_size);
    for (std::size_t offset = 0; offset < m_size; ++offset) {
        result.samples.push_back(m_ring[(m_start + offset) % m_ring.size()]);
    }
    unlock();
    return result;
}

void InputMetricsRecorder::reset() noexcept {
    while (!tryLock()) {
        std::this_thread::yield();
    }
    m_start = 0u;
    m_size = 0u;
    m_acceptedSamples = 0u;
    m_rotationDrops.store(0u, std::memory_order_relaxed);
    m_contentionDrops.store(0u, std::memory_order_relaxed);
    m_invalidSamples.store(0u, std::memory_order_relaxed);
    unlock();
}

std::uint64_t monotonicInputMetricTimestampMicros() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return micros <= 0 ? 1u : static_cast<std::uint64_t>(micros);
}

InputMetricEventClass classifyInputMetricEvent(
    const RawInputEvent& event) noexcept {
    constexpr std::uint32_t kMouseWheelMessage = 0x020Au;
    constexpr std::uint32_t kMouseHorizontalWheelMessage = 0x020Eu;

    InputMetricEventClass result = InputMetricEventClass::None;
    if (event.vkey != 0u) {
        result |= InputMetricEventClass::Key;
    }
    if (event.mouseButtons != 0u) {
        result |= InputMetricEventClass::Button;
    }
    if (event.deltaX != 0 || event.deltaY != 0) {
        result |= InputMetricEventClass::Movement;
    }
    if (event.messageType == kMouseWheelMessage ||
        event.messageType == kMouseHorizontalWheelMessage) {
        result |= InputMetricEventClass::Wheel;
    }
    return result;
}

std::uint32_t inputMetricDetailCode(const RawInputEvent& event) noexcept {
    if (event.vkey != 0u) {
        return event.vkey;
    }
    if (event.mouseButtons != 0u) {
        return event.mouseButtons;
    }
    return 0u;
}

std::string_view inputMetricStageName(InputMetricStage stage) noexcept {
    switch (stage) {
    case InputMetricStage::PhysicalObserved: return "physical_observed";
    case InputMetricStage::RouteEnqueued: return "route_enqueued";
    case InputMetricStage::RouteDequeued: return "route_dequeued";
    case InputMetricStage::RouteWritten: return "route_written";
    case InputMetricStage::TargetApplied: return "target_applied";
    case InputMetricStage::TargetQueried: return "target_queried";
    case InputMetricStage::RouteDropped: return "route_dropped";
    case InputMetricStage::RollbackStarted: return "rollback_started";
    case InputMetricStage::RollbackCompleted: return "rollback_completed";
    case InputMetricStage::HostResourceSample: return "host_resource_sample";
    }
    return "unknown";
}

std::string_view inputMetricsReportResultName(
    InputMetricsReportResult result) noexcept {
    switch (result) {
    case InputMetricsReportResult::Success: return "Success";
    case InputMetricsReportResult::EmptyTrace: return "EmptyTrace";
    case InputMetricsReportResult::UnsupportedSchema: return "UnsupportedSchema";
    case InputMetricsReportResult::InvalidTimestamp: return "InvalidTimestamp";
    case InputMetricsReportResult::TimestampOrderViolation:
        return "TimestampOrderViolation";
    }
    return "Unknown";
}

InputMetricsReportResult buildInputMetricsReport(
    const InputMetricsSnapshot& snapshot,
    InputMetricsReport& report) {
    report = {};
    if (snapshot.schemaVersion != kInputMetricsSchemaVersion) {
        return InputMetricsReportResult::UnsupportedSchema;
    }
    if (snapshot.samples.empty()) {
        return InputMetricsReportResult::EmptyTrace;
    }

    std::map<std::uint64_t, EventAccumulator> events;
    std::map<std::uint64_t, RollbackAccumulator> rollbacks;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t>
        queueDropsByTarget;
    std::vector<std::uint64_t> observationToEnqueue;
    std::vector<std::uint64_t> enqueueToDequeue;
    std::vector<std::uint64_t> dequeueToWrite;
    std::vector<std::uint64_t> writeToApply;
    std::vector<std::uint64_t> applyToQuery;
    std::vector<std::uint64_t> endToEnd;
    std::vector<std::uint64_t> rollbackDurations;

    report.traceSamples = static_cast<std::uint64_t>(snapshot.samples.size());
    report.recorderRotationDrops = snapshot.rotationDrops;
    report.recorderContentionDrops = snapshot.contentionDrops;
    report.recorderInvalidSamples = snapshot.invalidSamples;

    for (const auto& sample : snapshot.samples) {
        if (sample.schemaVersion != kInputMetricsSchemaVersion ||
            !validStage(sample.stage)) {
            return InputMetricsReportResult::UnsupportedSchema;
        }
        if (sample.timestampMicros == 0u) {
            return InputMetricsReportResult::InvalidTimestamp;
        }
        report.queueHighWater =
            (std::max)(report.queueHighWater, sample.queueHighWater);
        if (sample.expectedSeatId != 0u || sample.targetProcessId != 0u) {
            const auto queueKey =
                std::pair{sample.expectedSeatId, sample.targetProcessId};
            auto& dropped = queueDropsByTarget[queueKey];
            dropped = (std::max)(dropped, sample.queueDroppedCount);
        }

        if (sample.stage == InputMetricStage::HostResourceSample) {
            ++report.resourceSamples;
            report.lastHostCpuTimeMicros = sample.hostCpuTimeMicros;
            report.maxHostWorkingSetBytes =
                (std::max)(report.maxHostWorkingSetBytes,
                           sample.hostWorkingSetBytes);
            continue;
        }
        if (sample.correlationId == 0u) {
            return InputMetricsReportResult::UnsupportedSchema;
        }
        if (sample.stage == InputMetricStage::RollbackStarted ||
            sample.stage == InputMetricStage::RollbackCompleted) {
            auto& rollback = rollbacks[sample.correlationId];
            auto& value = sample.stage == InputMetricStage::RollbackStarted
                              ? rollback.start
                              : rollback.complete;
            if (!value || sample.timestampMicros < *value) {
                value = sample.timestampMicros;
            }
            continue;
        }

        auto& event = events[sample.correlationId];
        event.inputStageSeen = true;
        event.eventClass |= sample.eventClass;
        const bool receiverStage =
            sample.stage == InputMetricStage::TargetApplied ||
            sample.stage == InputMetricStage::TargetQueried;
        if (receiverStage) {
            const bool seatEvidence = sample.expectedSeatId != 0u &&
                                      sample.receivingSeatId != 0u;
            const bool processEvidence = sample.targetProcessId != 0u &&
                                         sample.receivingProcessId != 0u;
            const bool receiverVerified = seatEvidence && processEvidence;
            if (sample.stage == InputMetricStage::TargetApplied) {
                event.targetAppliedReceiverVerified =
                    event.targetAppliedReceiverVerified || receiverVerified;
            } else {
                event.targetQueriedReceiverVerified =
                    event.targetQueriedReceiverVerified || receiverVerified;
            }
            if (seatEvidence && sample.expectedSeatId != sample.receivingSeatId) {
                event.crossSeat = true;
            }
            if (processEvidence &&
                sample.targetProcessId != sample.receivingProcessId) {
                event.crossProcess = true;
            }
        }
        if (const auto stageIndex = primaryStageIndex(sample.stage)) {
            auto& timestamp = event.timestamps[*stageIndex];
            if (!timestamp || sample.timestampMicros < *timestamp) {
                timestamp = sample.timestampMicros;
            }
        }
    }

    for (const auto& [queueKey, drops] : queueDropsByTarget) {
        (void)queueKey;
        report.queueDroppedFrames += drops;
    }

    for (const auto& [correlationId, event] : events) {
        (void)correlationId;
        if (!event.inputStageSeen) {
            continue;
        }
        ++report.uniqueInputEvents;
        // Zero-bleed receiver evidence is complete only when the target both
        // applied the event and subsequently queried that exact applied state.
        // A single target-side stage must not mask the missing half.
        const bool receiverVerified =
            event.targetAppliedReceiverVerified &&
            event.targetQueriedReceiverVerified;
        report.receiverVerifiedEvents += receiverVerified ? 1u : 0u;
        report.missingReceiverEvidenceEvents += receiverVerified ? 0u : 1u;
        report.crossSeatEvents += event.crossSeat ? 1u : 0u;
        report.crossProcessEvents += event.crossProcess ? 1u : 0u;
        report.keyEvents +=
            hasInputMetricEventClass(event.eventClass, InputMetricEventClass::Key)
                ? 1u
                : 0u;
        report.buttonEvents += hasInputMetricEventClass(
                                   event.eventClass,
                                   InputMetricEventClass::Button)
                                   ? 1u
                                   : 0u;
        report.movementEvents += hasInputMetricEventClass(
                                     event.eventClass,
                                     InputMetricEventClass::Movement)
                                     ? 1u
                                     : 0u;
        report.wheelEvents += hasInputMetricEventClass(
                                  event.eventClass,
                                  InputMetricEventClass::Wheel)
                                  ? 1u
                                  : 0u;

        std::optional<std::uint64_t> previous;
        for (const auto& timestamp : event.timestamps) {
            if (!timestamp) {
                continue;
            }
            if (previous && *timestamp < *previous) {
                return InputMetricsReportResult::TimestampOrderViolation;
            }
            previous = timestamp;
        }

        const bool complete = std::all_of(
            event.timestamps.begin(), event.timestamps.end(),
            [](const auto& value) { return value.has_value(); });
        if (complete) {
            ++report.completeInputEvents;
        } else {
            ++report.missingStageEvents;
        }

        const auto addPair = [&](std::size_t first, std::size_t second,
                                 std::vector<std::uint64_t>& values) {
            if (event.timestamps[first] && event.timestamps[second]) {
                addLatency(values, *event.timestamps[first],
                           *event.timestamps[second]);
            }
        };
        addPair(0u, 1u, observationToEnqueue);
        addPair(1u, 2u, enqueueToDequeue);
        addPair(2u, 3u, dequeueToWrite);
        addPair(3u, 4u, writeToApply);
        addPair(4u, 5u, applyToQuery);
        addPair(0u, 5u, endToEnd);
    }

    for (const auto& [correlationId, rollback] : rollbacks) {
        (void)correlationId;
        if (!rollback.start || !rollback.complete) {
            continue;
        }
        if (*rollback.complete < *rollback.start) {
            return InputMetricsReportResult::TimestampOrderViolation;
        }
        addLatency(rollbackDurations, *rollback.start, *rollback.complete);
    }

    report.observationToEnqueue = percentiles(std::move(observationToEnqueue));
    report.enqueueToDequeue = percentiles(std::move(enqueueToDequeue));
    report.dequeueToWrite = percentiles(std::move(dequeueToWrite));
    report.writeToApply = percentiles(std::move(writeToApply));
    report.applyToQuery = percentiles(std::move(applyToQuery));
    report.endToEnd = percentiles(std::move(endToEnd));
    report.rollback = percentiles(std::move(rollbackDurations));
    return InputMetricsReportResult::Success;
}

std::string encodeInputMetricSampleJson(const InputMetricSample& sample) {
    std::ostringstream out;
    out << '{';
    out << "\"schema_version\":" << sample.schemaVersion;
    out << ",\"correlation_id\":" << sample.correlationId;
    out << ",\"timestamp_us\":" << sample.timestampMicros;
    out << ",\"stage\":\"" << inputMetricStageName(sample.stage) << '\"';
    out << ",\"event_class_bits\":"
        << static_cast<unsigned>(static_cast<std::uint8_t>(sample.eventClass));
    out << ",\"expected_seat_id\":" << sample.expectedSeatId;
    out << ",\"receiving_seat_id\":" << sample.receivingSeatId;
    out << ",\"target_process_id\":" << sample.targetProcessId;
    out << ",\"receiving_process_id\":" << sample.receivingProcessId;
    out << ",\"detail_code\":" << sample.detailCode;
    out << ",\"queue_depth\":" << sample.queueDepth;
    out << ",\"queue_high_water\":" << sample.queueHighWater;
    out << ",\"queue_dropped\":" << sample.queueDroppedCount;
    out << ",\"host_cpu_time_us\":" << sample.hostCpuTimeMicros;
    out << ",\"host_working_set_bytes\":" << sample.hostWorkingSetBytes;
    out << '}';
    return out.str();
}

std::string encodeInputMetricsReportJson(const InputMetricsReport& report) {
    std::ostringstream out;
    out << '{';
    out << "\"schema_version\":" << report.schemaVersion;
    out << ",\"trace_samples\":" << report.traceSamples;
    out << ",\"unique_input_events\":" << report.uniqueInputEvents;
    out << ",\"complete_input_events\":" << report.completeInputEvents;
    out << ",\"missing_stage_events\":" << report.missingStageEvents;
    out << ",\"receiver_verified_events\":" << report.receiverVerifiedEvents;
    out << ",\"missing_receiver_evidence_events\":"
        << report.missingReceiverEvidenceEvents;
    out << ",\"cross_seat_events\":" << report.crossSeatEvents;
    out << ",\"cross_process_events\":" << report.crossProcessEvents;
    out << ",\"event_classes\":{";
    out << "\"key\":" << report.keyEvents;
    out << ",\"button\":" << report.buttonEvents;
    out << ",\"movement\":" << report.movementEvents;
    out << ",\"wheel\":" << report.wheelEvents << '}';
    out << ",\"queue\":{";
    out << "\"high_water\":" << report.queueHighWater;
    out << ",\"dropped_frames\":" << report.queueDroppedFrames << '}';
    out << ",\"recorder\":{";
    out << "\"rotation_drops\":" << report.recorderRotationDrops;
    out << ",\"contention_drops\":" << report.recorderContentionDrops;
    out << ",\"invalid_samples\":" << report.recorderInvalidSamples << '}';
    out << ",\"latency\":{";
    appendPercentilesJson(out, "observation_to_enqueue", report.observationToEnqueue);
    out << ',';
    appendPercentilesJson(out, "enqueue_to_dequeue", report.enqueueToDequeue);
    out << ',';
    appendPercentilesJson(out, "dequeue_to_write", report.dequeueToWrite);
    out << ',';
    appendPercentilesJson(out, "write_to_apply", report.writeToApply);
    out << ',';
    appendPercentilesJson(out, "apply_to_query", report.applyToQuery);
    out << ',';
    appendPercentilesJson(out, "end_to_end", report.endToEnd);
    out << ',';
    appendPercentilesJson(out, "rollback", report.rollback);
    out << '}';
    out << ",\"resource\":{";
    out << "\"samples\":" << report.resourceSamples;
    out << ",\"last_host_cpu_time_us\":" << report.lastHostCpuTimeMicros;
    out << ",\"max_host_working_set_bytes\":"
        << report.maxHostWorkingSetBytes << '}';
    out << '}';
    return out.str();
}

} // namespace hydra
