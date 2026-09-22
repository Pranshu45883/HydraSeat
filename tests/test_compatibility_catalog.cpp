#include "hydra/compatibility_catalog.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::community;
using namespace hydra::compat;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string hash(char ch) { return std::string(64u, ch); }

GameSelector selector(std::string gameId = "game:catalog") {
    return {std::move(gameId), "steam", "100", "1.0"};
}

CompatibilityResult compatibility(std::string id,
                                  std::string bucket = "2026-08-20",
                                  EvidenceStatus input = EvidenceStatus::Pass) {
    CompatibilityResult value;
    value.resultId = std::move(id);
    value.timestampClass = TimestampClass::DayBucket;
    value.timestampBucket = std::move(bucket);
    value.gameId = "game:catalog";
    value.providerId = "steam";
    value.providerAppId = "100";
    value.gameVersion = "1.0";
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "build-a";
    value.windowsBuildClass = "win10-2009";
    value.architecture = "x64";
    value.scenario = Scenario::DifferentGames;
    value.backends = {{"raw-input", "1", EvidenceStatus::Pass}};
    value.launch = EvidenceStatus::Pass;
    value.inputIsolation = input;
    value.cleanExit = EvidenceStatus::Pass;
    if (input != EvidenceStatus::NotMeasured) {
        value.measurements.observedInputEvents = 100u;
        value.measurements.verifiedCrossSeatEvents = input == EvidenceStatus::Pass ? 0u : 1u;
    }
    value.origin = ResultOrigin::ControlledProcess;
    value.provenanceId = "fixture";
    value.provenanceRevision = 1u;
    return value;
}

profile::GameRecord game() {
    profile::GameRecord value;
    value.gameId = "game:catalog";
    value.providerId = "steam";
    value.providerAppId = "100";
    value.title = L"Catalog Fixture";
    value.installRoot = L"D:\\Games\\Catalog";
    value.executableCandidates = {L"D:\\Games\\Catalog\\game.exe"};
    value.localVersion = L"1.0";
    value.origin = profile::GameOrigin::Discovered;
    return value;
}

CommunitySetupEntry setup() {
    CommunitySetupEntry value;
    value.entryId = "setup-a";
    value.packageId = "catalog-package";
    value.packageRevision = 2u;
    value.selector = selector();
    value.knownLimitations = {"Needs two local data directories."};
    value.evidenceResultIds = {"result-a"};
    value.sourceId = "community-reviewed";
    value.licenseId = "CC0-1.0";
    value.authorAttribution = "Fixture Contributor";

    profile::TwoPlayerSetup source;
    source.setupId = "two-player";
    source.gameId = "game:catalog";
    source.displayName = L"Two player";
    source.instances = {
        {{L"--seat=1"}, L"C:\\Source\\Game", L"C:\\Source\\Data1"},
        {{L"--seat=2"}, L"C:\\Source\\Game", L"C:\\Source\\Data2"},
    };
    const auto exported = portable::exportSetup(
        source, game(), {value.sourceId, value.packageRevision, "fixture-exporter"},
        value.setupPackage);
    check(exported.succeeded(), "catalog setup fixture exports");
    return value;
}

CommunityPackageManifest manifest() {
    CommunityPackageManifest value;
    value.packageId = "catalog-package";
    value.packageRevision = 2u;
    value.packageVersion = "2.0.0";
    value.packageSha256 = hash('a');
    value.sourceId = "community-reviewed";
    value.licenseId = "CC0-1.0";
    value.redistributionAllowed = true;

    PackageEntry resultA;
    resultA.entryId = "result-a";
    resultA.kind = EntryKind::CompatibilityResult;
    resultA.contentSchemaVersion = kCompatibilityResultSchemaVersion;
    resultA.expectedSha256 = hash('b');
    resultA.selector = selector();
    PackageEntry resultB = resultA;
    resultB.entryId = "result-b";
    resultB.expectedSha256 = hash('c');
    PackageEntry setupEntry;
    setupEntry.entryId = "setup-a";
    setupEntry.kind = EntryKind::TwoPlayerSetup;
    setupEntry.contentSchemaVersion = portable::kSetupPackageVersion;
    setupEntry.expectedSha256 = hash('d');
    setupEntry.selector = selector();
    value.entries = {resultA, resultB, setupEntry};
    return value;
}

