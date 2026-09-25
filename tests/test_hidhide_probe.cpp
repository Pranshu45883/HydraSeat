#include "hydra/hidhide_probe.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using hydra::HidHideAvailability;
using hydra::HidHideBooleanQueryResult;
using hydra::HidHideControlReadResult;
using hydra::HidHideEvidenceResult;
using hydra::HidHidePlatformStatus;
using hydra::HidHideProbeDiagnostic;
using hydra::HidHideProbePlatform;
using hydra::HidHideVersion;
using hydra::HidHideVersionResult;

enum class FakeOperation {
    InstallationEvidence,
    ControlInterfaceEvidence,
    DriverVersion,
    ReadOnlyControlState
};

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class FakePlatform final : public HidHideProbePlatform {
public:
    HidHideEvidenceResult installation{
        HidHidePlatformStatus::Success, 0};
    HidHideEvidenceResult interface{
        HidHidePlatformStatus::Success, 0};
    HidHideVersionResult version{
        HidHidePlatformStatus::Success,
        HidHideVersion{1, 7, 346, 0}, 0};
    HidHideControlReadResult control{
        HidHidePlatformStatus::Success,
        {HidHidePlatformStatus::Success, {1}, 0},
        {HidHidePlatformStatus::Success, {0}, 0},
        0};

    std::vector<FakeOperation> operations;
    std::size_t mutationCalls{0};
    std::size_t elevationRequests{0};

    HidHideEvidenceResult installationEvidence() override {
        operations.push_back(FakeOperation::InstallationEvidence);
        return installation;
    }
    HidHideEvidenceResult controlInterfaceEvidence() override {
        operations.push_back(FakeOperation::ControlInterfaceEvidence);
        return interface;
    }
    HidHideVersionResult driverVersion() override {
        operations.push_back(FakeOperation::DriverVersion);
        return version;
    }
    HidHideControlReadResult queryControlStateReadOnly() override {
        operations.push_back(FakeOperation::ReadOnlyControlState);
        return control;
    }
};

bool called(const FakePlatform& platform, FakeOperation operation) {
    return std::find(
               platform.operations.begin(), platform.operations.end(),
               operation) != platform.operations.end();
}

void testNoEvidenceIsUnavailable() {
    FakePlatform platform;
    platform.installation = {HidHidePlatformStatus::NotFound, 0};
    platform.interface = {HidHidePlatformStatus::NotFound, 0};

    const auto report = hydra::probeHidHide(platform);
    check(report.availability == HidHideAvailability::Unavailable,
          "no evidence reports unavailable");
    check(report.diagnostic == HidHideProbeDiagnostic::NotDetected,
          "no evidence has a stable not-detected diagnostic");
    check(!called(platform, FakeOperation::DriverVersion) &&
              !called(platform, FakeOperation::ReadOnlyControlState),
          "no evidence performs no version or device-control query");
}

void testInstalledWithoutInterfaceIsUnverified() {
    FakePlatform platform;
    platform.interface = {HidHidePlatformStatus::NotFound, 0};

    const auto report = hydra::probeHidHide(platform);
    check(report.availability == HidHideAvailability::InstalledUnverified,
          "installed evidence without an interface is unverified");
    check(report.installedEvidence && !report.controlInterfacePresent,
          "report separates installation and interface evidence");
    check(report.diagnostic ==
              HidHideProbeDiagnostic::ControlInterfaceAbsent,
          "missing interface is explicit");
}

void testOrdinaryInstallationQueryFailureIsExplicit() {
    FakePlatform platform;
    platform.installation = {HidHidePlatformStatus::Failed, 1234};
    platform.interface = {HidHidePlatformStatus::NotFound, 0};

    const auto report = hydra::probeHidHide(platform);
    check(report.availability == HidHideAvailability::InstalledUnverified,
          "ordinary OS query failure does not become unavailable");
    check(report.diagnostic ==
              HidHideProbeDiagnostic::InstallationQueryFailed &&
              report.systemError == 1234,
          "ordinary OS query failure is explicit and bounded");
}

void testAccessDeniedNeverVerifiesOrElevates() {
    FakePlatform platform;
    platform.control.openStatus = HidHidePlatformStatus::AccessDenied;
    platform.control.systemError = 5;

    const auto report = hydra::probeHidHide(platform);
    check(report.availability == HidHideAvailability::InstalledUnverified,
          "access denied never verifies support");
    check(report.diagnostic == HidHideProbeDiagnostic::AccessDenied &&
              report.systemError == 5,
          "access denied preserves a bounded system error");
    check(platform.elevationRequests == 0,
          "probe never requests elevation");
}

void testRecognizedContractVerifiesReadOnlyState() {
    FakePlatform platform;
    platform.control.active.response = {0};
    platform.control.inverseWhitelist.response = {1};

    const auto report = hydra::probeHidHide(platform);
    check(report.availability == HidHideAvailability::VerifiedSupported,
          "recognized version plus readable contract verifies support");
    check(report.active.has_value() && !*report.active,
          "read-only active false is retained");
    check(report.inverseWhitelist.has_value() &&
              *report.inverseWhitelist,
          "read-only inverse true is retained");
    check(report.sessionBlacklistSupported,
          "known contract infers session capability without invoking it");
    check(platform.operations == std::vector<FakeOperation>{
              FakeOperation::InstallationEvidence,
              FakeOperation::ControlInterfaceEvidence,
              FakeOperation::DriverVersion,
              FakeOperation::ReadOnlyControlState},
          "verified probe performs only the four ordered read-only operations");
    check(platform.mutationCalls == 0,
          "verified probe still records zero mutation calls");
}

