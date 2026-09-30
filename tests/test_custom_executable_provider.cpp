#include "hydra/custom_executable_provider.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::provider;
using namespace hydra::provider::custom;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

CustomExecutableDefinition definition() {
    return {L"Manual Game", L"C:\\Games\\Manual\\game.exe",
            {L"--profile", L"seat-1", L"literal && value"},
            L"C:\\Games\\Manual", L"C:\\Games\\Manual\\game.ico"};
}

class FakeCustomSource final : public CustomExecutableSource {
public:
    CustomExecutableSourceResult inspectResult{CustomExecutableSourceResult::Success};
    CustomExecutableObservation observation{
        L"C:\\Games\\Manual\\game.exe",
        L"C:\\Games\\Manual",
        L"C:\\Games\\Manual\\game.ico",
        4096u,
        123456u,
        catalog::ExecutableArchitecture::X64};
    std::vector<ProviderProcessEvidence> processes{
        {42u, 77u, L"C:\\Games\\Manual\\game.exe", true}};
    bool processFailure{false};
    int inspectCalls{0};
    int processCalls{0};

    CustomExecutableSourceResult inspect(
        const CustomExecutableDefinition&,
        CustomExecutableObservation& output,
        std::string& error) noexcept override {
        ++inspectCalls;
        if (inspectResult != CustomExecutableSourceResult::Success) {
            error = "fixture inspection failed";
            return inspectResult;
        }
        output = observation;
        return inspectResult;
    }

    bool observeProcesses(const std::wstring& executable,
                          std::vector<ProviderProcessEvidence>& output,
                          std::string& error) noexcept override {
        ++processCalls;
        if (processFailure || executable != observation.canonicalExecutablePath) {
            error = "fixture process observation failed";
            return false;
        }
        output = processes;
        return true;
    }
};

struct ResolvedGame {
    std::string gameId;
    std::string appId;
};

ResolvedGame resolveGame(CustomExecutableProviderAdapter& adapter) {
    std::vector<catalog::GameCatalogCandidate> candidates;
    if (!discoverInstalledGames(adapter, candidates).succeeded() || candidates.size() != 1u) {
        return {};
    }
    catalog::LocalGameCatalog catalog;
    if (!catalog::buildLocalGameCatalog(candidates, catalog).succeeded() ||
        catalog.entries.size() != 1u || !candidates[0].providerAppId) {
        return {};
    }
    return {catalog.entries[0].game.gameId, *candidates[0].providerAppId};
}

void testManualDefinitionBecomesCatalogAndLaunch() {
    auto source = std::make_shared<FakeCustomSource>();
    const auto configured = definition();
    CustomExecutableProviderAdapter adapter(source, configured);
    const auto descriptor = adapter.descriptor();
    check(descriptor.providerId == "custom" &&
              descriptor.availability == ProviderAvailability::Available &&
              descriptor.metadataRevision != 0u &&
              descriptor.capabilities.installedGameDiscovery &&
              descriptor.capabilities.launchRequests &&
              descriptor.capabilities.offlineLaunch &&
              descriptor.capabilities.processIdentification &&
              !descriptor.capabilities.accountReferences,
          "custom executable exposes only local typed capabilities");

    std::vector<catalog::GameCatalogCandidate> candidates;
    check(discoverInstalledGames(adapter, candidates).succeeded() &&
              candidates.size() == 1u,
          "validated manual executable becomes one catalog candidate");
    if (candidates.size() == 1u) {
        check(candidates[0].providerId == "custom" &&
                  candidates[0].providerAppId &&
                  candidates[0].title == configured.title &&
                  candidates[0].executableCandidates ==
                      std::vector<std::wstring>({source->observation.canonicalExecutablePath}) &&
                  candidates[0].origin == profile::GameOrigin::Manual &&
                  candidates[0].localIconSource ==
                      std::optional<std::wstring>(source->observation.canonicalIconSource),
              "manual candidate keeps canonical executable/icon identity and manual origin");
    }
    const auto game = resolveGame(adapter);
    check(!game.gameId.empty() && !game.appId.empty(),
          "manual candidate resolves to stable GameRecord identity");

    LaunchSelection selection{"custom", game.gameId, game.appId, std::nullopt,
                              descriptor.metadataRevision, configured.arguments};
    ProviderLaunchRequest launch;
    check(buildLaunchRequest(adapter, selection, launch).succeeded() &&
              launch.targetKind == LaunchTargetKind::Executable &&
              launch.target == source->observation.canonicalExecutablePath &&
              launch.arguments == configured.arguments &&
              launch.workingDirectory ==
                  std::optional<std::wstring>(
                      source->observation.canonicalWorkingDirectory),
          "manual executable launch is a path plus argument vector, never a shell string");
    check(launch.arguments.size() == 3u && launch.arguments[2] == L"literal && value",
          "shell metacharacters remain inert data inside one typed argument");
}