void testOfflineSnapshotAndGameQuery() {
    const std::vector<CompatibilityResult> results{
        compatibility("result-a"), compatibility("result-b", "2026-03-10")};
    const std::vector<CommunitySetupEntry> setups{setup()};
    LocalCatalogSnapshot snapshot;
    check(buildLocalCatalogSnapshot(manifest(), results, setups, snapshot).succeeded(),
          "trusted decoded package payloads build an offline local snapshot");
    check(snapshot.results.size() == 2u && snapshot.setups.size() == 1u,
          "offline snapshot retains declared result/setup data only");

    GameEvidenceView view;
    const AggregationPolicy policy{"2026-08", 3u};
    check(queryGameEvidence(snapshot, selector(), policy, view).succeeded(),
          "game evidence query works with no network client or remote service");
    check(view.totalResultSamples == 2u && view.currentResultSamples == 1u &&
              view.staleResultSamples == 1u && view.setups.size() == 1u,
          "local query reports explicit sample/current/stale counts and setup candidate");
}

void testPayloadOrderingIsCanonical() {
    auto results = std::vector<CompatibilityResult>{
        compatibility("result-b", "2026-03-10"), compatibility("result-a")};
    auto setups = std::vector<CommunitySetupEntry>{setup()};
    LocalCatalogSnapshot first;
    check(buildLocalCatalogSnapshot(manifest(), results, setups, first).succeeded(),
          "reverse payload order snapshot builds");
    std::reverse(results.begin(), results.end());
    LocalCatalogSnapshot second;
    check(buildLocalCatalogSnapshot(manifest(), results, setups, second).succeeded() &&
              second == first,
          "decoded payload input order cannot change local catalog rendering state");
}

void testUndeclaredKindAndSelectorMismatchFailTransactionally() {
    LocalCatalogSnapshot output;
    output.manifest.packageId = "sentinel";
    const auto sentinel = output;
    const std::vector<CompatibilityResult> validResults{
        compatibility("result-a"), compatibility("result-b", "2026-03-10")};
    const std::vector<CommunitySetupEntry> validSetups{setup()};

    auto package = manifest();
    package.entries[0].kind = EntryKind::CatalogSnapshot;
    check(buildLocalCatalogSnapshot(package, validResults, validSetups, output).code ==
              CatalogCode::KindMismatch && output == sentinel,
          "manifest payload-kind confusion fails without replacing local catalog");

    package = manifest();
    package.entries[0].selector.gameVersion = "2.0";
    check(buildLocalCatalogSnapshot(package, validResults, validSetups, output).code ==
              CatalogCode::SelectorMismatch && output == sentinel,
          "manifest/result selector disagreement fails closed");

    auto undeclared = validResults;
    undeclared.push_back(compatibility("result-extra"));
    check(buildLocalCatalogSnapshot(manifest(), undeclared, validSetups, output).code ==
              CatalogCode::MissingManifestEntry && output == sentinel,
          "decoded result absent from signed/trusted manifest is ignored by failing closed");
}

void testUnknownGameRendersEmptyEvidenceNotFailure() {
    const std::vector<CompatibilityResult> results{
        compatibility("result-a"), compatibility("result-b", "2026-03-10")};
    const std::vector<CommunitySetupEntry> setups{setup()};
    LocalCatalogSnapshot snapshot;
    buildLocalCatalogSnapshot(manifest(), results, setups, snapshot);
    GameEvidenceView view;
    const AggregationPolicy policy{"2026-08", 3u};
    check(queryGameEvidence(snapshot, selector("game:unknown"), policy, view).succeeded() &&
              view.totalResultSamples == 0u && view.cohorts.empty() && view.setups.empty(),
          "unknown local game is explicitly untested rather than converted to a compatibility failure");
}

} // namespace

int main() {
    testOfflineSnapshotAndGameQuery();
    testPayloadOrderingIsCanonical();
    testUndeclaredKindAndSelectorMismatchFailTransactionally();
    testUnknownGameRendersEmptyEvidenceNotFailure();
    if (failures != 0) {
        std::cerr << failures << " compatibility catalog test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Compatibility catalog tests passed.\n";
    return EXIT_SUCCESS;
}
