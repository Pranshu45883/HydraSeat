#include "hydra/community_package.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
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

std::string hash(char ch) {
    return std::string(64u, ch);
}

trust::TrustPolicy policy() {
    trust::TrustPolicy value;
    value.hostArchitecture = trust::ArtifactArchitecture::X64;
    value.requireRedistributionPermission = true;
    value.allowedSourceIds = {"community-reviewed"};
    value.allowedCapabilities = {"compatibility-data", "two-player-setup"};
    return value;
}

PackageEntry resultEntry(std::string id = "result-a", char hashChar = 'b') {
    PackageEntry entry;
    entry.entryId = std::move(id);
    entry.kind = EntryKind::CompatibilityResult;
    entry.contentSchemaVersion = compat::kCompatibilityResultSchemaVersion;
    entry.expectedSha256 = hash(hashChar);
    entry.selector.gameId = "game:a";
    entry.selector.providerId = "steam";
    entry.selector.providerAppId = "123";
    entry.selector.gameVersion = "1.0";
    return entry;
}

PackageEntry setupEntry(std::string id = "setup-a", char hashChar = 'c') {
    PackageEntry entry;
    entry.entryId = std::move(id);
    entry.kind = EntryKind::TwoPlayerSetup;
    entry.contentSchemaVersion = portable::kSetupPackageVersion;
    entry.expectedSha256 = hash(hashChar);
    entry.selector.gameId = "game:a";
    entry.selector.providerId = "steam";
    entry.selector.providerAppId = "123";
    entry.selector.gameVersion = "1.0";
    return entry;
}

CommunityPackageManifest manifest(std::uint64_t revision = 1u,
                                  std::string version = "1.0.0",
                                  char packageHash = 'a') {
    CommunityPackageManifest value;
    value.packageId = "community-seed";
    value.packageRevision = revision;
    value.packageVersion = std::move(version);
    value.packageSha256 = hash(packageHash);
    value.sourceId = "community-reviewed";
    value.licenseId = "CC0-1.0";
    value.redistributionAllowed = true;
    value.entries = {resultEntry(), setupEntry()};
    return value;
}

std::vector<EntryObservation> observations() {
    return {{"result-a", hash('b')}, {"setup-a", hash('c')}};
}

void testInstallUpdateRollbackLifecycle() {
    CommunityPackageStore store;
    auto first = manifest();
    check(store.installOrUpdate(first, first.packageSha256, observations(), policy()).succeeded(),
          "valid data-only community package installs");
    check(store.current() && store.current()->packageRevision == 1u && store.history().empty(),
          "first install becomes current without synthetic rollback state");

    auto second = manifest(2u, "1.1.0", 'd');
    second.entries[0].expectedSha256 = hash('e');
    std::vector<EntryObservation> secondObserved{{"result-a", hash('e')},
                                                  {"setup-a", hash('c')}};
    check(store.installOrUpdate(second, second.packageSha256, secondObserved, policy()).succeeded(),
          "higher package revision updates transactionally");
    check(store.current()->packageRevision == 2u && store.history().size() == 1u &&
              store.history().back().packageRevision == 1u,
          "previous valid revision is retained for rollback");

    check(store.rollback().succeeded() && store.current()->packageRevision == 1u &&
              store.history().empty(),
          "rollback restores the immediately previous valid revision");
    check(store.rollback().code == PackageCode::NoRollbackVersion,
          "rollback does not toggle forward into the discarded revision");
}

