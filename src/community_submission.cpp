#include "hydra/community_submission.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace hydra::community {
namespace {

constexpr std::uint64_t kFnvOffsetA = 1469598103934665603ull;
constexpr std::uint64_t kFnvOffsetB = 1099511628211ull ^ 0x9e3779b97f4a7c15ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

SubmissionDiagnostic fail(SubmissionCode code, std::string message) {
    return {code, std::move(message)};
}

std::uint64_t fnv1a(std::string_view value, std::uint64_t seed) noexcept {
    std::uint64_t hash = seed;
    for (const char raw : value) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
        hash *= kFnvPrime;
    }
    return hash;
}

std::string hex64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

bool validReceiptId(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumSubmissionReceiptBytes) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
              ch == ':')) {
            return false;
        }
    }
    return true;
}

bool validTransportResult(SubmissionTransportResult result) noexcept {
    return result == SubmissionTransportResult::Accepted ||
           result == SubmissionTransportResult::DuplicateAccepted ||
           result == SubmissionTransportResult::Offline ||
           result == SubmissionTransportResult::Unavailable ||
           result == SubmissionTransportResult::Timeout ||
           result == SubmissionTransportResult::RetryableFailure ||
           result == SubmissionTransportResult::PermanentFailure ||
           result == SubmissionTransportResult::InvalidResponse;
}

SubmissionDiagnostic validateReceipt(const SubmissionReceipt& receipt) {
    if (!validTransportResult(receipt.result)) {
        return fail(SubmissionCode::InvalidTransportResponse,
                    "submission transport returned an unknown result value");
    }
    if (receipt.receiptId && !validReceiptId(*receipt.receiptId)) {
        return fail(SubmissionCode::InvalidTransportResponse,
                    "submission transport returned an invalid bounded receipt identifier");
    }
    if (receipt.succeeded()) {
        if (!receipt.remoteAccepted || !receipt.receiptId) {
            return fail(SubmissionCode::InvalidTransportResponse,
                        "successful transport response must prove remote acceptance with a receipt");
        }
    } else if (receipt.remoteAccepted) {
        return fail(SubmissionCode::InvalidTransportResponse,
                    "failed transport response cannot claim remote acceptance");
    }
    return {};
}

SubmissionDiagnostic transportFailure(const SubmissionReceipt& receipt) {
    switch (receipt.result) {
        case SubmissionTransportResult::Accepted:
        case SubmissionTransportResult::DuplicateAccepted:
            return {};
        case SubmissionTransportResult::Offline:
            return fail(SubmissionCode::TransportOffline,
                        "community submission transport is offline; local result remains available");
        case SubmissionTransportResult::Unavailable:
            return fail(SubmissionCode::TransportUnavailable,
                        "community submission transport is unavailable; local result remains available");
        case SubmissionTransportResult::Timeout:
            return fail(SubmissionCode::TransportTimeout,
                        "community submission timed out; explicit retry may reuse the same idempotency key");
        case SubmissionTransportResult::RetryableFailure:
            return fail(SubmissionCode::TransportRetryableFailure,
                        "community submission failed transiently; local result remains available");
        case SubmissionTransportResult::PermanentFailure:
            return fail(SubmissionCode::TransportPermanentFailure,
                        "community submission was rejected; local result remains available");
        case SubmissionTransportResult::InvalidResponse:
            return fail(SubmissionCode::InvalidTransportResponse,
                        "community submission transport reported an invalid response");
    }
    return fail(SubmissionCode::InvalidTransportResponse,
                "community submission transport result is unknown");
}

} // namespace

