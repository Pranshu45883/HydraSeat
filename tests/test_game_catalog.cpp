#include "hydra/game_catalog.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace hydra::catalog;
using hydra::profile::CompatibilityReference;
using hydra::profile::GameOrigin;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

GameCatalogCandidate discovered(std::string provider,
                                std::string appId,
                                std::wstring title,
                                std::wstring executable) {
    GameCatalogCandidate value;
    value.providerId = std::move(provider);
    value.providerAppId = std::move(appId);
    value.title = std::move(title);
    value.installRoot = L"C:\\Games\\Shared";
    value.executableCandidates = {std::move(executable)};
    value.localVersion = L"1.2.3";
    value.executableSha256 =
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    value.compatibility = CompatibilityReference{"compat.game", "local", 2u};
    value.origin = GameOrigin::Discovered;
    value.localIconSource = L"C:\\Games\\Shared\\game.ico";
    value.architecture = ExecutableArchitecture::X64;
    value.staleness = CatalogStaleness::Current;
    return value;
}

GameCatalogCandidate manual(std::wstring title, std::wstring executable) {
    GameCatalogCandidate value;
    value.providerId = "custom";
    value.title = std::move(title);
    value.installRoot = L"C:\\Games\\Shared";
    value.executableCandidates = {std::move(executable)};
    value.origin = GameOrigin::Manual;
    value.architecture = ExecutableArchitecture::Unknown;
    value.staleness = CatalogStaleness::Unknown;
    return value;
}

void testEmptyAndSingleCandidateCatalog() {
    LocalGameCatalog output;
    output.entries.push_back({});
    const std::vector<GameCatalogCandidate> empty;
    const auto emptyResult = buildLocalGameCatalog(empty, output);
    check(emptyResult.succeeded() && output.entries.empty(),
          "empty local discovery produces an empty valid catalog");

    auto candidate = discovered("Steam", "123456", L"테스트 게임 🎮",
                                L"C:\\Games\\Shared\\Game.exe");
    LocalGameCatalog first;
    const auto built = buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&candidate, 1u),
                                             first);
    check(built.succeeded() && first.entries.size() == 1u,
          "one valid provider-neutral discovery candidate builds one catalog entry");
    if (first.entries.size() == 1u) {
        const auto& entry = first.entries.front();
        check(entry.game.providerId == "steam" &&
                  entry.game.providerAppId == std::optional<std::string>("123456") &&
                  entry.game.title == L"테스트 게임 🎮",
              "provider ID is canonicalized while title remains presentation metadata");
        check(entry.game.gameId.starts_with("game:") && entry.game.gameId.size() == 37u,
              "catalog assigns a bounded stable logical game ID");
        check(entry.game.executableSha256 &&
                  *entry.game.executableSha256 ==
                      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
              "hex hash metadata is canonicalized without reading the executable");
        check(entry.localIconSource == candidate.localIconSource &&
                  entry.architecture == ExecutableArchitecture::X64 &&
                  entry.staleness == CatalogStaleness::Current &&
                  entry.mergedCandidateCount == 1u,
              "catalog-only icon/architecture/staleness metadata remains attached");
    }

    LocalGameCatalog second;
    check(buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&candidate, 1u), second)
              .succeeded() &&
              second == first,
          "same candidate builds a byte-model-equivalent deterministic catalog");
}

void testDuplicateProviderAndExecutableReconciliation() {
    auto stale = discovered("steam", "42", L"Old Friendly Title",
                            L"C:\\Games\\Shared\\GAME.EXE");
    stale.staleness = CatalogStaleness::Stale;
    stale.localVersion = L"1.0";
    stale.localIconSource.reset();
    stale.executableCandidates.push_back(L"C:\\Games\\Shared\\helper.exe");

    auto current = discovered("STEAM", "42", L"Current Friendly Title",
                              L"c:/games/shared/./game.exe");
    current.localVersion = L"2.0";
    current.localIconSource = L"C:/Games/Shared/new.ico";
    current.architecture = ExecutableArchitecture::X64;

    auto fallback = manual(L"Whatever User Typed", L"c:\\games\\shared\\game.exe");
    fallback.installRoot = L"C:\\Games\\Shared\\.";
    fallback.staleness = CatalogStaleness::Unknown;

    std::vector<GameCatalogCandidate> candidates{stale, fallback, current};
    LocalGameCatalog catalog;
    const auto result = buildLocalGameCatalog(candidates, catalog);
    check(result.succeeded() && catalog.entries.size() == 1u,
          "same provider/app and normalized executable candidates reconcile into one entry");
    if (catalog.entries.size() == 1u) {
        const auto& entry = catalog.entries.front();
        check(entry.game.providerId == "steam" &&
                  entry.game.providerAppId == std::optional<std::string>("42") &&
                  entry.game.title == current.title &&
                  entry.game.localVersion == current.localVersion &&
                  entry.game.origin == GameOrigin::Discovered,
              "current strong provider metadata outranks stale/manual duplicate presentation data");
        check(entry.game.executableCandidates.size() == 2u,
              "duplicate executable spellings collapse while distinct helper executable is retained");
        check(entry.localIconSource == current.localIconSource &&
                  entry.staleness == CatalogStaleness::Current &&
                  entry.mergedCandidateCount == 3u,
              "best local icon and current staleness survive deterministic merge");
    }

    std::reverse(candidates.begin(), candidates.end());
    LocalGameCatalog reversed;
    check(buildLocalGameCatalog(candidates, reversed).succeeded() && reversed == catalog,
          "catalog output is invariant to overall discovery candidate ordering");
}

