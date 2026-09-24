#pragma once

#include "hydra/compatibility_result.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::compat {

inline constexpr std::size_t kMaximumAggregationResults = 100000u;

enum class FreshnessClass : std::uint8_t {
    Current = 0,
    Stale = 1,
};

enum class SessionDisposition : std::uint8_t {
    Untested = 0,
    Success = 1,
    Failure = 2,
};

struct AggregationPolicy {
    // Calendar month used only as a deterministic comparison point; aggregation
    // does not read wall-clock time. Format YYYY-MM.
    std::string referenceMonth;
    std::uint32_t staleAfterMonths{6u};
};

struct BackendSelector {
    std::string backendId;
    std::optional<std::string> version;

    bool operator==(const BackendSelector&) const = default;
    auto operator<=>(const BackendSelector&) const = default;
};

struct CohortKey {
    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::optional<std::string> gameVersion;
    std::string hydraSeatVersion;
    std::string windowsBuildClass;
    std::string architecture;
    Scenario scenario{Scenario::DifferentGames};
    bool protectedExperimental{false};
    std::optional<std::uint64_t> setupRevision;
    std::vector<BackendSelector> backends;
    FreshnessClass freshness{FreshnessClass::Current};

    bool operator==(const CohortKey&) const = default;
    auto operator<=>(const CohortKey&) const = default;
};

struct OutcomeCounts {
    std::uint64_t notMeasured{0};
    std::uint64_t pass{0};
    std::uint64_t fail{0};
    std::uint64_t unsupported{0};

    std::uint64_t total() const noexcept {
        return notMeasured + pass + fail + unsupported;
    }

    bool operator==(const OutcomeCounts&) const = default;
};

struct SessionDispositionCounts {
    std::uint64_t untested{0};
    std::uint64_t success{0};
    std::uint64_t failure{0};

    std::uint64_t total() const noexcept { return untested + success + failure; }

    bool operator==(const SessionDispositionCounts&) const = default;
};

struct OriginCounts {
    std::uint64_t synthetic{0};
    std::uint64_t controlledProcess{0};
    std::uint64_t physical{0};
    std::uint64_t importedCommunity{0};

    bool operator==(const OriginCounts&) const = default;
};

struct CohortStatistics {
    CohortKey key;
    std::uint64_t sampleSize{0};
    SessionDispositionCounts session;
    OutcomeCounts launch;
    OutcomeCounts secondInstance;
    OutcomeCounts inputIsolation;
    OutcomeCounts controller;
    OutcomeCounts audio;
    OutcomeCounts cleanExit;
    OutcomeCounts rollback;
    OriginCounts origins;

    bool operator==(const CohortStatistics&) const = default;
};

enum class AggregationCode : std::uint8_t {
    Success = 0,
    InvalidPolicy,
    TooManyResults,
    InvalidResult,
    DuplicateResultId,
    FutureTimestamp,
};

struct AggregationDiagnostic {
    AggregationCode code{AggregationCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == AggregationCode::Success; }
};

AggregationDiagnostic aggregateCompatibilityResults(
    std::span<const CompatibilityResult> results,
    const AggregationPolicy& policy,
    std::vector<CohortStatistics>& output);

SessionDisposition classifySessionDisposition(const CompatibilityResult& result) noexcept;
std::string_view freshnessClassName(FreshnessClass value) noexcept;
std::string_view sessionDispositionName(SessionDisposition value) noexcept;
std::string_view aggregationCodeName(AggregationCode value) noexcept;

} // namespace hydra::compat
