#include "hydra/provider_adapter.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::provider;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

catalog::GameCatalogCandidate game(std::string appId = "123456") {
    catalog::GameCatalogCandidate value;
    value.providerId = "fake";
    value.providerAppId = std::move(appId);
    value.title = L"Fixture Game";
    value.installRoot = L"C:\\Games\\Fixture";
    value.executableCandidates = {L"C:\\Games\\Fixture\\game.exe"};
    value.origin = profile::GameOrigin::Discovered;
    value.staleness = catalog::CatalogStaleness::Current;
    return value;
}

class FakeProvider final : public LauncherProviderAdapter {
public:
    ProviderDescriptor descriptorValue{
        "fake", ProviderAvailability::Available, 7u,
        {true, true, true, false, true}};
    DiscoveryResponse discovery{ProviderResult::Success, 7u, {game()}, {}};
    AccountReferenceResponse accounts{
        ProviderResult::Success, 7u, {{"fake", "account-b"}, {"fake", "account-a"}}, {}};
    LaunchResponse launch{
        ProviderResult::Success,
        {"fake", "game:fixture", "123456", "account-a", 7u,
         LaunchTargetKind::ProviderUri, L"fake://launch/123456",
         {L"--fixture"}, std::nullopt, "launch-1"},
        {}};
    ProcessIdentificationResponse process{
        ProviderResult::Success, 7u,
        {{100u, 200u, L"C:\\Games\\Fixture\\game.exe", true}}, {}};

    int discoveryCalls{0};
    int accountCalls{0};
    int launchCalls{0};
    int processCalls{0};

    ProviderDescriptor descriptor() const noexcept override { return descriptorValue; }
    DiscoveryResponse discoverInstalledGames() noexcept override {
        ++discoveryCalls;
        return discovery;
    }
    AccountReferenceResponse listAccountReferences() noexcept override {
        ++accountCalls;
        return accounts;
    }
    LaunchResponse buildLaunchRequest(const LaunchSelection&) noexcept override {
        ++launchCalls;
        return launch;
    }
    ProcessIdentificationResponse identifyProcesses(
        const ProcessIdentificationQuery&) noexcept override {
        ++processCalls;
        return process;
    }
};

LaunchSelection selection() {
    return {"fake", "game:fixture", "123456", "account-a", 7u, {L"--seat=1"}};
}

void testDeterministicProviderContract() {
    FakeProvider provider;
    std::vector<catalog::GameCatalogCandidate> discovered;
    check(discoverInstalledGames(provider, discovered).succeeded() &&
              discovered == provider.discovery.candidates,
          "valid read-only provider discovery passes catalog validation");

    std::vector<catalog::GameCatalogCandidate> repeated;
    check(discoverInstalledGames(provider, repeated).succeeded() && repeated == discovered,
          "fake provider discovery is deterministic for one metadata revision");

    std::vector<profile::ProviderAccountReference> accounts;
    const std::vector<profile::ProviderAccountReference> expectedAccounts{
        {"fake", "account-a"}, {"fake", "account-b"}};
    check(listAccountReferences(provider, accounts).succeeded() &&
              accounts == expectedAccounts,
          "bounded account references are canonicalized without credentials");

    ProviderLaunchRequest launch;
    check(buildLaunchRequest(provider, selection(), launch).succeeded() &&
              launch == provider.launch.request,
          "typed URI plus argument-vector launch request passes validation");

    std::vector<ProviderProcessEvidence> processes;
    const ProcessIdentificationQuery query{
        "fake", "game:fixture", "123456", "launch-1", 7u};
    check(identifyProcesses(provider, query, processes).succeeded() &&
              processes == provider.process.processes,
          "post-launch PID and creation identity evidence passes validation");
}

void testAbsentOfflineAndUnsupportedAreExplicit() {
    FakeProvider provider;
    provider.descriptorValue.availability = ProviderAvailability::Absent;
    provider.descriptorValue.metadataRevision = 0u;
    std::vector<catalog::GameCatalogCandidate> games;
    check(discoverInstalledGames(provider, games).result == ProviderResult::ProviderAbsent &&
              provider.discoveryCalls == 0,
          "absent provider fails before invoking discovery");

    provider.descriptorValue.availability = ProviderAvailability::Offline;
    provider.descriptorValue.metadataRevision = 7u;
    check(discoverInstalledGames(provider, games).succeeded(),
          "offline state still permits read-only local discovery");

    ProviderLaunchRequest launch;
    check(buildLaunchRequest(provider, selection(), launch).result ==
                  ProviderResult::ProviderOffline &&
              provider.launchCalls == 0,
          "offline provider without offline-launch capability fails explicitly");

    provider.descriptorValue.capabilities.offlineLaunch = true;
    check(buildLaunchRequest(provider, selection(), launch).succeeded(),
          "provider may explicitly support construction of an offline launch request");

    provider.descriptorValue.availability = ProviderAvailability::Available;
    provider.descriptorValue.capabilities.accountReferences = false;
    std::vector<profile::ProviderAccountReference> accounts;
    check(listAccountReferences(provider, accounts).result ==
                  ProviderResult::UnsupportedOperation &&
              provider.accountCalls == 0,
          "unsupported account lookup never falls back or invokes the adapter");
}