void testFriendlyTitleIsNeverIdentity() {
    auto first = manual(L"Same Name", L"C:\\Games\\One\\game.exe");
    first.installRoot = L"C:\\Games\\One";
    auto second = manual(L"Same Name", L"C:\\Games\\Two\\game.exe");
    second.installRoot = L"C:\\Games\\Two";

    const std::vector<GameCatalogCandidate> candidates{first, second};
    LocalGameCatalog catalog;
    const auto result = buildLocalGameCatalog(candidates, catalog);
    check(result.succeeded() && catalog.entries.size() == 2u,
          "two manual games with the same friendly title remain separate by executable identity");
    if (catalog.entries.size() == 2u) {
        check(catalog.entries[0].game.gameId != catalog.entries[1].game.gameId,
              "friendly title does not determine stable game ID");
    }
}

void testManualCandidateIsNotFilteredBySteamLikeTitle() {
    auto candidate = manual(L"Steamworks Common Redistributables",
                            L"C:\\Games\\Manual\\manual-runtime-name.exe");
    candidate.installRoot = L"C:\\Games\\Manual";

    LocalGameCatalog catalog;
    const auto result = buildLocalGameCatalog(
        std::span<const GameCatalogCandidate>(&candidate, 1u), catalog);
    check(result.succeeded() && catalog.entries.size() == 1u &&
              catalog.entries.front().game.origin == GameOrigin::Manual &&
              catalog.entries.front().game.title == candidate.title,
          "manual Add EXE remains visible even when its title resembles a Steam runtime package");
}

void testConflictingStrongIdentitiesFailTransactionally() {
    auto steam = discovered("steam", "100", L"Steam Record",
                            L"C:\\Games\\Shared\\game.exe");
    auto epic = discovered("epic", "different-app", L"Epic Record",
                           L"c:/games/shared/game.exe");
    const std::vector<GameCatalogCandidate> candidates{steam, epic};

    LocalGameCatalog output;
    auto sentinel = manual(L"Sentinel", L"C:\\Sentinel\\sentinel.exe");
    sentinel.installRoot = L"C:\\Sentinel";
    check(buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&sentinel, 1u), output)
              .succeeded(),
          "sentinel catalog is available before conflict test");
    const auto original = output;

    const auto result = buildLocalGameCatalog(candidates, output);
    check(result.result == CatalogBuildResult::IdentityConflict && output == original,
          "same executable carrying two strong provider/app identities fails closed transactionally");
}

void testMalformedAndBoundedCandidatesFailClosed() {
    auto valid = manual(L"Valid", L"C:\\Games\\Valid\\game.exe");
    valid.installRoot = L"C:\\Games\\Valid";
    LocalGameCatalog output;
    check(buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&valid, 1u), output)
              .succeeded(),
          "sentinel catalog is available before malformed tests");
    const auto sentinel = output;

    auto malformed = valid;
    malformed.providerId = "bad provider id";
    auto result = buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&malformed, 1u),
                                        output);
    check(result.result == CatalogBuildResult::InvalidCandidate && output == sentinel,
          "invalid provider identifier is rejected without replacing valid catalog");

    malformed = valid;
    malformed.executableCandidates.clear();
    result = buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&malformed, 1u), output);
    check(result.result == CatalogBuildResult::InvalidCandidate && output == sentinel,
          "candidate without executable identity is rejected transactionally");

    malformed = valid;
    malformed.localIconSource =
        std::wstring(hydra::profile::kMaximumPathCodeUnits + 1u, L'x');
    result = buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&malformed, 1u), output);
    check(result.result == CatalogBuildResult::InvalidCandidate && output == sentinel,
          "overlong local icon metadata is bounded even though missing icon itself is allowed");

    malformed = valid;
    malformed.architecture = static_cast<ExecutableArchitecture>(255u);
    result = buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&malformed, 1u), output);
    check(result.result == CatalogBuildResult::InvalidCandidate && output == sentinel,
          "unknown architecture enum is rejected instead of silently generalized");

    std::vector<GameCatalogCandidate> tooMany(kMaximumCatalogCandidates + 1u, valid);
    result = buildLocalGameCatalog(tooMany, output);
    check(result.result == CatalogBuildResult::TooManyCandidates && output == sentinel,
          "candidate list is bounded before normalization or merge work");
}

