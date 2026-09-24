#pragma once

#include "hydra/session_metrics.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::compat {

inline constexpr std::uint32_t kCompatibilityResultSchemaVersion = 1u;
inline constexpr std::uint32_t kCompatibilityRedactionSchemaVersion = 1u;
inline constexpr std::size_t kMaximumCompatibilityResultBytes = 256u * 1024u;
inline constexpr std::size_t kMaximumCompatibilityBackends = 32u;
inline constexpr std::size_t kMaximumCompatibilityIdentifierBytes = 128u;
inline constexpr std::size_t kMaximumCompatibilityVersionBytes = 96u;

enum class TimestampClass : std::uint8_t {
    DayBucket = 0,
    MonthBucket = 1,
};

enum class Scenario : std::uint8_t {
    DifferentGames = 0,
    SameGameTwoInstance = 1,
    ProtectedExperiment = 2,
};

enum class EvidenceStatus : std::uint8_t {
    NotMeasured = 0,
    Pass = 1,
    Fail = 2,
    Unsupported = 3,
};

enum class ResultOrigin : std::uint8_t {
    Synthetic = 0,
    ControlledProcess = 1,
    Physical = 2,
    ImportedCommunity = 3,
};

struct BackendEvidence {
    std::string backendId;
    std::optional<std::string> version;
    EvidenceStatus status{EvidenceStatus::NotMeasured};

    bool operator==(const BackendEvidence&) const = default;
};

struct OptionalMeasurements {
    std::optional<std::uint64_t> launchDurationMicros;
    std::optional<std::uint64_t> stopDurationMicros;
    std::optional<std::uint64_t> rollbackDurationMicros;
    std::optional<std::uint64_t> observedInputEvents;
    std::optional<std::uint64_t> verifiedCrossSeatEvents;
    std::optional<std::uint64_t> inputLatencyP95Micros;

    bool operator==(const OptionalMeasurements&) const = default;
};

struct RedactionMetadata {
    std::uint32_t schemaVersion{kCompatibilityRedactionSchemaVersion};
    bool credentialsExcluded{true};
    bool playerNamesExcluded{true};
    bool personalPathsExcluded{true};
    bool rawTypedTextExcluded{true};

    bool operator==(const RedactionMetadata&) const = default;
};

struct CompatibilityResult {
    std::uint32_t schemaVersion{kCompatibilityResultSchemaVersion};
    std::string resultId;
    TimestampClass timestampClass{TimestampClass::DayBucket};
    std::string timestampBucket;

    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::optional<std::string> gameVersion;

    std::string hydraSeatVersion;
    std::string hydraSeatBuild;
    std::string windowsBuildClass;
    std::string architecture;

    Scenario scenario{Scenario::DifferentGames};
    bool protectedExperimental{false};
    std::optional<std::uint64_t> setupRevision;
    std::vector<BackendEvidence> backends;

    EvidenceStatus launch{EvidenceStatus::NotMeasured};
    EvidenceStatus secondInstance{EvidenceStatus::NotMeasured};
    EvidenceStatus inputIsolation{EvidenceStatus::NotMeasured};
    EvidenceStatus controller{EvidenceStatus::NotMeasured};
    EvidenceStatus audio{EvidenceStatus::NotMeasured};
    EvidenceStatus cleanExit{EvidenceStatus::NotMeasured};
    EvidenceStatus rollback{EvidenceStatus::NotMeasured};

    OptionalMeasurements measurements;
    ResultOrigin origin{ResultOrigin::Synthetic};
    RedactionMetadata redaction;
    std::string provenanceId;
    std::uint64_t provenanceRevision{0};

    bool operator==(const CompatibilityResult&) const = default;
};

struct LocalEvidenceContext {
    std::string resultId;
    TimestampClass timestampClass{TimestampClass::DayBucket};
    std::string timestampBucket;
    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::optional<std::string> gameVersion;
    std::string hydraSeatVersion;
    std::string hydraSeatBuild;
    std::string windowsBuildClass;
    std::string architecture;
    Scenario scenario{Scenario::DifferentGames};
    bool protectedExperimental{false};
    std::optional<std::uint64_t> setupRevision;
    std::vector<BackendEvidence> backends;
    std::string provenanceId;
    std::uint64_t provenanceRevision{0};
};

enum class CompatibilityResultCode : std::uint8_t {
    Success = 0,
    TooLarge,
    ParseError,
    UnsupportedSchema,
    UnknownField,
    MissingField,
    WrongType,
    InvalidIdentifier,
    InvalidVersion,
    InvalidTimestamp,
    InvalidEnum,
    DuplicateBackend,
    InvalidMeasurement,
    InvalidRedaction,
    InvalidProvenance,
    InvalidLocalEvidence,
};

struct CompatibilityDiagnostic {
    CompatibilityResultCode code{CompatibilityResultCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == CompatibilityResultCode::Success; }
};

CompatibilityDiagnostic validateCompatibilityResult(const CompatibilityResult& value);
CompatibilityDiagnostic canonicalizeCompatibilityResult(CompatibilityResult& value);
CompatibilityDiagnostic encodeCompatibilityResultJson(const CompatibilityResult& value,
                                                        std::string& output);
CompatibilityDiagnostic decodeCompatibilityResultJson(std::string_view json,
                                                        CompatibilityResult& output);

// Converts the already privacy-safe Phase 5 session evidence into the public data
// boundary. Missing measurements remain NotMeasured/null; they are never encoded as
// a numeric zero merely because the local source did not measure them.
CompatibilityDiagnostic buildCompatibilityResultFromSessionMetrics(
    const LocalEvidenceContext& context,
    const metrics::SessionMetricsReport& report,
    CompatibilityResult& output);

std::string_view timestampClassName(TimestampClass value) noexcept;
std::string_view scenarioName(Scenario value) noexcept;
std::string_view evidenceStatusName(EvidenceStatus value) noexcept;
std::string_view resultOriginName(ResultOrigin value) noexcept;
std::string_view compatibilityResultCodeName(CompatibilityResultCode value) noexcept;

} // namespace hydra::compat
