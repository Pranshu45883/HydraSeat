#pragma once

#include "hydra/compatibility_local_store.hpp"
#include "hydra/compatibility_result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hydra::compat {

enum class LocalCompatibilityEvidenceWriteCode : std::uint8_t {
    Success = 0,
    UnsupportedOrigin,
    IncompleteSession,
    ConversionFailed,
    OriginMismatch,
    StorePathUnavailable,
    StoreLoadFailed,
    RecordFailed,
    StoreSaveFailed,
};

struct LocalCompatibilityEvidenceWriteDiagnostic {
    LocalCompatibilityEvidenceWriteCode code{LocalCompatibilityEvidenceWriteCode::Success};
    CompatibilityResultCode compatibilityCode{CompatibilityResultCode::Success};
    community::CompatibilityLocalStoreCode storeCode{
        community::CompatibilityLocalStoreCode::Success};
    community::ShareModelCode shareModelCode{community::ShareModelCode::Success};
    std::string message;

    bool succeeded() const noexcept {
        return code == LocalCompatibilityEvidenceWriteCode::Success;
    }
};

struct LocalCompatibilityEvidenceWriteResult {
    LocalCompatibilityEvidenceWriteDiagnostic diagnostic;
    std::optional<CompatibilityResult> result;

    bool succeeded() const noexcept { return diagnostic.succeeded(); }
};

// Writes one completed local compatibility result to the exact trusted store path.
// The caller supplies only the evidence context/report; result origin and technical
// statuses are always derived by buildCompatibilityResultFromSessionMetrics().
LocalCompatibilityEvidenceWriteResult writeLocalCompatibilityEvidence(
    const LocalEvidenceContext& context,
    const metrics::SessionMetricsReport& report,
    const std::filesystem::path& storePath);

// Production convenience wrapper over the existing fixed LocalAppData store path.
// It does not introduce another persistence root.
LocalCompatibilityEvidenceWriteResult writeDefaultLocalCompatibilityEvidence(
    const LocalEvidenceContext& context,
    const metrics::SessionMetricsReport& report);

std::string_view localCompatibilityEvidenceWriteCodeName(
    LocalCompatibilityEvidenceWriteCode code) noexcept;

} // namespace hydra::compat
