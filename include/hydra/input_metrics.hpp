#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hydra {

struct RawInputEvent;

inline constexpr std::uint32_t kInputMetricsSchemaVersion = 1;
inline constexpr std::size_t kDefaultInputMetricsCapacity = 4096;
inline constexpr std::size_t kMaxInputMetricsCapacity = 16384;

enum class InputMetricStage : std::uint8_t {
    PhysicalObserved = 1,
    RouteEnqueued = 2,
    RouteDequeued = 3,
    RouteWritten = 4,
    TargetApplied = 5,
    TargetQueried = 6,
    RouteDropped = 7,
    RollbackStarted = 8,
    RollbackCompleted = 9,
    HostResourceSample = 10
};

enum class InputMetricEventClass : std::uint8_t {
    None = 0,
    Key = 1u << 0,
    Button = 1u << 1,
    Movement = 1u << 2,
    Wheel = 1u << 3
};

constexpr InputMetricEventClass operator|(InputMetricEventClass left,
                                           InputMetricEventClass right) noexcept {
    return static_cast<InputMetricEventClass>(
        static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

constexpr InputMetricEventClass& operator|=(InputMetricEventClass& left,
                                             InputMetricEventClass right) noexcept {
    left = left | right;
    return left;
}

constexpr bool hasInputMetricEventClass(InputMetricEventClass value,
                                        InputMetricEventClass expected) noexcept {
    return (static_cast<std::uint8_t>(value) &
            static_cast<std::uint8_t>(expected)) ==
           static_cast<std::uint8_t>(expected);
}

enum class InputMetricsPrivacyMode : std::uint8_t {
    Redacted = 0,
    Diagnostic = 1
};

struct InputMetricSample {
    std::uint32_t schemaVersion{kInputMetricsSchemaVersion};
    std::uint64_t correlationId{0};
    // Every sample in one report must use the recorder's monotonic clock domain.
    // Cross-process producers must normalize before recording; unrelated raw
    // steady-clock epochs are not comparable latency evidence.
    std::uint64_t timestampMicros{0};
    InputMetricStage stage{InputMetricStage::PhysicalObserved};
    InputMetricEventClass eventClass{InputMetricEventClass::None};

    std::uint32_t expectedSeatId{0};
    // receiving* stays zero for host routing intent and is populated only from
    // receiver-side apply/query evidence.
    std::uint32_t receivingSeatId{0};
    std::uint32_t targetProcessId{0};
    std::uint32_t receivingProcessId{0};

    // Key/button identity is populated only in explicitly enabled Diagnostic
    // privacy mode. Redacted recorders force this field to zero before storage.
    std::uint32_t detailCode{0};

    std::uint32_t queueDepth{0};
    std::uint32_t queueHighWater{0};
    std::uint64_t queueDroppedCount{0};

    // Host resource values are externally sampled hooks. This component does not
    // poll the OS from an input callback.
    std::uint64_t hostCpuTimeMicros{0};
    std::uint64_t hostWorkingSetBytes{0};

    bool operator==(const InputMetricSample&) const = default;
};

struct InputMetricsSnapshot {
    std::uint32_t schemaVersion{kInputMetricsSchemaVersion};
    InputMetricsPrivacyMode privacyMode{InputMetricsPrivacyMode::Redacted};
    std::size_t capacity{0};
    std::uint64_t acceptedSamples{0};
    std::uint64_t rotationDrops{0};
    std::uint64_t contentionDrops{0};
    std::uint64_t invalidSamples{0};
    std::vector<InputMetricSample> samples;
};

// Fixed-capacity process-local recorder. tryRecord() never waits for another
// thread and performs no allocation or I/O. If the recorder is momentarily busy,
// the sample is rejected and contentionDrops advances. When the ring is full the
// oldest sample is rotated out and rotationDrops advances.
class InputMetricsRecorder {
public:
    explicit InputMetricsRecorder(
        std::size_t capacity = kDefaultInputMetricsCapacity,
        InputMetricsPrivacyMode privacyMode =
            InputMetricsPrivacyMode::Redacted);

    InputMetricsRecorder(const InputMetricsRecorder&) = delete;
    InputMetricsRecorder& operator=(const InputMetricsRecorder&) = delete;

    bool tryRecord(InputMetricSample sample) noexcept;

    // Snapshot/report generation is intentionally off the latency-sensitive input
    // path. Call after producers are quiesced when a lossless snapshot is needed.
    InputMetricsSnapshot snapshot() const;
    void reset() noexcept;

    std::size_t capacity() const noexcept { return m_ring.size(); }
    InputMetricsPrivacyMode privacyMode() const noexcept { return m_privacyMode; }

private:
    bool tryLock() const noexcept;
    void unlock() const noexcept;

    InputMetricsPrivacyMode m_privacyMode{InputMetricsPrivacyMode::Redacted};
    std::vector<InputMetricSample> m_ring;
    std::size_t m_start{0};
    std::size_t m_size{0};
    std::uint64_t m_acceptedSamples{0};
    mutable std::atomic_flag m_guard = ATOMIC_FLAG_INIT;
    mutable std::atomic<std::uint64_t> m_contentionDrops{0};
    std::atomic<std::uint64_t> m_rotationDrops{0};
    std::atomic<std::uint64_t> m_invalidSamples{0};
};

struct InputLatencyPercentiles {
    std::uint64_t count{0};
    std::uint64_t p50Micros{0};
    std::uint64_t p95Micros{0};
    std::uint64_t p99Micros{0};
    std::uint64_t maxMicros{0};

    bool operator==(const InputLatencyPercentiles&) const = default;
};

struct InputMetricsReport {
    std::uint32_t schemaVersion{kInputMetricsSchemaVersion};
    std::uint64_t traceSamples{0};
    std::uint64_t uniqueInputEvents{0};
    std::uint64_t completeInputEvents{0};
    std::uint64_t missingStageEvents{0};
    std::uint64_t receiverVerifiedEvents{0};
    std::uint64_t missingReceiverEvidenceEvents{0};
    std::uint64_t crossSeatEvents{0};
    std::uint64_t crossProcessEvents{0};

    std::uint64_t keyEvents{0};
    std::uint64_t buttonEvents{0};
    std::uint64_t movementEvents{0};
    std::uint64_t wheelEvents{0};

    std::uint32_t queueHighWater{0};
    std::uint64_t queueDroppedFrames{0};
    std::uint64_t recorderRotationDrops{0};
    std::uint64_t recorderContentionDrops{0};
    std::uint64_t recorderInvalidSamples{0};

    InputLatencyPercentiles observationToEnqueue;
    InputLatencyPercentiles enqueueToDequeue;
    InputLatencyPercentiles dequeueToWrite;
    InputLatencyPercentiles writeToApply;
    InputLatencyPercentiles applyToQuery;
    InputLatencyPercentiles endToEnd;
    InputLatencyPercentiles rollback;

    std::uint64_t resourceSamples{0};
    std::uint64_t lastHostCpuTimeMicros{0};
    std::uint64_t maxHostWorkingSetBytes{0};
};

enum class InputMetricsReportResult : std::uint32_t {
    Success = 0,
    EmptyTrace = 1,
    UnsupportedSchema = 2,
    InvalidTimestamp = 3,
    TimestampOrderViolation = 4
};

std::uint64_t monotonicInputMetricTimestampMicros() noexcept;
InputMetricEventClass classifyInputMetricEvent(const RawInputEvent& event) noexcept;
std::uint32_t inputMetricDetailCode(const RawInputEvent& event) noexcept;

std::string_view inputMetricStageName(InputMetricStage stage) noexcept;
std::string_view inputMetricsReportResultName(InputMetricsReportResult result) noexcept;

InputMetricsReportResult buildInputMetricsReport(
    const InputMetricsSnapshot& snapshot,
    InputMetricsReport& report);

std::string encodeInputMetricSampleJson(const InputMetricSample& sample);
std::string encodeInputMetricsReportJson(const InputMetricsReport& report);

} // namespace hydra
