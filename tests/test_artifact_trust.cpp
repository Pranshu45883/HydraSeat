#include "hydra/artifact_trust.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace hydra::trust;
constexpr std::string_view kPublisher = "0123456789ABCDEF0123456789ABCDEF01234567";

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string hash(char value = 'a') {
    return std::string(64u, value);
}

TrustPolicy policy() {
    TrustPolicy value;
    value.hostArchitecture = ArtifactArchitecture::X64;
    value.allowedSourceIds = {"official", "community-reviewed"};
    value.allowedCapabilities = {"compatibility-data", "two-player-setup", "provider-helper"};
    value.trustedPublisherIdentities = {std::string(kPublisher)};
    return value;
}

ArtifactManifest dataManifest() {
    ArtifactManifest value;
    value.artifactId = "catalog-v1";
    value.artifactClass = ArtifactClass::DataCatalog;
    value.artifactVersion = "1.0.0";
    value.architecture = ArtifactArchitecture::Any;
    value.expectedSha256 = hash('a');
    value.sourceId = "community-reviewed";
    value.licenseId = "CC0-1.0";
    value.redistributionAllowed = true;
    value.optional = true;
    value.capabilityScope = {"compatibility-data"};
    return value;
}

ArtifactObservation dataObservation() {
    return {true, "1.0.0", ArtifactArchitecture::Any, hash('a'),
            SignatureState::NotApplicable, {}};
}

ArtifactManifest executableManifest() {
    ArtifactManifest value;
    value.artifactId = "provider-helper-v1";
    value.artifactClass = ArtifactClass::ProviderHelper;
    value.artifactVersion = "1.2.0";
    value.architecture = ArtifactArchitecture::X64;
    value.expectedSha256 = hash('b');
    value.sourceId = "official";
    value.licenseId = "HydraSeat-Project";
    value.optional = true;
    value.requiresInstall = true;
    value.requiresRestart = true;
    value.requiresRecoveryPlan = true;
    value.capabilityScope = {"provider-helper"};
    return value;
}

ArtifactObservation executableObservation() {
    return {true, "1.2.0", ArtifactArchitecture::X64, hash('b'),
            SignatureState::ValidTrustedPublisher, std::string(kPublisher)};
}

void testDataAndExecutableTrustClassesStayDifferent() {
    check(evaluateArtifact(dataManifest(), dataObservation(), policy()).accepted(),
          "data catalog can pass exact hash/provenance/license policy without executable signing");
    check(evaluateArtifact(executableManifest(), executableObservation(), policy()).accepted(),
          "executable helper requires and passes exact architecture plus trusted signing");

    auto badData = dataManifest();
    badData.requiresInstall = true;
    const auto dataResult = evaluateArtifact(badData, dataObservation(), policy());
    check(dataResult.decision == TrustDecision::Reject &&
              dataResult.code == TrustCode::InvalidManifest,
          "data-only catalog cannot silently acquire install/executable privileges");
}

void testTamperVersionArchitectureAndPublisherFailClosed() {
    auto tampered = dataObservation();
    tampered.observedSha256 = hash('c');
    check(evaluateArtifact(dataManifest(), tampered, policy()).code == TrustCode::HashMismatch,
          "tampered data hash is rejected deterministically");

    auto wrongVersion = executableObservation();
    wrongVersion.artifactVersion = "1.3.0";
    check(evaluateArtifact(executableManifest(), wrongVersion, policy()).code ==
              TrustCode::VersionMismatch,
          "unexpected executable version cannot inherit prior trust");

    auto wrongArch = executableObservation();
    wrongArch.architecture = ArtifactArchitecture::X86;
    check(evaluateArtifact(executableManifest(), wrongArch, policy()).code ==
              TrustCode::ArchitectureMismatch,
          "wrong architecture is rejected before use");

    auto unknownPublisher = executableObservation();
    unknownPublisher.signature = SignatureState::UnknownPublisher;
    check(evaluateArtifact(executableManifest(), unknownPublisher, policy()).code ==
              TrustCode::SignatureInvalid,
          "unknown executable publisher cannot silently become trusted");

    auto wrongPublisher = executableObservation();
    wrongPublisher.publisherIdentity = "89ABCDEF0123456789ABCDEF0123456789ABCDEF";
    check(evaluateArtifact(executableManifest(), wrongPublisher, policy()).code ==
              TrustCode::PublisherIdentityMismatch,
          "validly signed executable from a different exact publisher is rejected");

    auto missingPublisher = executableObservation();
    missingPublisher.publisherIdentity.clear();
    check(evaluateArtifact(executableManifest(), missingPublisher, policy()).code ==
              TrustCode::SignatureMetadataMalformed,
          "valid signature metadata without exact publisher identity is malformed");

    auto noPublisherPolicy = policy();
    noPublisherPolicy.trustedPublisherIdentities.clear();
    check(evaluateArtifact(executableManifest(), executableObservation(), noPublisherPolicy).code ==
              TrustCode::PublisherIdentityRequired,
          "signed status alone is insufficient without an exact publisher trust anchor");
}

