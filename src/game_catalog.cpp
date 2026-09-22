#include "hydra/game_catalog.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hydra::catalog {
namespace {

struct NormalizedExecutable {
    std::string key;
    std::wstring display;
};

struct NormalizedCandidate {
    std::size_t inputIndex{0};
    GameCatalogCandidate value;
    std::string normalizedInstallRoot;
    std::string normalizedIconSource;
    std::vector<NormalizedExecutable> executables;
    std::optional<std::string> strongIdentity;
    std::string canonicalKey;
};

struct DisjointSet {
    explicit DisjointSet(std::size_t count) : parent(count), rank(count, 0u) {
        for (std::size_t index = 0u; index < count; ++index) parent[index] = index;
    }

    std::size_t find(std::size_t value) {
        if (parent[value] != value) parent[value] = find(parent[value]);
        return parent[value];
    }

    void unite(std::size_t left, std::size_t right) {
        left = find(left);
        right = find(right);
        if (left == right) return;
        if (rank[left] < rank[right]) std::swap(left, right);
        parent[right] = left;
        if (rank[left] == rank[right]) ++rank[left];
    }

    std::vector<std::size_t> parent;
    std::vector<unsigned> rank;
};

CatalogBuildDiagnostic diagnostic(CatalogBuildResult result,
                                  std::string message,
                                  std::size_t candidateIndex = kNoCandidateIndex) {
    return {result, candidateIndex, std::move(message)};
}

bool validArchitecture(ExecutableArchitecture value) noexcept {
    switch (value) {
    case ExecutableArchitecture::Unknown:
    case ExecutableArchitecture::X86:
    case ExecutableArchitecture::X64:
    case ExecutableArchitecture::Arm64:
        return true;
    }
    return false;
}

bool validStaleness(CatalogStaleness value) noexcept {
    switch (value) {
    case CatalogStaleness::Unknown:
    case CatalogStaleness::Current:
    case CatalogStaleness::Stale:
        return true;
    }
    return false;
}

bool validOrigin(profile::GameOrigin value) noexcept {
    switch (value) {
    case profile::GameOrigin::Discovered:
    case profile::GameOrigin::Manual:
        return true;
    }
    return false;
}

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch >= static_cast<unsigned char>('A') &&
            ch <= static_cast<unsigned char>('Z')) {
            return static_cast<char>(ch - static_cast<unsigned char>('A') +
                                     static_cast<unsigned char>('a'));
        }
        return raw;
    });
    return value;
}

std::string lowerHex(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch >= static_cast<unsigned char>('A') &&
            ch <= static_cast<unsigned char>('F')) {
            return static_cast<char>(ch - static_cast<unsigned char>('A') +
                                     static_cast<unsigned char>('a'));
        }
        return raw;
    });
    return value;
}

bool wideToUtf8(std::wstring_view input, std::string& output) {
    output.clear();
    output.reserve(input.size());
    auto append = [&](std::uint32_t codePoint) {
        if (codePoint <= 0x7fu) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else if (codePoint <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        }
    };

    for (std::size_t index = 0u; index < input.size(); ++index) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(input[index]);
        if constexpr (sizeof(wchar_t) == 2u) {
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu) {
                if (++index >= input.size()) return false;
                const auto low = static_cast<std::uint32_t>(input[index]);
                if (low < 0xdc00u || low > 0xdfffu) return false;
                codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) +
                            (low - 0xdc00u);
            } else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
                return false;
            }
        } else {
            if (codePoint > 0x10ffffu ||
                (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
                return false;
            }
        }
        append(codePoint);
    }
    return true;
}