void testTamperMissingUnexpectedAndStaleAreTransactional() {
    CommunityPackageStore store;
    auto first = manifest();
    check(store.installOrUpdate(first, first.packageSha256, observations(), policy()).succeeded(),
          "baseline community package installs");
    const auto beforeCurrent = *store.current();
    const auto beforeHistory = store.history();

    auto tampered = manifest(2u, "1.1.0", 'd');
    check(store.installOrUpdate(tampered, hash('f'), observations(), policy()).code ==
              PackageCode::PackageTrustRejected &&
              *store.current() == beforeCurrent && store.history() == beforeHistory,
          "package-level hash tamper leaves last valid cache untouched");

    auto entryTampered = observations();
    entryTampered[0].observedSha256 = hash('f');
    check(store.installOrUpdate(tampered, tampered.packageSha256, entryTampered, policy()).code ==
              PackageCode::EntryHashMismatch &&
              *store.current() == beforeCurrent,
          "entry-level tamper is rejected transactionally");

    auto missing = observations();
    missing.pop_back();
    check(store.installOrUpdate(tampered, tampered.packageSha256, missing, policy()).code ==
              PackageCode::MissingEntry &&
              *store.current() == beforeCurrent,
          "missing package member cannot partially install");

    auto extra = observations();
    extra.push_back({"extra", hash('f')});
    check(store.installOrUpdate(tampered, tampered.packageSha256, extra, policy()).code ==
              PackageCode::UnexpectedEntry &&
              *store.current() == beforeCurrent,
          "undeclared package member is rejected");

    check(store.installOrUpdate(first, first.packageSha256, observations(), policy()).code ==
              PackageCode::StaleRevision &&
              *store.current() == beforeCurrent,
          "same/older revision cannot overwrite current trusted package");
}

void testManifestHasNoExecutablePrivilegeAndRejectsBadSelectors() {
    auto value = manifest();
    check(validateCommunityPackageManifest(value).succeeded(),
          "valid manifest contains only data entry kinds");
    for (const auto& entry : value.entries) {
        check(entry.kind != static_cast<EntryKind>(3u),
              "fixture exposes no executable/script entry kind");
    }

    value.entries[0].selector.gameVersion = "C:\\Users\\Alice\\private";
    check(validateCommunityPackageManifest(value).code == PackageCode::InvalidEntry,
          "absolute private path cannot masquerade as game version selector");

    value = manifest();
    value.entries.push_back(value.entries[0]);
    check(validateCommunityPackageManifest(value).code == PackageCode::DuplicateEntry,
          "duplicate package entry identity is rejected deterministically");

    value = manifest();
    check(value.entries[1].contentSchemaVersion == portable::kSetupPackageVersion &&
              validateCommunityPackageManifest(value).succeeded(),
          "current setup-package v2 content schema is accepted explicitly");

    value.entries[1].contentSchemaVersion = portable::kLegacySetupPackageVersion;
    check(validateCommunityPackageManifest(value).succeeded(),
          "legacy setup-package v1 content schema remains explicitly supported");

    value.entries[1].contentSchemaVersion = portable::kSetupPackageVersion + 1u;
    check(validateCommunityPackageManifest(value).code == PackageCode::InvalidEntry,
          "unknown future setup-package schema cannot inherit community-package trust silently");

    value = manifest();
    value.entries[0].contentSchemaVersion = 999u;
    check(validateCommunityPackageManifest(value).code == PackageCode::InvalidEntry,
          "unknown future result entry schema cannot inherit trust silently");

    value = manifest();
    value.minimumProfileSchema = profile::kProfileSchemaVersion + 1u;
    value.maximumProfileSchema = value.minimumProfileSchema;
    check(validateCommunityPackageManifest(value).code == PackageCode::UnsupportedSchemaRange,
          "package incompatible with current HydraSeat profile schema is disabled");
}

void testTrustPolicyAndClear() {
    CommunityPackageStore store;
    auto value = manifest();
    auto denied = policy();
    denied.allowedSourceIds.clear();
    check(store.installOrUpdate(value, value.packageSha256, observations(), denied).code ==
              PackageCode::PackageTrustRejected &&
              !store.current(),
          "untrusted provenance cannot install a community package");

    check(store.installOrUpdate(value, value.packageSha256, observations(), policy()).succeeded(),
          "trusted package can install after prior rejection");
    store.clear();
    check(!store.current() && store.history().empty(),
          "clear removes optional package state without changing core profile/runtime state");
    check(store.rollback().code == PackageCode::NoInstalledPackage,
          "rollback after explicit clear reports no installed package");
}

} // namespace

int main() {
    testInstallUpdateRollbackLifecycle();
    testTamperMissingUnexpectedAndStaleAreTransactional();
    testManifestHasNoExecutablePrivilegeAndRejectsBadSelectors();
    testTrustPolicyAndClear();
    if (failures != 0) {
        std::cerr << failures << " community package test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Community package lifecycle tests passed.\n";
    return EXIT_SUCCESS;
}
