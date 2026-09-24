#pragma once

#include "hydra/community_package.hpp"
#include "hydra/provider_launch_plan.hpp"
#include "hydra/setup_package.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::community {

inline constexpr std::uint32_t kCommunitySetupEntryVersion = 1u;
inline constexpr std::size_t kMaximumKnownLimitations = 32u;
inline constexpr std::size_t kMaximumEvidenceReferences = 128u;
inline constexpr std::size_t kMaximumLimitationBytes = 512u;

struct CommunitySetupEntry {
    std::uint32_t schemaVersion{kCommunitySetupEntryVersion};
    std::string entryId;
    std::string packageId;
    std::uint64_t packageRevision{0};
    GameSelector selector;
    portable::SetupPackage setupPackage;
    bool protectedExperimental{false};
    std::vector<std::string> knownLimitations;
    std::vector<std::string> evidenceResultIds;
    std::string sourceId;
    std::string licenseId;
    std::string authorAttribution;

    bool operator==(const CommunitySetupEntry&) const = default;
};

enum class CommunitySetupCode : std::uint8_t {
    Success = 0,
    UnsupportedSchema,
    InvalidIdentity,
    InvalidSelector,
    InvalidPortableSetup,
    SelectorMismatch,
    InvalidProvenance,
    InvalidText,
    TooManyLimitations,
    TooManyEvidenceReferences,
    DuplicateEvidenceReference,
    ProtectedClassificationMismatch,
    LocalGameMismatch,
    LocalImportFailed,
};

struct CommunitySetupDiagnostic {
    CommunitySetupCode code{CommunitySetupCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == CommunitySetupCode::Success; }
};

CommunitySetupDiagnostic validateCommunitySetupEntry(const CommunitySetupEntry& entry);

// Converts an already package-verified community entry into a local P6 setup.
// Explicit local path bindings are still mandatory and all P6 setup validation is
// rerun. Community evidence/popularity never supplies or overrides the local
// GameRuntimeRequirement used later by compileProviderAwareLaunchPlan.
// Typed import preserves any version-2 declarative instance materialization
// templates. The imported descriptors remain untrusted data and cannot become
// runtime/provider/physical authority without fresh local validation later.
CommunitySetupDiagnostic importCommunitySetup(
    const CommunitySetupEntry& entry,
    const profile::GameRecord& localGame,
    std::span<const portable::PathBinding> pathBindings,
    portable::ImportedSetup& output);

// Legacy convenience overload. A semantic-bearing version-2 package is rejected
// rather than silently discarding its phase/materialization descriptors.
CommunitySetupDiagnostic importCommunitySetup(
    const CommunitySetupEntry& entry,
    const profile::GameRecord& localGame,
    std::span<const portable::PathBinding> pathBindings,
    profile::TwoPlayerSetup& output);

std::string_view communitySetupCodeName(CommunitySetupCode code) noexcept;

} // namespace hydra::community