void testUnknownVersionFailsClosedBeforeControlQuery() {
    FakePlatform platform;
    platform.version.version = HidHideVersion{1, 7, 347, 0};

    const auto report = hydra::probeHidHide(platform);
    check(report.availability == HidHideAvailability::InstalledUnverified,
          "unknown version remains unverified");
    check(report.diagnostic == HidHideProbeDiagnostic::UnsupportedVersion,
          "unknown version is explicit");
    check(!report.sessionBlacklistSupported,
          "unknown version never infers session capability");
    check(!called(platform, FakeOperation::ReadOnlyControlState),
          "unknown version performs no IOCTL query");
}

void testMalformedAndTruncatedResponseFailsClosed() {
    FakePlatform platform;
    platform.control.active.response.clear();

    const auto report = hydra::probeHidHide(platform);
    check(report.availability == HidHideAvailability::InstalledUnverified,
          "truncated response remains unverified");
    check(report.diagnostic == HidHideProbeDiagnostic::MalformedResponse,
          "truncated response is malformed");
    check(!report.sessionBlacklistSupported,
          "malformed response cannot advertise session capability");
}

void testExcessiveResponseIsRejected() {
    FakePlatform platform;
    platform.control.active.response.assign(
        hydra::kHidHideMaxControlResponseBytes + 1, 0);

    const auto report = hydra::probeHidHide(platform);
    check(report.availability == HidHideAvailability::InstalledUnverified,
          "excessive response remains unverified");
    check(report.diagnostic == HidHideProbeDiagnostic::ResponseTooLarge,
          "excessive response has a bounded diagnostic");
}

void testSupportedVersionsAreExact() {
    check(hydra::isKnownSupportedHidHideVersion({1, 7, 339, 0}),
          "tagged 1.7.339.0 contract is recognized");
    check(hydra::isKnownSupportedHidHideVersion({1, 7, 344, 0}),
          "tagged 1.7.344.0 contract is recognized");
    check(hydra::isKnownSupportedHidHideVersion({1, 7, 346, 0}),
          "tagged 1.7.346.0 contract is recognized");
    check(!hydra::isKnownSupportedHidHideVersion({1, 5, 230, 0}),
          "older release without the contract is unverified");
    check(!hydra::isKnownSupportedHidHideVersion({1, 7, 346, 1}),
          "nearby build is not guessed compatible");
}

void testPublicReportContainsNoPrivateListsOrPaths() {
    FakePlatform platform;
    const auto report = hydra::probeHidHide(platform);
    const auto text = hydra::formatHidHideProbeReport(report);

    check(text.find("device_path:") == std::string::npos,
          "report contains no control or device path");
    check(text.find("allowlist_contents") == std::string::npos &&
              text.find("denylist_contents") == std::string::npos &&
              text.find("session_list_contents") == std::string::npos,
          "report contains no private list contents");
    check(text.size() < 2048,
          "CLI report remains bounded");
}

void testRepeatedProbeIsDeterministicAndReadOnly() {
    FakePlatform platform;
    const auto first = hydra::probeHidHide(platform);
    const auto second = hydra::probeHidHide(platform);

    check(first == second,
          "repeated probe returns the same report");
    check(platform.mutationCalls == 0 &&
              platform.elevationRequests == 0,
          "repeated probe leaves mutation/elevation counters at zero");
}

void testUnsupportedPlatformIsUnavailable() {
    FakePlatform platform;
    platform.installation = {
        HidHidePlatformStatus::UnsupportedPlatform, 0};

    const auto report = hydra::probeHidHide(platform);
    check(report.availability == HidHideAvailability::Unavailable &&
              report.diagnostic ==
                  HidHideProbeDiagnostic::UnsupportedPlatform,
          "unsupported platform is explicitly unavailable");
    check(platform.operations.size() == 1,
          "unsupported platform stops before Windows-only queries");

#if !defined(_WIN32)
    const auto native = hydra::probeHidHide();
    check(native.availability == HidHideAvailability::Unavailable &&
              native.diagnostic ==
                  HidHideProbeDiagnostic::UnsupportedPlatform,
          "native non-Windows behavior does not fake success");
#endif
}

} // namespace

int main() {
    testNoEvidenceIsUnavailable();
    testInstalledWithoutInterfaceIsUnverified();
    testOrdinaryInstallationQueryFailureIsExplicit();
    testAccessDeniedNeverVerifiesOrElevates();
    testRecognizedContractVerifiesReadOnlyState();
    testUnknownVersionFailsClosedBeforeControlQuery();
    testMalformedAndTruncatedResponseFailsClosed();
    testExcessiveResponseIsRejected();
    testSupportedVersionsAreExact();
    testPublicReportContainsNoPrivateListsOrPaths();
    testRepeatedProbeIsDeterministicAndReadOnly();
    testUnsupportedPlatformIsUnavailable();

    std::cout << "HidHide probe tests passed.\n";
    return EXIT_SUCCESS;
}