SubmissionDiagnostic SubmissionSession::prepare(const compat::CompatibilityResult& localResult,
                                                SubmissionPreview& preview) {
    auto canonical = localResult;
    const auto canonicalized = compat::canonicalizeCompatibilityResult(canonical);
    if (!canonicalized.succeeded()) {
        return fail(SubmissionCode::InvalidResult,
                    "local compatibility result is not valid for sharing: " + canonicalized.message);
    }

    std::string json;
    const auto encoded = compat::encodeCompatibilityResultJson(canonical, json);
    if (!encoded.succeeded()) {
        return fail(SubmissionCode::InvalidResult,
                    "local compatibility result cannot be encoded: " + encoded.message);
    }

    SubmissionEnvelope envelope;
    envelope.submissionId = "compat:" + canonical.resultId;
    envelope.idempotencyKey = "compat-v1-" + hex64(fnv1a(json, kFnvOffsetA)) +
                              hex64(fnv1a(json, kFnvOffsetB));
    envelope.resultId = canonical.resultId;
    envelope.exactRedactedJson = json;
    envelope.protectedExperimental = canonical.protectedExperimental;

    SubmissionPreview candidate;
    candidate.exactRedactedJson = envelope.exactRedactedJson;
    candidate.submissionId = envelope.submissionId;
    candidate.idempotencyKey = envelope.idempotencyKey;
    candidate.protectedExperimental = envelope.protectedExperimental;

    localResult_ = std::move(canonical);
    envelope_ = std::move(envelope);
    approved_ = false;
    preview = std::move(candidate);
    return {};
}

SubmissionDiagnostic SubmissionSession::approve(std::string_view exactPreviewJson) {
    if (!envelope_) {
        return fail(SubmissionCode::PreviewRequired,
                    "local compatibility result must be prepared and previewed before sharing");
    }
    if (exactPreviewJson != envelope_->exactRedactedJson) {
        approved_ = false;
        return fail(SubmissionCode::PreviewMismatch,
                    "sharing approval does not match the exact redacted JSON preview");
    }
    approved_ = true;
    return {};
}

SubmissionDiagnostic SubmissionSession::submit(CommunitySubmissionTransport& transport,
                                               SubmissionReceipt& receipt) {
    if (!envelope_ || !approved_) {
        return fail(SubmissionCode::PreviewRequired,
                    "community submission requires explicit approval of the exact preview");
    }

    switch (transport.availability()) {
        case TransportAvailability::Available:
            break;
        case TransportAvailability::Offline:
            return fail(SubmissionCode::TransportOffline,
                        "community submission is offline; local result remains available");
        case TransportAvailability::Unavailable:
            return fail(SubmissionCode::TransportUnavailable,
                        "community submission transport is unavailable; local result remains available");
    }

    const auto candidate = transport.submit(*envelope_);
    const auto validation = validateReceipt(candidate);
    if (!validation.succeeded()) return validation;
    receipt = candidate;
    return transportFailure(candidate);
}

std::string_view transportAvailabilityName(TransportAvailability value) noexcept {
    switch (value) {
        case TransportAvailability::Available: return "Available";
        case TransportAvailability::Offline: return "Offline";
        case TransportAvailability::Unavailable: return "Unavailable";
    }
    return "Unknown";
}

std::string_view submissionTransportResultName(SubmissionTransportResult value) noexcept {
    switch (value) {
        case SubmissionTransportResult::Accepted: return "Accepted";
        case SubmissionTransportResult::DuplicateAccepted: return "DuplicateAccepted";
        case SubmissionTransportResult::Offline: return "Offline";
        case SubmissionTransportResult::Unavailable: return "Unavailable";
        case SubmissionTransportResult::Timeout: return "Timeout";
        case SubmissionTransportResult::RetryableFailure: return "RetryableFailure";
        case SubmissionTransportResult::PermanentFailure: return "PermanentFailure";
        case SubmissionTransportResult::InvalidResponse: return "InvalidResponse";
    }
    return "Unknown";
}

std::string_view submissionCodeName(SubmissionCode value) noexcept {
    switch (value) {
        case SubmissionCode::Success: return "Success";
        case SubmissionCode::InvalidResult: return "InvalidResult";
        case SubmissionCode::PreviewRequired: return "PreviewRequired";
        case SubmissionCode::PreviewMismatch: return "PreviewMismatch";
        case SubmissionCode::TransportOffline: return "TransportOffline";
        case SubmissionCode::TransportUnavailable: return "TransportUnavailable";
        case SubmissionCode::TransportTimeout: return "TransportTimeout";
        case SubmissionCode::TransportRetryableFailure: return "TransportRetryableFailure";
        case SubmissionCode::TransportPermanentFailure: return "TransportPermanentFailure";
        case SubmissionCode::InvalidTransportResponse: return "InvalidTransportResponse";
    }
    return "Unknown";
}

} // namespace hydra::community