void testStaleAndMismatchedSelectionsFailClosed() {
    auto source = std::make_shared<FakeCustomSource>();
    const auto configured = definition();
    CustomExecutableProviderAdapter adapter(source, configured);
    const auto game = resolveGame(adapter);
    const auto oldRevision = adapter.descriptor().metadataRevision;
    source->observation.executableWriteTime += 1u;
    check(adapter.refresh().succeeded() &&
              adapter.descriptor().metadataRevision != oldRevision,
          "custom executable identity change advances metadata revision");

    ProviderLaunchRequest sentinel;
    sentinel.launchCorrelationId = "sentinel";
    const auto original = sentinel;
    LaunchSelection stale{"custom", game.gameId, game.appId, std::nullopt,
                          oldRevision, configured.arguments};
    check(buildLaunchRequest(adapter, stale, sentinel).result ==
                  ProviderResult::StaleMetadata && sentinel == original,
          "stale manual executable selection cannot replace a prior launch request");

    const auto refreshed = resolveGame(adapter);
    LaunchSelection mismatch{"custom", refreshed.gameId, refreshed.appId, std::nullopt,
                             adapter.descriptor().metadataRevision, {L"changed"}};
    check(buildLaunchRequest(adapter, mismatch, sentinel).result ==
                  ProviderResult::InvalidRequest && sentinel == original,
          "runtime arguments cannot silently diverge from validated manual definition");
}

void testMalformedMissingAndShellLikeDefinitionsFailBeforeSource() {
    auto source = std::make_shared<FakeCustomSource>();
    auto relative = definition();
    relative.executablePath = L"game.exe";
    CustomExecutableProviderAdapter invalid(source, relative);
    check(invalid.descriptor().availability == ProviderAvailability::Offline &&
              source->inspectCalls == 0,
          "relative executable path is rejected before filesystem observation");

    auto shell = definition();
    shell.executablePath = L"C:\\Windows\\System32\\cmd.exe /c game.exe";
    CustomExecutableProviderAdapter shellAdapter(source, shell);
    check(shellAdapter.descriptor().availability == ProviderAvailability::Offline &&
              source->inspectCalls == 0,
          "command-shell text cannot masquerade as an executable path");

    auto tooMany = definition();
    tooMany.arguments.assign(kMaximumLaunchArguments + 1u, L"x");
    CustomExecutableProviderAdapter bounded(source, tooMany);
    check(bounded.descriptor().availability == ProviderAvailability::Offline &&
              source->inspectCalls == 0,
          "manual executable argument count is bounded before source access");

    source->inspectResult = CustomExecutableSourceResult::NotFound;
    CustomExecutableProviderAdapter missing(source, definition());
    std::vector<catalog::GameCatalogCandidate> output;
    check(missing.descriptor().availability == ProviderAvailability::Absent &&
              discoverInstalledGames(missing, output).result ==
                  ProviderResult::ProviderAbsent,
          "missing manual executable is explicit and produces no catalog entry");

    source->inspectResult = CustomExecutableSourceResult::InvalidExecutable;
    CustomExecutableProviderAdapter nonPe(source, definition());
    check(nonPe.descriptor().availability == ProviderAvailability::Offline &&
              discoverInstalledGames(nonPe, output).result ==
                  ProviderResult::InvalidMetadata,
          "non-PE or unsupported executable identity fails closed");
}

void testProcessObservationRemainsCandidateEvidence() {
    auto source = std::make_shared<FakeCustomSource>();
    const auto configured = definition();
    CustomExecutableProviderAdapter adapter(source, configured);
    const auto game = resolveGame(adapter);
    const auto revision = adapter.descriptor().metadataRevision;
    const ProcessIdentificationQuery query{
        "custom", game.gameId, game.appId,
        "custom-0000000000000000", revision};
    std::vector<ProviderProcessEvidence> output;
    check(identifyProcesses(adapter, query, output).succeeded() && output.size() == 1u &&
              !output[0].providerRelationshipVerified && source->processCalls == 1,
          "manual exact-path process observation is not falsely promoted to launch causality");
}

int liveSmoke(const char* executableArgument) {
    std::error_code error;
    const auto executable = std::filesystem::absolute(executableArgument, error);
    if (error) {
        std::cerr << "Could not resolve custom-provider smoke executable\n";
        return EXIT_FAILURE;
    }
    CustomExecutableDefinition configured;
    configured.title = L"HydraSeat custom-provider smoke";
    configured.executablePath = executable.wstring();
    configured.workingDirectory = executable.parent_path().wstring();
    CustomExecutableProviderAdapter adapter(makeNativeCustomExecutableSource(), configured);
    const auto game = resolveGame(adapter);
    if (game.gameId.empty()) {
        std::cerr << "Native custom-provider read-only inspection failed\n";
        return EXIT_FAILURE;
    }
    ProviderLaunchRequest launch;
    const LaunchSelection selection{
        "custom", game.gameId, game.appId, std::nullopt,
        adapter.descriptor().metadataRevision, {}};
    const auto result = buildLaunchRequest(adapter, selection, launch);
    std::error_code equivalentError;
    const bool sameTarget = result.succeeded() &&
        std::filesystem::equivalent(std::filesystem::path(launch.target), executable,
                                    equivalentError) && !equivalentError;
    if (!sameTarget) {
        std::cerr << "Native custom-provider launch-plan construction failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Custom executable read-only live inspection passed; no process was started\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--live-smoke") {
        return liveSmoke(argv[0]);
    }

    testManualDefinitionBecomesCatalogAndLaunch();
    testStaleAndMismatchedSelectionsFailClosed();
    testMalformedMissingAndShellLikeDefinitionsFailBeforeSource();
    testProcessObservationRemainsCandidateEvidence();

    if (failures != 0) {
        std::cerr << failures << " custom executable provider test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "P6-PROV-03D custom executable provider tests passed\n";
    return EXIT_SUCCESS;
}