std::wstring normalizeWindowsPath(std::wstring_view input) {
    std::wstring value(input);
    for (auto& ch : value) {
        if (ch == L'/') ch = L'\\';
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }

    bool unc = value.size() >= 2u && value[0] == L'\\' && value[1] == L'\\';
    bool rooted = !value.empty() && value[0] == L'\\';
    std::wstring drive;
    std::size_t cursor = 0u;
    if (value.size() >= 2u &&
        ((value[0] >= L'a' && value[0] <= L'z') ||
         (value[0] >= L'A' && value[0] <= L'Z')) &&
        value[1] == L':') {
        drive.assign(value.substr(0u, 2u));
        cursor = 2u;
        rooted = cursor < value.size() && value[cursor] == L'\\';
        if (rooted) ++cursor;
        unc = false;
    } else if (unc) {
        cursor = 2u;
        rooted = true;
    } else if (rooted) {
        cursor = 1u;
    }

    std::vector<std::wstring> segments;
    std::size_t protectedSegments = 0u;
    while (cursor <= value.size()) {
        const auto next = value.find(L'\\', cursor);
        const auto end = next == std::wstring::npos ? value.size() : next;
        std::wstring segment = value.substr(cursor, end - cursor);
        cursor = next == std::wstring::npos ? value.size() + 1u : next + 1u;
        if (segment.empty() || segment == L".") continue;

        if (unc && protectedSegments < 2u) {
            segments.push_back(std::move(segment));
            ++protectedSegments;
            continue;
        }
        if (segment == L"..") {
            const std::size_t floor = unc ? protectedSegments : 0u;
            if (segments.size() > floor && segments.back() != L"..") {
                segments.pop_back();
            } else if (!rooted) {
                segments.push_back(std::move(segment));
            }
            continue;
        }
        segments.push_back(std::move(segment));
    }

    std::wstring output;
    if (!drive.empty()) {
        output = drive;
        if (rooted) output.push_back(L'\\');
    } else if (unc) {
        output = L"\\\\";
    } else if (rooted) {
        output = L"\\";
    }

    for (std::size_t index = 0u; index < segments.size(); ++index) {
        if (!output.empty() && output.back() != L'\\' &&
            !(output.size() == 2u && output[1] == L':' && !rooted)) {
            output.push_back(L'\\');
        }
        output += segments[index];
    }
    return output.empty() ? value : output;
}

bool normalizedPathKey(std::wstring_view path, std::string& key) {
    const auto normalized = normalizeWindowsPath(path);
    return !normalized.empty() && wideToUtf8(normalized, key);
}

