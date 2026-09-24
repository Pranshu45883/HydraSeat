#include "hydra/local_compatibility_evidence.hpp"

#include <utility>

namespace hydra::compat {
namespace {

LocalCompatibilityEvidenceWriteResult fail(
    LocalCompatibilityEvidenceWriteCode code,
    std::string message,
    CompatibilityResultCode compatibilityCode = CompatibilityResultCode::Success,
    community::CompatibilityLocalStoreCode storeCode =
        community::CompatibilityLocalStoreCode::Success,
    community::ShareModelCode shareModelCode = community::ShareModelCode::Success) {
    LocalCompatibilityEvidenceWriteResult result;
    result.diagnostic.code = code;
    result.diagnostic.compatibilityCode = compatibilityCode;
    result.diagnostic.storeCode = storeCode;
    result.diagnostic.shareModelCode = shareModelCode;
    result.diagnostic.message = std::move(message);
    return result;
}

ResultOrigin expectedResultOrigin(metrics::EvidenceOrigin origin) noexcept {
    switch (origin) {
        case metrics::EvidenceOrigin::ControlledProcess:
            return ResultOrigin::ControlledProcess;
        case metrics::EvidenceOrigin::Physical:
            return ResultOrigin::Physical;
        case metrics::EvidenceOrigin::Synthetic:
            return ResultOrigin::Synthetic;
    }
    return ResultOrigin::Synthetic;
}

} // namespace

LocalCompatibilityEvidenceWriteResult writeLocalCompatibilityEvidence(
    const LocalEvidenceContext& context,
    const metrics::SessionMetricsReport& report,
    const std::filesystem::path& storePath) {
    if (report.origin == metrics::EvidenceOrigin::Synthetic) {
        return fail(LocalCompatibilityEvidenceWriteCode::UnsupportedOrigin,
                    "Synthetic session evidence cannot become a release-produced local compatibility result");
    }
    if (report.finalState == metrics::SessionFinalState::Running) {
        return fail(LocalCompatibilityEvidenceWriteCode::IncompleteSession,
                    "running session evidence is not complete enough for durable local compatibility history");
    }
    if (storePath.empty()) {
        return fail(LocalCompatibilityEvidenceWriteCode::StorePathUnavailable,
                    "local compatibility evidence store path is empty");
    }

    CompatibilityResult produced;
    const auto converted = buildCompatibilityResultFromSessionMetrics(context, report, produced);
    if (!converted.succeeded()) {
        return fail(LocalCompatibilityEvidenceWriteCode::ConversionFailed,
                    "session evidence could not be converted to canonical compatibility result: " +
                        converted.message,
                    converted.code);
    }

    const auto expectedOrigin = expectedResultOrigin(report.origin);
    if (produced.origin != expectedOrigin) {
        return fail(LocalCompatibilityEvidenceWriteCode::OriginMismatch,
                    "canonical compatibility result origin does not match the measured session evidence origin");
    }

    community::CompatibilityLocalStore store(storePath);
    community::CompatibilityShareModel model;
    const auto loaded = store.load(model);
    if (!loaded.succeeded()) {
        return fail(LocalCompatibilityEvidenceWriteCode::StoreLoadFailed,
                    "existing local compatibility history could not be loaded: " + loaded.message,
                    CompatibilityResultCode::Success,
                    loaded.code);
    }

    const auto recorded = model.recordLocalResult(produced);
    if (!recorded.succeeded()) {
        return fail(LocalCompatibilityEvidenceWriteCode::RecordFailed,
                    "canonical local compatibility result was rejected by the existing history model: " +
                        recorded.message,
                    CompatibilityResultCode::Success,
                    community::CompatibilityLocalStoreCode::Success,
                    recorded.code);
    }

    const auto saved = store.save(model);
    if (!saved.succeeded()) {
        return fail(LocalCompatibilityEvidenceWriteCode::StoreSaveFailed,
                    "updated local compatibility history could not be persisted atomically: " +
                        saved.message,
                    CompatibilityResultCode::Success,
                    saved.code);
    }

    LocalCompatibilityEvidenceWriteResult result;
    result.result = std::move(produced);
    return result;
}

LocalCompatibilityEvidenceWriteResult writeDefaultLocalCompatibilityEvidence(
    const LocalEvidenceContext& context,
    const metrics::SessionMetricsReport& report) {
    std::string pathError;
    const auto storePath = community::defaultCompatibilityLocalStorePath(&pathError);
    if (!storePath) {
        return fail(LocalCompatibilityEvidenceWriteCode::StorePathUnavailable,
                    "default local compatibility evidence store path is unavailable: " + pathError);
    }
    return writeLocalCompatibilityEvidence(context, report, *storePath);
}

std::string_view localCompatibilityEvidenceWriteCodeName(
    LocalCompatibilityEvidenceWriteCode code) noexcept {
    switch (code) {
        case LocalCompatibilityEvidenceWriteCode::Success: return "Success";
        case LocalCompatibilityEvidenceWriteCode::UnsupportedOrigin: return "UnsupportedOrigin";
        case LocalCompatibilityEvidenceWriteCode::IncompleteSession: return "IncompleteSession";
        case LocalCompatibilityEvidenceWriteCode::ConversionFailed: return "ConversionFailed";
        case LocalCompatibilityEvidenceWriteCode::OriginMismatch: return "OriginMismatch";
        case LocalCompatibilityEvidenceWriteCode::StorePathUnavailable: return "StorePathUnavailable";
        case LocalCompatibilityEvidenceWriteCode::StoreLoadFailed: return "StoreLoadFailed";
        case LocalCompatibilityEvidenceWriteCode::RecordFailed: return "RecordFailed";
        case LocalCompatibilityEvidenceWriteCode::StoreSaveFailed: return "StoreSaveFailed";
    }
    return "Unknown";
}

} // namespace hydra::compat
