#include "hydra/community_submission.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::community;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

compat::CompatibilityResult resultFixture(bool protectedExperimental = false) {
    compat::CompatibilityResult value;
    value.resultId = "rpc-result-1";
    value.timestampClass = compat::TimestampClass::MonthBucket;
    value.timestampBucket = "2026-08";
    value.gameId = "game:rpc";
    value.providerId = "custom";
    value.providerAppId = "rpc-app";
    value.gameVersion = "1.0";
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "rpc-build";
    value.windowsBuildClass = "win10-2009";
    value.architecture = "x64";
    value.scenario = protectedExperimental ? compat::Scenario::ProtectedExperiment
                                           : compat::Scenario::DifferentGames;
    value.protectedExperimental = protectedExperimental;
    value.launch = compat::EvidenceStatus::Pass;
    value.inputIsolation = compat::EvidenceStatus::Pass;
    value.cleanExit = compat::EvidenceStatus::Pass;
    value.origin = compat::ResultOrigin::ControlledProcess;
    value.provenanceId = "rpc-fixture";
    value.provenanceRevision = 3u;
    return value;
}

class FakeTransport final : public CommunitySubmissionTransport {
public:
    TransportAvailability availabilityValue{TransportAvailability::Available};
    std::vector<SubmissionReceipt> scripted;
    std::size_t calls{0u};
    std::vector<SubmissionEnvelope> seen;

    TransportAvailability availability() const noexcept override { return availabilityValue; }

    SubmissionReceipt submit(const SubmissionEnvelope& envelope) noexcept override {
        ++calls;
        try {
            seen.push_back(envelope);
        } catch (...) {
            return {SubmissionTransportResult::RetryableFailure, std::nullopt, false};
        }
        if (scripted.empty()) {
            return {SubmissionTransportResult::Accepted, "receipt-default", true};
        }
        const auto index = calls - 1u;
        return scripted[index < scripted.size() ? index : scripted.size() - 1u];
    }
};

void testPrepareIsDeterministicAndPreviewBound() {
    SubmissionSession first;
    SubmissionSession second;
    SubmissionPreview firstPreview;
    SubmissionPreview secondPreview;
    check(first.prepare(resultFixture(true), firstPreview).succeeded() &&
              second.prepare(resultFixture(true), secondPreview).succeeded() &&
              firstPreview == secondPreview,
          "same local result produces deterministic submission identity and exact JSON preview");
    check(firstPreview.protectedExperimental &&
              firstPreview.exactRedactedJson.find("\"protected_experimental\":true") !=
                  std::string::npos,
          "Protected / Experimental marker is retained in the exact submission preview");

    FakeTransport transport;
    SubmissionReceipt receipt;
    check(first.submit(transport, receipt).code == SubmissionCode::PreviewRequired &&
              transport.calls == 0u,
          "transport cannot run before exact preview approval");

    auto changed = firstPreview.exactRedactedJson;
    changed.push_back(' ');
    check(first.approve(changed).code == SubmissionCode::PreviewMismatch &&
              !first.approved(),
          "approval is rejected if exact preview bytes changed");
    check(first.approve(firstPreview.exactRedactedJson).succeeded() && first.approved(),
          "exact redacted JSON can be explicitly approved");
}

void testOfflineAndUnavailableDoNotTouchLocalResultOrTransport() {
    SubmissionSession session;
    SubmissionPreview preview;
    const auto local = resultFixture();
    session.prepare(local, preview);
    session.approve(preview.exactRedactedJson);

    FakeTransport transport;
    transport.availabilityValue = TransportAvailability::Offline;
    SubmissionReceipt receipt;
    check(session.submit(transport, receipt).code == SubmissionCode::TransportOffline &&
              transport.calls == 0u && session.localResult().has_value() &&
              session.localResult()->resultId == local.resultId,
          "offline transport fails before invocation and preserves the complete local result");

    transport.availabilityValue = TransportAvailability::Unavailable;
    check(session.submit(transport, receipt).code == SubmissionCode::TransportUnavailable &&
              transport.calls == 0u && session.localResult()->resultId == local.resultId,
          "unavailable optional service cannot affect local compatibility evidence");
}

