#include "hydra/community_setup.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <utility>

namespace hydra::community {
namespace {

CommunitySetupDiagnostic fail(CommunitySetupCode code, std::string message) {
    return {code, std::move(message)};
}

bool validId(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128u) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
              ch == ':' || ch == '@' || ch == '+')) {
            return false;
        }
    }
    return true;
}

bool validVersion(std::string_view value) noexcept {
    return validId(value);
}

bool validUtf8(std::string_view value) noexcept {
    std::size_t position = 0u;
    while (position < value.size()) {
        const auto first = static_cast<unsigned char>(value[position++]);
        if (first <= 0x7fu) {
            if (first < 0x20u && first != '\t') return false;
            continue;
        }
        std::uint32_t codePoint = 0u;
        std::size_t continuation = 0u;
        if ((first & 0xe0u) == 0xc0u) {
            codePoint = first & 0x1fu;
            continuation = 1u;
            if (codePoint == 0u) return false;
        } else if ((first & 0xf0u) == 0xe0u) {
            codePoint = first & 0x0fu;
            continuation = 2u;
        } else if ((first & 0xf8u) == 0xf0u) {
            codePoint = first & 0x07u;
            continuation = 3u;
        } else {
            return false;
        }
        if (position + continuation > value.size()) return false;
        for (std::size_t index = 0u; index < continuation; ++index) {
            const auto next = static_cast<unsigned char>(value[position++]);
            if ((next & 0xc0u) != 0x80u) return false;
            codePoint = (codePoint << 6u) | (next & 0x3fu);
        }
        if ((continuation == 1u && codePoint < 0x80u) ||
            (continuation == 2u && codePoint < 0x800u) ||
            (continuation == 3u && codePoint < 0x10000u) || codePoint > 0x10ffffu ||
            (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

std::string asciiLower(std::string_view value) {
    std::string output(value);
    std::transform(output.begin(), output.end(), output.begin(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return static_cast<char>(std::tolower(ch));
    });
    return output;
}

bool forbiddenInstructionText(std::string_view value) {
    const auto lower = asciiLower(value);
    constexpr std::string_view forbidden[] = {
        "powershell", "cmd.exe", "rundll32", "reg add", "reg delete",
        "inject dll", "dll injection", "bypass drm", "disable drm",
        "disable anti-cheat", "disable anticheat", "patch anti-cheat",
        "http://", "https://", "file://", "javascript:",
    };
    for (const auto marker : forbidden) {
        if (lower.find(marker) != std::string::npos) return true;
    }
    return false;
}

bool validSelector(const GameSelector& selector) noexcept {
    return validId(selector.gameId) && validId(selector.providerId) &&
           (!selector.providerAppId || validId(*selector.providerAppId)) &&
           (!selector.gameVersion || validVersion(*selector.gameVersion));
}

bool localVersionMatches(std::string_view expected, std::wstring_view observed) noexcept {
    if (expected.size() != observed.size()) return false;
    for (std::size_t index = 0u; index < expected.size(); ++index) {
        const auto byte = static_cast<unsigned char>(expected[index]);
        if (observed[index] != static_cast<wchar_t>(byte)) return false;
    }
    return true;
}

} // namespace

CommunitySetupDiagnostic validateCommunitySetupEntry(const CommunitySetupEntry& entry) {
    if (entry.schemaVersion != kCommunitySetupEntryVersion) {
        return fail(CommunitySetupCode::UnsupportedSchema,
                    "unsupported community setup entry schema");
    }
    if (!validId(entry.entryId) || !validId(entry.packageId) || entry.packageRevision == 0u) {
        return fail(CommunitySetupCode::InvalidIdentity,
                    "community setup entry/package identity is invalid");
    }
    if (!validSelector(entry.selector)) {
        return fail(CommunitySetupCode::InvalidSelector,
                    "community setup game/provider/version selector is invalid");
    }
    if (!validId(entry.sourceId) || !validId(entry.licenseId) ||
        entry.authorAttribution.empty() || entry.authorAttribution.size() > 256u ||
        !validUtf8(entry.authorAttribution) || forbiddenInstructionText(entry.authorAttribution)) {
        return fail(CommunitySetupCode::InvalidProvenance,
                    "community setup provenance/license/author attribution is invalid");
    }
    if (entry.knownLimitations.size() > kMaximumKnownLimitations) {
        return fail(CommunitySetupCode::TooManyLimitations,
                    "community setup contains too many limitation records");
    }
    for (const auto& limitation : entry.knownLimitations) {
        if (limitation.empty() || limitation.size() > kMaximumLimitationBytes ||
            !validUtf8(limitation) || forbiddenInstructionText(limitation)) {
            return fail(CommunitySetupCode::InvalidText,
                        "community setup limitation text is malformed or contains executable/bypass instructions or external resources");
        }
    }
    if (entry.evidenceResultIds.size() > kMaximumEvidenceReferences) {
        return fail(CommunitySetupCode::TooManyEvidenceReferences,
                    "community setup has too many evidence references");
    }
    std::set<std::string> evidenceIds;
    for (const auto& resultId : entry.evidenceResultIds) {
        if (!validId(resultId) || !evidenceIds.insert(resultId).second) {
            return fail(CommunitySetupCode::DuplicateEvidenceReference,
                        "community setup evidence references are invalid or duplicated");
        }
    }

    std::string encoded;
    const auto packageDiagnostic = portable::encodePackage(entry.setupPackage, encoded);
    if (!packageDiagnostic.succeeded()) {
        return fail(CommunitySetupCode::InvalidPortableSetup,
                    "community setup portable payload is invalid: " + packageDiagnostic.message);
    }
    if (entry.setupPackage.redactedSetup.gameId != entry.selector.gameId) {
        return fail(CommunitySetupCode::SelectorMismatch,
                    "community setup payload Game ID differs from the declared selector");
    }
    if (entry.setupPackage.provenance.sourceId != entry.sourceId ||
        entry.setupPackage.provenance.sourceRevision != entry.packageRevision) {
        return fail(CommunitySetupCode::InvalidProvenance,
                    "portable setup provenance does not match the enclosing community package revision");
    }
    return {};
}

CommunitySetupDiagnostic importCommunitySetup(
    const CommunitySetupEntry& entry,
    const profile::GameRecord& localGame,
    std::span<const portable::PathBinding> pathBindings,
    portable::ImportedSetup& output) {
    const auto validation = validateCommunitySetupEntry(entry);
    if (!validation.succeeded()) return validation;
    if (entry.selector.gameId != localGame.gameId ||
        entry.selector.providerId != localGame.providerId ||
        entry.selector.providerAppId != localGame.providerAppId) {
        return fail(CommunitySetupCode::LocalGameMismatch,
                    "community setup selector does not match the selected local Game identity");
    }
    if (entry.selector.gameVersion && localGame.localVersion &&
        !localVersionMatches(*entry.selector.gameVersion, *localGame.localVersion)) {
        return fail(CommunitySetupCode::LocalGameMismatch,
                    "community setup exact game version selector does not match the local Game version");
    }

    portable::ImportedSetup imported;
    const auto importedDiagnostic = portable::importSetup(entry.setupPackage, localGame,
                                                           pathBindings, imported);
    if (!importedDiagnostic.succeeded()) {
        return fail(CommunitySetupCode::LocalImportFailed,
                    "community setup failed local P6 remap/validation: " +
                        importedDiagnostic.message);
    }
    output = std::move(imported);
    return {};
}

CommunitySetupDiagnostic importCommunitySetup(
    const CommunitySetupEntry& entry,
    const profile::GameRecord& localGame,
    std::span<const portable::PathBinding> pathBindings,
    profile::TwoPlayerSetup& output) {
    const auto validation = validateCommunitySetupEntry(entry);
    if (!validation.succeeded()) return validation;
    if (!entry.setupPackage.instanceMaterializations.empty()) {
        return fail(CommunitySetupCode::LocalImportFailed,
                    "community setup carries typed materialization semantics and requires typed import");
    }

    portable::ImportedSetup imported;
    const auto importedDiagnostic = importCommunitySetup(entry, localGame, pathBindings, imported);
    if (!importedDiagnostic.succeeded()) return importedDiagnostic;
    output = std::move(imported.setup);
    return {};
}

std::string_view communitySetupCodeName(CommunitySetupCode code) noexcept {
    switch (code) {
        case CommunitySetupCode::Success: return "Success";
        case CommunitySetupCode::UnsupportedSchema: return "UnsupportedSchema";
        case CommunitySetupCode::InvalidIdentity: return "InvalidIdentity";
        case CommunitySetupCode::InvalidSelector: return "InvalidSelector";
        case CommunitySetupCode::InvalidPortableSetup: return "InvalidPortableSetup";
        case CommunitySetupCode::SelectorMismatch: return "SelectorMismatch";
        case CommunitySetupCode::InvalidProvenance: return "InvalidProvenance";
        case CommunitySetupCode::InvalidText: return "InvalidText";
        case CommunitySetupCode::TooManyLimitations: return "TooManyLimitations";
        case CommunitySetupCode::TooManyEvidenceReferences: return "TooManyEvidenceReferences";
        case CommunitySetupCode::DuplicateEvidenceReference: return "DuplicateEvidenceReference";
        case CommunitySetupCode::ProtectedClassificationMismatch: return "ProtectedClassificationMismatch";
        case CommunitySetupCode::LocalGameMismatch: return "LocalGameMismatch";
        case CommunitySetupCode::LocalImportFailed: return "LocalImportFailed";
    }
    return "Unknown";
}

} // namespace hydra::community