void testMalformedAndStaleDiscoveryIsTransactional() {
    FakeProvider provider;
    std::vector<catalog::GameCatalogCandidate> output{game("sentinel")};
    const auto sentinel = output;

    provider.discovery.metadataRevision = 6u;
    check(discoverInstalledGames(provider, output).result == ProviderResult::StaleMetadata &&
              output == sentinel,
          "stale discovery revision leaves previous output unchanged");

    provider.discovery.metadataRevision = 7u;
    provider.discovery.candidates.front().providerId = "other";
    check(discoverInstalledGames(provider, output).result == ProviderResult::InvalidMetadata &&
              output == sentinel,
          "cross-provider discovery metadata is rejected transactionally");

    provider.discovery.candidates.front() = game();
    provider.discovery.candidates.front().executableCandidates.clear();
    check(discoverInstalledGames(provider, output).result == ProviderResult::InvalidMetadata &&
              output == sentinel,
          "catalog-invalid discovery metadata is rejected transactionally");
}

void testMalformedAccountLaunchAndProcessMetadataIsTransactional() {
    FakeProvider provider;
    std::vector<profile::ProviderAccountReference> accountOutput{{"fake", "sentinel"}};
    const std::vector<profile::ProviderAccountReference> accountSentinel = accountOutput;
    provider.accounts.accounts.push_back({"fake", "account-a"});
    check(listAccountReferences(provider, accountOutput).result ==
                  ProviderResult::InvalidMetadata &&
              accountOutput == accountSentinel,
          "duplicate account references are rejected transactionally");

    ProviderLaunchRequest launchOutput = provider.launch.request;
    launchOutput.launchCorrelationId = "sentinel";
    const auto launchSentinel = launchOutput;
    provider.launch.request.metadataRevision = 6u;
    check(buildLaunchRequest(provider, selection(), launchOutput).result ==
                  ProviderResult::StaleMetadata &&
              launchOutput == launchSentinel,
          "stale launch response cannot replace a previous plan");

    provider.launch.request.metadataRevision = 7u;
    provider.launch.request.target = L"cmd.exe /c arbitrary";
    check(buildLaunchRequest(provider, selection(), launchOutput).result ==
                  ProviderResult::InvalidMetadata &&
              launchOutput == launchSentinel,
          "untyped command or shell text is not accepted as an executable target");

    std::vector<ProviderProcessEvidence> processOutput{
        {1u, 1u, L"C:\\sentinel.exe", true}};
    const auto processSentinel = processOutput;
    provider.process.processes.front().creationTime100ns = 0u;
    check(identifyProcesses(provider,
                            {"fake", "game:fixture", "123456", "launch-1", 7u},
                            processOutput).result ==
                  ProviderResult::InvalidMetadata &&
              processOutput == processSentinel,
          "process evidence without creation identity is rejected transactionally");
}

void testInvalidRequestsDoNotReachProvider() {
    FakeProvider provider;
    auto stale = selection();
    stale.expectedMetadataRevision = 6u;
    ProviderLaunchRequest output;
    check(buildLaunchRequest(provider, stale, output).result ==
                  ProviderResult::StaleMetadata &&
              provider.launchCalls == 0,
          "stale launch selection is rejected before provider invocation");

    auto overlong = selection();
    overlong.instanceArguments.assign(kMaximumLaunchArguments + 1u, L"x");
    check(buildLaunchRequest(provider, overlong, output).result ==
                  ProviderResult::InvalidRequest &&
              provider.launchCalls == 0,
          "oversized argument vector is rejected before provider invocation");

    std::vector<ProviderProcessEvidence> processes;
    check(identifyProcesses(provider,
                            {"fake", "game:fixture", "123456", "bad correlation", 7u},
                            processes).result ==
                  ProviderResult::InvalidRequest &&
              provider.processCalls == 0,
          "malformed process query is rejected before provider invocation");
}

} // namespace

int main() {
    testDeterministicProviderContract();
    testAbsentOfflineAndUnsupportedAreExplicit();
    testMalformedAndStaleDiscoveryIsTransactional();
    testMalformedAccountLaunchAndProcessMetadataIsTransactional();
    testInvalidRequestsDoNotReachProvider();

    if (failures != 0) {
        std::cerr << failures << " provider adapter test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "P6-PROV-01 provider adapter tests passed\n";
    return EXIT_SUCCESS;
}
