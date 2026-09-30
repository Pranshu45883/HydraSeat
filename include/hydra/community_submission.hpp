#pragma once

#include "hydra/compatibility_result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hydra::community {

inline constexpr std::size_t kMaximumSubmissionReceiptBytes = 128u;

enum class TransportAvailability : std::uint8_t {
    Available = 0,
    Offline = 1,
    Unavailable = 2,
};

enum class SubmissionTransportResult : std::uint8_t {
    Accepted = 0,
    DuplicateAccepted = 1,
    Offline = 2,
    Unavailable = 3,
    Timeout = 4,
    RetryableFailure = 5,
    PermanentFailure = 6,
    InvalidResponse = 7,
};

struct SubmissionEnvelope {
    std::string submissionId;
    std::string idempotencyKey;
    std::string resultId;
    std::string exactRedactedJson;
    bool protectedExperimental{false};

    bool operator==(const SubmissionEnvelope&) const = default;
};

struct SubmissionReceipt {
    SubmissionTransportResult result{SubmissionTransportResult::RetryableFailure};
    std::optional<std::string> receiptId;
    bool remoteAccepted{false};

    bool succeeded() const noexcept {
        return result == SubmissionTransportResult::Accepted ||
               result == SubmissionTransportResult::DuplicateAccepted;
    }
};

class CommunitySubmissionTransport {
public:
    virtual ~CommunitySubmissionTransport() = default;
    virtual TransportAvailability availability() const noexcept = 0;
    virtual SubmissionReceipt submit(const SubmissionEnvelope& envelope) noexcept = 0;
};

enum class SubmissionCode : std::uint8_t {
    Success = 0,
    InvalidResult,
    PreviewRequired,
    PreviewMismatch,
    TransportOffline,
    TransportUnavailable,
    TransportTimeout,
    TransportRetryableFailure,
    TransportPermanentFailure,
    InvalidTransportResponse,
};

struct SubmissionDiagnostic {
    SubmissionCode code{SubmissionCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == SubmissionCode::Success; }
};

struct SubmissionPreview {
    std::string exactRedactedJson;
    std::string submissionId;
    std::string idempotencyKey;
    bool protectedExperimental{false};

    bool operator==(const SubmissionPreview&) const = default;
};

// One explicit local-result sharing session. There is deliberately no timer,
// background worker, automatic retry, account field, or credential surface.
class SubmissionSession final {
public:
    SubmissionDiagnostic prepare(const compat::CompatibilityResult& localResult,
                                 SubmissionPreview& preview);
    SubmissionDiagnostic approve(std::string_view exactPreviewJson);
    SubmissionDiagnostic submit(CommunitySubmissionTransport& transport,
                                SubmissionReceipt& receipt);

    const std::optional<compat::CompatibilityResult>& localResult() const noexcept {
        return localResult_;
    }
    const std::optional<SubmissionEnvelope>& preparedEnvelope() const noexcept {
        return envelope_;
    }
    bool approved() const noexcept { return approved_; }

private:
    std::optional<compat::CompatibilityResult> localResult_;
    std::optional<SubmissionEnvelope> envelope_;
    bool approved_{false};
};

std::string_view transportAvailabilityName(TransportAvailability value) noexcept;
std::string_view submissionTransportResultName(SubmissionTransportResult value) noexcept;
std::string_view submissionCodeName(SubmissionCode value) noexcept;

} // namespace hydra::community
