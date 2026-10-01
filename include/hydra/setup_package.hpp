#pragma once

#include "hydra/two_player_setup_editor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::portable {

inline constexpr std::uint32_t kLegacySetupPackageVersion = 1u;
inline constexpr std::uint32_t kSetupPackageVersion = 2u;
inline constexpr std::size_t kMaximumSetupPackageBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaximumPathVariables = 4u;
inline constexpr std::size_t kMaximumPortableInstanceMaterializations = 2u;
inline constexpr std::size_t kMaximumPortableMaterializationStepsPerInstance = 16u;
inline constexpr std::size_t kMaximumPortableMutableFilesPerInstance = 64u;
inline constexpr std::uint64_t kMaximumPortableMutableFileBytes = 16ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaximumPortableMutableBytesPerInstance = 64ull * 1024ull * 1024ull;

constexpr bool isSupportedSetupPackageVersion(std::uint32_t version) noexcept {
    return version == kLegacySetupPackageVersion || version == kSetupPackageVersion;
}

struct Provenance {
    std::string sourceId;
    std::uint64_t sourceRevision{0};
    std::string exportedBy;

    bool operator==(const Provenance&) const = default;
};

enum class PathField : std::uint8_t {
    WorkingDirectory = 0,
    DataRoot = 1,
};

struct PathVariable {
    std::string variableName;
    std::uint32_t instanceIndex{0};
    PathField field{PathField::DataRoot};

    bool operator==(const PathVariable&) const = default;
};

// Portable materialization is deliberately a declarative template, not runtime
// authority. It identifies only one of the setup's two instance indexes and the
// bounded relative-file operations/timing that may later be rebound to fresh
// local provider/requirement evidence. Seat IDs, provider revisions, trusted
// evidence, plan/source/instance/recipe fingerprints, absolute destinations,
// scripts, commands, and executable payloads are intentionally not representable.
struct PortableMutableFileSpec {
    std::wstring sourceRelativePath;
    std::wstring destinationRelativePath;
    std::uint64_t maximumBytes{kMaximumPortableMutableFileBytes};

    bool operator==(const PortableMutableFileSpec&) const = default;
};

struct PortableCompatibilityStep {
    std::string stepId;
    setup::RecipeExecutionPhase phase{setup::RecipeExecutionPhase::PreSpawn};
    setup::MutationScope scope{setup::MutationScope::SeatWritableInstance};
    std::vector<PortableMutableFileSpec> files;

    bool operator==(const PortableCompatibilityStep&) const = default;
};

struct PortableInstanceMaterialization {
    std::uint32_t instanceIndex{0};
    std::vector<PortableCompatibilityStep> steps;

    bool operator==(const PortableInstanceMaterialization&) const = default;
};

struct SetupPackage {
    std::uint32_t version{kSetupPackageVersion};
    Provenance provenance;
    profile::TwoPlayerSetup redactedSetup;
    std::vector<PathVariable> pathVariables;
    std::vector<PortableInstanceMaterialization> instanceMaterializations;

    bool operator==(const SetupPackage&) const = default;
};

struct PathBinding {
    std::string variableName;
    std::wstring localPath;

    bool operator==(const PathBinding&) const = default;
};

struct ImportedSetup {
    profile::TwoPlayerSetup setup;
    std::vector<PortableInstanceMaterialization> instanceMaterializations;

    bool operator==(const ImportedSetup&) const = default;
};

enum class PackageResult : std::uint8_t {
    Success = 0,
    InvalidSetup = 1,
    InvalidProvenance = 2,
    InvalidPackage = 3,
    PackageTooLarge = 4,
    UnsupportedVersion = 5,
    MissingBinding = 6,
    DuplicateBinding = 7,
    UnexpectedBinding = 8,
    InvalidLocalPath = 9,
    UnsupportedSemantic = 10,
};

struct PackageDiagnostic {
    PackageResult result{PackageResult::Success};
    std::string message;

    bool succeeded() const noexcept { return result == PackageResult::Success; }
};

// Export is pure: source absolute working/data paths are replaced with typed
// variables before the package is returned. Player names, provider credentials,
// authentication material, device identities, scripts, binaries, and runtime
// trust evidence are not representable.
PackageDiagnostic exportSetup(const profile::TwoPlayerSetup& setup,
                              const profile::GameRecord& game,
                              Provenance provenance,
                              SetupPackage& output);

// Version-2 export for declarative phase/materialization templates. The templates
// stay instance-indexed and untrusted; production must bind them to fresh local
// provider/requirement authority before any mutation is compiled or executed.
PackageDiagnostic exportSetup(
    const profile::TwoPlayerSetup& setup,
    const profile::GameRecord& game,
    Provenance provenance,
    std::span<const PortableInstanceMaterialization> instanceMaterializations,
    SetupPackage& output);

// Typed import preserves every version-2 declarative semantic in addition to the
// locally remapped TwoPlayerSetup. Import itself never manufactures runtime trust.
PackageDiagnostic importSetup(const SetupPackage& package,
                              const profile::GameRecord& localGame,
                              std::span<const PathBinding> bindings,
                              ImportedSetup& output);

// Legacy convenience overload. It is intentionally fail-closed when a package
// carries version-2 materialization semantics because returning only TwoPlayerSetup
// would silently discard them.
PackageDiagnostic importSetup(const SetupPackage& package,
                              const profile::GameRecord& localGame,
                              std::span<const PathBinding> bindings,
                              profile::TwoPlayerSetup& output);

PackageDiagnostic encodePackage(const SetupPackage& package, std::string& output);
PackageDiagnostic decodePackage(std::string_view bytes, SetupPackage& output);

std::string_view packageResultName(PackageResult result) noexcept;
std::string_view pathFieldName(PathField field) noexcept;

} // namespace hydra::portable