void testUnsignedDevelopmentExceptionIsExplicitAndNarrow() {
    auto manifest = executableManifest();
    manifest.developmentBuild = true;
    auto observation = executableObservation();
    observation.signature = SignatureState::Missing;
    observation.publisherIdentity.clear();

    auto defaultPolicy = policy();
    check(evaluateArtifact(manifest, observation, defaultPolicy).code == TrustCode::SignatureRequired,
          "unsigned development executable is rejected by default");

    auto developmentPolicy = policy();
    developmentPolicy.allowUnsignedDevelopmentExecutables = true;
    check(evaluateArtifact(manifest, observation, developmentPolicy).accepted(),
          "explicit development policy may admit exact-hash unsigned development executable");

    manifest.developmentBuild = false;
    check(evaluateArtifact(manifest, observation, developmentPolicy).code ==
              TrustCode::SignatureRequired,
          "unsigned exception does not widen to ordinary/release executable artifacts");
}

void testCapabilitySourceLicenseAndRecoveryPolicy() {
    auto manifest = executableManifest();
    manifest.capabilityScope.push_back("kernel-superpower");
    check(evaluateArtifact(manifest, executableObservation(), policy()).code ==
              TrustCode::CapabilityNotAllowed,
          "policy-disallowed capability scope is rejected");

    manifest = executableManifest();
    manifest.sourceId = "unknown-source";
    check(evaluateArtifact(manifest, executableObservation(), policy()).code ==
              TrustCode::SourceNotAllowed,
          "unapproved artifact provenance is rejected");

    manifest = executableManifest();
    manifest.requiresRecoveryPlan = false;
    check(evaluateArtifact(manifest, executableObservation(), policy()).code ==
              TrustCode::RecoveryPlanRequired,
          "install/restart executable cannot omit recovery plan");

    auto data = dataManifest();
    data.redistributionAllowed = false;
    auto strictPolicy = policy();
    strictPolicy.requireRedistributionPermission = true;
    check(evaluateArtifact(data, dataObservation(), strictPolicy).code ==
              TrustCode::RedistributionNotAllowed,
          "distribution policy can require explicit redistribution permission metadata");
}

void testOptionalAbsenceKeepsCoreUsable() {
    auto absent = dataObservation();
    absent.present = false;
    const auto optional = evaluateArtifact(dataManifest(), absent, policy());
    check(optional.decision == TrustDecision::OptionalAbsent && optional.accepted(),
          "absent optional data catalog is non-fatal to offline/core operation");

    auto required = dataManifest();
    required.optional = false;
    const auto missing = evaluateArtifact(required, absent, policy());
    check(missing.decision == TrustDecision::Reject &&
              missing.code == TrustCode::RequiredArtifactMissing,
          "required artifact absence remains explicit rather than silently downgraded");
}

} // namespace

int main() {
    testDataAndExecutableTrustClassesStayDifferent();
    testTamperVersionArchitectureAndPublisherFailClosed();
    testUnsignedDevelopmentExceptionIsExplicitAndNarrow();
    testCapabilitySourceLicenseAndRecoveryPolicy();
    testOptionalAbsenceKeepsCoreUsable();
    if (failures != 0) {
        std::cerr << failures << " artifact trust test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Artifact trust policy tests passed.\n";
    return EXIT_SUCCESS;
}