void testMissingOptionalMetadataAndArchitectureConflictAreConservative() {
    auto first = manual(L"No Artwork Required", L"C:\\Games\\NoArt\\game.exe");
    first.installRoot = L"C:\\Games\\NoArt";
    first.localIconSource.reset();
    first.architecture = ExecutableArchitecture::X86;
    first.staleness = CatalogStaleness::Current;

    auto second = first;
    second.executableCandidates = {L"c:/games/noart/game.exe"};
    second.architecture = ExecutableArchitecture::X64;

    const std::vector<GameCatalogCandidate> candidates{first, second};
    LocalGameCatalog catalog;
    const auto result = buildLocalGameCatalog(candidates, catalog);
    check(result.succeeded() && catalog.entries.size() == 1u,
          "missing artwork does not block a valid catalog entry");
    if (catalog.entries.size() == 1u) {
        check(!catalog.entries.front().localIconSource &&
                  catalog.entries.front().architecture == ExecutableArchitecture::Unknown,
              "equally current conflicting architecture evidence degrades to Unknown rather than guessing");
    }
}

void testProviderIdentityKeepsStableIdAcrossPresentationChanges() {
    auto first = discovered("steam", "999", L"Old Localized Title",
                            L"C:\\Games\\A\\game.exe");
    first.installRoot = L"C:\\Games\\A";
    auto second = first;
    second.title = L"새 제목";
    second.installRoot = L"D:\\Moved\\Game";
    second.executableCandidates = {L"D:\\Moved\\Game\\game.exe"};
    second.localIconSource = L"D:\\Moved\\Game\\game.ico";

    LocalGameCatalog before;
    LocalGameCatalog after;
    check(buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&first, 1u), before)
              .succeeded() &&
              buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&second, 1u), after)
                  .succeeded() &&
              before.entries.size() == 1u && after.entries.size() == 1u &&
              before.entries.front().game.gameId == after.entries.front().game.gameId,
          "provider/app identity keeps game ID stable across title/path/icon presentation changes");
}

void testExecutableFallbackIdentityNormalizesWindowsPath() {
    auto first = manual(L"Manual A", L"C:\\Games\\Example\\bin\\..\\game.exe");
    first.installRoot = L"C:\\Games\\Example";
    auto second = manual(L"Manual B", L"c:/games/example/game.exe");
    second.installRoot = L"C:/Games/Example/.";

    LocalGameCatalog firstCatalog;
    LocalGameCatalog secondCatalog;
    check(buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&first, 1u), firstCatalog)
              .succeeded() &&
              buildLocalGameCatalog(std::span<const GameCatalogCandidate>(&second, 1u), secondCatalog)
                  .succeeded() &&
              firstCatalog.entries.size() == 1u && secondCatalog.entries.size() == 1u &&
              firstCatalog.entries.front().game.gameId ==
                  secondCatalog.entries.front().game.gameId,
          "manual executable fallback normalizes Windows slash/case/dot-segment identity");
}

} // namespace

int main() {
    testEmptyAndSingleCandidateCatalog();
    testDuplicateProviderAndExecutableReconciliation();
    testFriendlyTitleIsNeverIdentity();
    testManualCandidateIsNotFilteredBySteamLikeTitle();
    testConflictingStrongIdentitiesFailTransactionally();
    testMalformedAndBoundedCandidatesFailClosed();
    testMissingOptionalMetadataAndArchitectureConflictAreConservative();
    testProviderIdentityKeepsStableIdAcrossPresentationChanges();
    testExecutableFallbackIdentityNormalizesWindowsPath();

    if (failures != 0) {
        std::cerr << failures << " game catalog test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "P6-CATALOG-01 game catalog tests passed\n";
    return EXIT_SUCCESS;
}