std::uint64_t fnv1a64(std::string_view bytes, std::uint64_t offset) noexcept {
    std::uint64_t hash = offset;
    for (const char raw : bytes) {
        hash ^= static_cast<unsigned char>(raw);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex64(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(16u, '0');
    for (std::size_t index = 0u; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((output.size() - index - 1u) * 4u);
        output[index] = digits[(value >> shift) & 0x0fu];
    }
    return output;
}

std::string stableGameId(std::string_view identity) {
    constexpr std::uint64_t offsetA = 14695981039346656037ull;
    constexpr std::uint64_t offsetB = 7809847782465536322ull;
    const auto first = fnv1a64(identity, offsetA);
    const auto second = fnv1a64(identity, offsetB);
    return "game:" + hex64(first) + hex64(second);
}

int stalenessQuality(CatalogStaleness value) noexcept {
    switch (value) {
    case CatalogStaleness::Current: return 3;
    case CatalogStaleness::Unknown: return 2;
    case CatalogStaleness::Stale: return 1;
    }
    return 0;
}

int originQuality(profile::GameOrigin value) noexcept {
    return value == profile::GameOrigin::Discovered ? 2 : 1;
}

std::string optionalStringKey(const std::optional<std::string>& value) {
    return value ? *value : std::string{};
}

std::string optionalWideKey(const std::optional<std::wstring>& value) {
    if (!value) return {};
    std::string utf8;
    if (!wideToUtf8(*value, utf8)) return {};
    return utf8;
}

std::string compatibilityKey(
    const std::optional<profile::CompatibilityReference>& value) {
    if (!value) return {};
    return value->recordId + "|" + value->provenance + "|" +
           std::to_string(value->evidenceRevision);
}

std::string canonicalCandidateKey(const NormalizedCandidate& candidate) {
    std::ostringstream output;
    output << candidate.value.providerId << '|'
           << optionalStringKey(candidate.value.providerAppId) << '|'
           << static_cast<unsigned>(candidate.value.origin) << '|'
           << static_cast<unsigned>(candidate.value.architecture) << '|'
           << static_cast<unsigned>(candidate.value.staleness) << '|'
           << candidate.normalizedInstallRoot << '|';
    std::string title;
    std::string installRootDisplay;
    (void)wideToUtf8(candidate.value.title, title);
    (void)wideToUtf8(candidate.value.installRoot, installRootDisplay);
    output << title << '|';
    for (const auto& executable : candidate.executables) {
        std::string display;
        (void)wideToUtf8(executable.display, display);
        // The normalized key defines semantic path identity. The display spelling
        // is a deterministic final tie-breaker so equivalent slash/dot/case input
        // cannot leave representative selection dependent on candidate order.
        output << executable.key << ':' << display << ';';
    }
    output << '|'
           << installRootDisplay << '|'
           << optionalWideKey(candidate.value.localVersion) << '|'
           << optionalStringKey(candidate.value.executableSha256) << '|'
           << compatibilityKey(candidate.value.compatibility) << '|'
           << candidate.normalizedIconSource << '|'
           << optionalWideKey(candidate.value.localIconSource);
    return output.str();
}

bool betterCandidate(const NormalizedCandidate& left,
                     const NormalizedCandidate& right) {
    const int leftStaleness = stalenessQuality(left.value.staleness);
    const int rightStaleness = stalenessQuality(right.value.staleness);
    if (leftStaleness != rightStaleness) return leftStaleness > rightStaleness;

    const bool leftStrong = left.strongIdentity.has_value();
    const bool rightStrong = right.strongIdentity.has_value();
    if (leftStrong != rightStrong) return leftStrong;

    const int leftOrigin = originQuality(left.value.origin);
    const int rightOrigin = originQuality(right.value.origin);
    if (leftOrigin != rightOrigin) return leftOrigin > rightOrigin;

    return left.canonicalKey < right.canonicalKey;
}

CatalogBuildDiagnostic normalizeCandidate(const GameCatalogCandidate& input,
                                          std::size_t inputIndex,
                                          NormalizedCandidate& output) {
    if (!validArchitecture(input.architecture) || !validStaleness(input.staleness) ||
        !validOrigin(input.origin)) {
        return diagnostic(CatalogBuildResult::InvalidCandidate,
                          "candidate contains an unknown enum value", inputIndex);
    }

    NormalizedCandidate candidate;
    candidate.inputIndex = inputIndex;
    candidate.value = input;
    candidate.value.providerId = asciiLower(candidate.value.providerId);
    if (candidate.value.executableSha256) {
        candidate.value.executableSha256 = lowerHex(*candidate.value.executableSha256);
    }

    if (!normalizedPathKey(candidate.value.installRoot,
                           candidate.normalizedInstallRoot)) {
        return diagnostic(CatalogBuildResult::InvalidCandidate,
                          "candidate install_root is not valid Unicode/path text",
                          inputIndex);
    }
    if (candidate.value.localIconSource) {
        if (candidate.value.localIconSource->empty() ||
            candidate.value.localIconSource->size() > profile::kMaximumPathCodeUnits ||
            !normalizedPathKey(*candidate.value.localIconSource,
                               candidate.normalizedIconSource)) {
            return diagnostic(CatalogBuildResult::InvalidCandidate,
                              "candidate local icon source is invalid or exceeds path bounds",
                              inputIndex);
        }
    }

    std::map<std::string, std::wstring> seenExecutables;
    std::vector<NormalizedExecutable> executables;
    executables.reserve(candidate.value.executableCandidates.size());
    for (const auto& executable : candidate.value.executableCandidates) {
        std::string key;
        if (!normalizedPathKey(executable, key)) {
            return diagnostic(CatalogBuildResult::InvalidCandidate,
                              "candidate executable path is not valid Unicode/path text",
                              inputIndex);
        }
        const auto [found, inserted] = seenExecutables.emplace(key, executable);
        if (inserted) {
            executables.push_back({key, executable});
        } else {
            std::string existingUtf8;
            std::string incomingUtf8;
            if (!wideToUtf8(found->second, existingUtf8) ||
                !wideToUtf8(executable, incomingUtf8)) {
                return diagnostic(CatalogBuildResult::InvalidCandidate,
                                  "candidate executable path contains invalid Unicode",
                                  inputIndex);
            }
            if (incomingUtf8 < existingUtf8) {
                found->second = executable;
                const auto located = std::find_if(
                    executables.begin(), executables.end(),
                    [&](const auto& item) { return item.key == key; });
                if (located != executables.end()) located->display = executable;
            }
        }
    }
    candidate.executables = std::move(executables);
    candidate.value.executableCandidates.clear();
    for (const auto& executable : candidate.executables) {
        candidate.value.executableCandidates.push_back(executable.display);
    }

    profile::GameRecord provisional;
    provisional.gameId = "candidate";
    provisional.providerId = candidate.value.providerId;
    provisional.providerAppId = candidate.value.providerAppId;
    provisional.title = candidate.value.title;
    provisional.installRoot = candidate.value.installRoot;
    provisional.executableCandidates = candidate.value.executableCandidates;
    provisional.localVersion = candidate.value.localVersion;
    provisional.executableSha256 = candidate.value.executableSha256;
    provisional.compatibility = candidate.value.compatibility;
    provisional.origin = candidate.value.origin;
    profile::GameRecordDocument document;
    document.games.push_back(std::move(provisional));
    const auto schemaResult = profile::validateGameRecordDocument(document);
    if (!schemaResult.succeeded()) {
        return diagnostic(CatalogBuildResult::InvalidCandidate,
                          "candidate failed GameRecord bounds/schema validation: " +
                              schemaResult.message,
                          inputIndex);
    }

    if (candidate.value.providerAppId) {
        candidate.strongIdentity = "provider|" + candidate.value.providerId + "|" +
                                   *candidate.value.providerAppId;
    }
    candidate.canonicalKey = canonicalCandidateKey(candidate);
    output = std::move(candidate);
    return {};
}

CatalogStaleness mergedStaleness(
    const std::vector<const NormalizedCandidate*>& group) noexcept {
    bool unknown = false;
    for (const auto* candidate : group) {
        if (candidate->value.staleness == CatalogStaleness::Current) {
            return CatalogStaleness::Current;
        }
        if (candidate->value.staleness == CatalogStaleness::Unknown) unknown = true;
    }
    return unknown ? CatalogStaleness::Unknown : CatalogStaleness::Stale;
}

ExecutableArchitecture mergedArchitecture(
    const std::vector<const NormalizedCandidate*>& orderedGroup) noexcept {
    int bestQuality = 0;
    std::set<ExecutableArchitecture> bestArchitectures;
    for (const auto* candidate : orderedGroup) {
        if (candidate->value.architecture == ExecutableArchitecture::Unknown) continue;
        const int quality = stalenessQuality(candidate->value.staleness);
        if (quality > bestQuality) {
            bestQuality = quality;
            bestArchitectures.clear();
            bestArchitectures.insert(candidate->value.architecture);
        } else if (quality == bestQuality) {
            bestArchitectures.insert(candidate->value.architecture);
        }
    }
    if (bestArchitectures.size() == 1u) return *bestArchitectures.begin();
    return ExecutableArchitecture::Unknown;
}

template <typename Selector>
auto firstOptional(const std::vector<const NormalizedCandidate*>& orderedGroup,
                   Selector selector) {
    using OptionalType = decltype(selector(*orderedGroup.front()));
    for (const auto* candidate : orderedGroup) {
        OptionalType value = selector(*candidate);
        if (value) return value;
    }
    return OptionalType{};
}

std::vector<std::wstring> mergedExecutables(
    const NormalizedCandidate& representative,
    const std::vector<const NormalizedCandidate*>& orderedGroup) {
    std::map<std::string, std::wstring> selected;
    std::vector<std::string> order;

    auto add = [&](const NormalizedExecutable& executable) {
        if (selected.emplace(executable.key, executable.display).second) {
            order.push_back(executable.key);
        }
    };
    for (const auto& executable : representative.executables) add(executable);

    std::map<std::string, std::wstring> remaining;
    for (const auto* candidate : orderedGroup) {
        for (const auto& executable : candidate->executables) {
            if (selected.contains(executable.key)) continue;
            auto found = remaining.find(executable.key);
            if (found == remaining.end()) {
                remaining.emplace(executable.key, executable.display);
            } else {
                std::string existing;
                std::string incoming;
                (void)wideToUtf8(found->second, existing);
                (void)wideToUtf8(executable.display, incoming);
                if (incoming < existing) found->second = executable.display;
            }
        }
    }
    for (const auto& [key, display] : remaining) {
        selected.emplace(key, display);
        order.push_back(key);
    }

    std::vector<std::wstring> output;
    output.reserve(order.size());
    for (const auto& key : order) output.push_back(selected.at(key));
    return output;
}

CatalogBuildDiagnostic buildEntry(
    const std::vector<const NormalizedCandidate*>& unsortedGroup,
    std::map<std::string, std::string>& gameIdIdentities,
    LocalGameCatalogEntry& output) {
    std::vector<const NormalizedCandidate*> group = unsortedGroup;
    std::sort(group.begin(), group.end(), [](const auto* left, const auto* right) {
        return betterCandidate(*left, *right);
    });

    std::set<std::string> strongIdentities;
    for (const auto* candidate : group) {
        if (candidate->strongIdentity) strongIdentities.insert(*candidate->strongIdentity);
    }
    if (strongIdentities.size() > 1u) {
        std::size_t firstIndex = kNoCandidateIndex;
        for (const auto* candidate : group) {
            firstIndex = std::min(firstIndex, candidate->inputIndex);
        }
        return diagnostic(
            CatalogBuildResult::IdentityConflict,
            "one executable-equivalent candidate group contains multiple provider/app identities",
            firstIndex);
    }

    const NormalizedCandidate* representative = group.front();
    if (!strongIdentities.empty()) {
        const auto& strong = *strongIdentities.begin();
        const auto found = std::find_if(group.begin(), group.end(), [&](const auto* item) {
            return item->strongIdentity && *item->strongIdentity == strong;
        });
        if (found != group.end()) representative = *found;
    }

    std::string identity;
    if (representative->strongIdentity) {
        identity = *representative->strongIdentity;
    } else {
        if (representative->executables.empty()) {
            return diagnostic(CatalogBuildResult::SchemaValidationError,
                              "normalized catalog representative has no executable",
                              representative->inputIndex);
        }
        identity = "exe|" + representative->executables.front().key;
    }
    const auto gameId = stableGameId(identity);
    const auto [identityIt, inserted] = gameIdIdentities.emplace(gameId, identity);
    if (!inserted && identityIt->second != identity) {
        return diagnostic(CatalogBuildResult::IdentityCollision,
                          "two distinct catalog identities generated the same bounded game_id",
                          representative->inputIndex);
    }

    profile::GameRecord game;
    game.gameId = gameId;
    game.providerId = representative->value.providerId;
    game.providerAppId = representative->value.providerAppId;
    game.title = representative->value.title;
    game.installRoot = representative->value.installRoot;
    game.executableCandidates = mergedExecutables(*representative, group);
    game.localVersion = firstOptional(group, [](const NormalizedCandidate& candidate) {
        return candidate.value.localVersion;
    });
    game.executableSha256 = firstOptional(group, [](const NormalizedCandidate& candidate) {
        return candidate.value.executableSha256;
    });
    game.compatibility = firstOptional(group, [](const NormalizedCandidate& candidate) {
        return candidate.value.compatibility;
    });
    game.origin = representative->value.origin;

    profile::GameRecordDocument document;
    document.games.push_back(game);
    const auto schemaResult = profile::validateGameRecordDocument(document);
    if (!schemaResult.succeeded()) {
        return diagnostic(CatalogBuildResult::SchemaValidationError,
                          "reconciled GameRecord failed schema validation: " +
                              schemaResult.message,
                          representative->inputIndex);
    }

    LocalGameCatalogEntry entry;
    entry.game = std::move(game);
    entry.localIconSource = firstOptional(group, [](const NormalizedCandidate& candidate) {
        return candidate.value.localIconSource;
    });
    entry.architecture = mergedArchitecture(group);
    entry.staleness = mergedStaleness(group);
    if (group.size() > std::numeric_limits<std::uint32_t>::max()) {
        return diagnostic(CatalogBuildResult::SchemaValidationError,
                          "candidate merge count exceeds uint32 range",
                          representative->inputIndex);
    }
    entry.mergedCandidateCount = static_cast<std::uint32_t>(group.size());
    output = std::move(entry);
    return {};
}

} // namespace

CatalogBuildDiagnostic buildLocalGameCatalog(
    std::span<const GameCatalogCandidate> candidates,
    LocalGameCatalog& output) {
    if (candidates.size() > kMaximumCatalogCandidates) {
        return diagnostic(CatalogBuildResult::TooManyCandidates,
                          "catalog candidate count exceeds bounded maximum");
    }

    std::vector<NormalizedCandidate> normalized;
    normalized.reserve(candidates.size());
    for (std::size_t index = 0u; index < candidates.size(); ++index) {
        NormalizedCandidate candidate;
        const auto checked = normalizeCandidate(candidates[index], index, candidate);
        if (!checked.succeeded()) return checked;
        normalized.push_back(std::move(candidate));
    }

    DisjointSet sets(normalized.size());
    std::map<std::string, std::size_t> strongOwner;
    std::map<std::string, std::size_t> executableOwner;
    for (std::size_t index = 0u; index < normalized.size(); ++index) {
        if (normalized[index].strongIdentity) {
            const auto [found, inserted] = strongOwner.emplace(*normalized[index].strongIdentity,
                                                               index);
            if (!inserted) sets.unite(index, found->second);
        }
        for (const auto& executable : normalized[index].executables) {
            const auto [found, inserted] = executableOwner.emplace(executable.key, index);
            if (!inserted) sets.unite(index, found->second);
        }
    }

    std::map<std::size_t, std::vector<const NormalizedCandidate*>> groups;
    for (std::size_t index = 0u; index < normalized.size(); ++index) {
        groups[sets.find(index)].push_back(&normalized[index]);
    }

    LocalGameCatalog candidateCatalog;
    candidateCatalog.entries.reserve(groups.size());
    std::map<std::string, std::string> gameIdIdentities;
    for (const auto& [root, group] : groups) {
        (void)root;
        LocalGameCatalogEntry entry;
        const auto built = buildEntry(group, gameIdIdentities, entry);
        if (!built.succeeded()) return built;
        candidateCatalog.entries.push_back(std::move(entry));
    }

    std::sort(candidateCatalog.entries.begin(), candidateCatalog.entries.end(),
              [](const auto& left, const auto& right) {
                  return left.game.gameId < right.game.gameId;
              });

    profile::GameRecordDocument document;
    document.games.reserve(candidateCatalog.entries.size());
    for (const auto& entry : candidateCatalog.entries) {
        document.games.push_back(entry.game);
    }
    const auto schemaResult = profile::validateGameRecordDocument(document);
    if (!schemaResult.succeeded()) {
        return diagnostic(CatalogBuildResult::SchemaValidationError,
                          "final catalog failed GameRecordDocument validation: " +
                              schemaResult.message);
    }

    output = std::move(candidateCatalog);
    return {};
}

std::string_view catalogBuildResultName(CatalogBuildResult result) noexcept {
    switch (result) {
    case CatalogBuildResult::Success: return "success";
    case CatalogBuildResult::TooManyCandidates: return "too-many-candidates";
    case CatalogBuildResult::InvalidCandidate: return "invalid-candidate";
    case CatalogBuildResult::IdentityConflict: return "identity-conflict";
    case CatalogBuildResult::IdentityCollision: return "identity-collision";
    case CatalogBuildResult::SchemaValidationError: return "schema-validation-error";
    }
    return "unknown";
}

std::string_view executableArchitectureName(ExecutableArchitecture architecture) noexcept {
    switch (architecture) {
    case ExecutableArchitecture::Unknown: return "unknown";
    case ExecutableArchitecture::X86: return "x86";
    case ExecutableArchitecture::X64: return "x64";
    case ExecutableArchitecture::Arm64: return "arm64";
    }
    return "unknown";
}

std::string_view catalogStalenessName(CatalogStaleness staleness) noexcept {
    switch (staleness) {
    case CatalogStaleness::Unknown: return "unknown";
    case CatalogStaleness::Current: return "current";
    case CatalogStaleness::Stale: return "stale";
    }
    return "unknown";
}

} // namespace hydra::catalog