void testRetryIsExplicitAndReusesIdempotencyIdentity() {
    SubmissionSession session;
    SubmissionPreview preview;
    session.prepare(resultFixture(), preview);
    session.approve(preview.exactRedactedJson);

    FakeTransport transport;
    transport.scripted = {
        {SubmissionTransportResult::Timeout, std::nullopt, false},
        {SubmissionTransportResult::Accepted, "receipt-1", true},
    };

    SubmissionReceipt receipt;
    check(session.submit(transport, receipt).code == SubmissionCode::TransportTimeout &&
              transport.calls == 1u,
          "timeout performs exactly one transport attempt with no hidden automatic retry");
    check(session.localResult().has_value(),
          "timeout leaves the local result available for inspection/export/retry");

    check(session.submit(transport, receipt).succeeded() && transport.calls == 2u &&
              receipt.succeeded() && receipt.remoteAccepted,
          "a later explicit retry may succeed");
    check(transport.seen.size() == 2u &&
              transport.seen[0].idempotencyKey == transport.seen[1].idempotencyKey &&
              transport.seen[0].exactRedactedJson == transport.seen[1].exactRedactedJson,
          "explicit retry reuses the exact payload and deterministic idempotency identity");
}

void testDuplicateAndMalformedReceiptsAreHandledFailClosed() {
    SubmissionSession duplicateSession;
    SubmissionPreview duplicatePreview;
    duplicateSession.prepare(resultFixture(), duplicatePreview);
    duplicateSession.approve(duplicatePreview.exactRedactedJson);
    FakeTransport duplicateTransport;
    duplicateTransport.scripted = {
        {SubmissionTransportResult::DuplicateAccepted, "receipt-existing", true},
    };
    SubmissionReceipt receipt;
    check(duplicateSession.submit(duplicateTransport, receipt).succeeded() &&
              receipt.result == SubmissionTransportResult::DuplicateAccepted,
          "transport may idempotently acknowledge an already accepted submission");

    SubmissionSession malformedSession;
    SubmissionPreview malformedPreview;
    malformedSession.prepare(resultFixture(), malformedPreview);
    malformedSession.approve(malformedPreview.exactRedactedJson);
    FakeTransport malformedTransport;
    malformedTransport.scripted = {
        {SubmissionTransportResult::Accepted, "bad receipt with spaces", true},
    };
    receipt = {SubmissionTransportResult::RetryableFailure, "sentinel", false};
    check(malformedSession.submit(malformedTransport, receipt).code ==
              SubmissionCode::InvalidTransportResponse &&
              receipt.receiptId == "sentinel",
          "malformed remote receipt is rejected transactionally instead of becoming success evidence");
}

void testInvalidLocalResultNeverReachesTransport() {
    auto invalid = resultFixture();
    invalid.redaction.credentialsExcluded = false;
    SubmissionSession session;
    SubmissionPreview preview;
    check(session.prepare(invalid, preview).code == SubmissionCode::InvalidResult &&
              !session.localResult().has_value() && !session.preparedEnvelope().has_value(),
          "privacy-invalid local evidence cannot enter the submission state machine");
}

} // namespace

int main() {
    testPrepareIsDeterministicAndPreviewBound();
    testOfflineAndUnavailableDoNotTouchLocalResultOrTransport();
    testRetryIsExplicitAndReusesIdempotencyIdentity();
    testDuplicateAndMalformedReceiptsAreHandledFailClosed();
    testInvalidLocalResultNeverReachesTransport();
    if (failures != 0) {
        std::cerr << failures << " community submission test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Community submission tests passed.\n";
    return EXIT_SUCCESS;
}
