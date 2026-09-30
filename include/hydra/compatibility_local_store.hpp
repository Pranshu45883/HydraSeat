#pragma once

#include "hydra/compatibility_share_model.hpp"
#include "hydra/instance_materialization.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hydra::community {

enum class CompatibilityLocalStoreCode : std::uint8_t {
    Success = 0,
    Missing,
    TooLarge,
    InvalidHistory,
    ReadFailed,
    WriteFailed,
    RemoveFailed,
    CleanupFailed,
};

struct CompatibilityLocalStoreDiagnostic {
    CompatibilityLocalStoreCode code{CompatibilityLocalStoreCode::Success};
    std::string message;

    bool succeeded() const noexcept {
        return code == CompatibilityLocalStoreCode::Success ||
               code == CompatibilityLocalStoreCode::Missing;
    }
    bool found() const noexcept { return code != CompatibilityLocalStoreCode::Missing; }
};

// Fixed-purpose local technical-evidence store. The path is selected by the
// trusted caller/default resolver; persisted bytes cannot select another path.
// Writes stage a sibling temporary file and publish only complete bounded JSONL.
class CompatibilityLocalStore final {
public:
    explicit CompatibilityLocalStore(std::filesystem::path path)
        : path_(std::move(path)) {}

    CompatibilityLocalStoreDiagnostic load(CompatibilityShareModel& model) const;
    CompatibilityLocalStoreDiagnostic save(const CompatibilityShareModel& model) const;
    CompatibilityLocalStoreDiagnostic remove() const;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path temporaryPath() const;

    std::filesystem::path path_;
};

std::optional<std::filesystem::path> defaultCompatibilityLocalStorePath(
    std::string* error = nullptr);
std::string_view compatibilityLocalStoreCodeName(CompatibilityLocalStoreCode code) noexcept;

} // namespace hydra::community

namespace hydra::materialization {

inline constexpr std::uint32_t kLocalMaterializationDecisionSchemaVersion = 1u;
inline constexpr std::size_t kMaximumLocalMaterializationDecisions = 128u;
inline constexpr std::size_t kMaximumLocalMaterializationDecisionBytes = 1024u * 1024u;

// Imported/community materialization descriptors are deliberately representable as
// data so the trust boundary can reject them explicitly. Only LocalApproved records
// from this fixed-purpose local store are eligible to become runtime mutation authority.
enum class LocalMaterializationDecisionOrigin : std::uint8_t {
    LocalApproved = 0,
    ImportedCommunity = 1,
};

struct LocalMaterializationDecision {
    std::uint32_t schemaVersion{kLocalMaterializationDecisionSchemaVersion};
    std::string decisionId;
    std::uint64_t revision{0};
    LocalMaterializationDecisionOrigin origin{LocalMaterializationDecisionOrigin::LocalApproved};
    std::string setupId;
    std::uint32_t instanceIndex{0};
    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::uint64_t providerMetadataRevision{0};
    std::uint64_t requirementRevision{0};
    std::optional<profile::CompatibilityReference> compatibility;
    std::vector<CompatibilityRecipeStep> steps;

    bool operator==(const LocalMaterializationDecision&) const = default;
};

struct LocalMaterializationDecisionDocument {
    std::uint32_t schemaVersion{kLocalMaterializationDecisionSchemaVersion};
    std::vector<LocalMaterializationDecision> decisions;

    bool operator==(const LocalMaterializationDecisionDocument&) const = default;
};

enum class MaterializationDecisionStoreCode : std::uint8_t {
    Success = 0,
    Missing,
    TooLarge,
    InvalidDocument,
    ParseError,
    UnknownField,
    ReadFailed,
    WriteFailed,
    RemoveFailed,
    CleanupFailed,
};

struct MaterializationDecisionStoreDiagnostic {
    MaterializationDecisionStoreCode code{MaterializationDecisionStoreCode::Success};
    std::string message;

    bool succeeded() const noexcept {
        return code == MaterializationDecisionStoreCode::Success ||
               code == MaterializationDecisionStoreCode::Missing;
    }
    bool found() const noexcept { return code != MaterializationDecisionStoreCode::Missing; }
};

MaterializationDecisionStoreDiagnostic validateLocalMaterializationDecisionDocument(
    const LocalMaterializationDecisionDocument& document);
MaterializationDecisionStoreDiagnostic encodeLocalMaterializationDecisionDocumentJson(
    const LocalMaterializationDecisionDocument& document,
    std::string& output);
MaterializationDecisionStoreDiagnostic decodeLocalMaterializationDecisionDocumentJson(
    std::string_view bytes,
    LocalMaterializationDecisionDocument& output);

class LocalMaterializationDecisionStore final {
public:
    explicit LocalMaterializationDecisionStore(std::filesystem::path path)
        : path_(std::move(path)) {}

    MaterializationDecisionStoreDiagnostic load(LocalMaterializationDecisionDocument& document) const;
    MaterializationDecisionStoreDiagnostic save(const LocalMaterializationDecisionDocument& document) const;
    MaterializationDecisionStoreDiagnostic remove() const;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path temporaryPath() const;

    std::filesystem::path path_;
};

struct MaterializationDecisionQuery {
    std::string setupId;
    std::uint32_t instanceIndex{0};
    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::uint64_t providerMetadataRevision{0};
    std::uint64_t requirementRevision{0};
    std::optional<profile::CompatibilityReference> compatibility;

    bool operator==(const MaterializationDecisionQuery&) const = default;
};

enum class TrustedMaterializationDecisionCode : std::uint8_t {
    Success = 0,
    NotRequired,
    InvalidStore,
    UntrustedOrigin,
    IdentityMismatch,
};

struct TrustedMaterializationDecisionDiagnostic {
    TrustedMaterializationDecisionCode code{TrustedMaterializationDecisionCode::Success};
    std::string message;

    bool succeeded() const noexcept {
        return code == TrustedMaterializationDecisionCode::Success ||
               code == TrustedMaterializationDecisionCode::NotRequired;
    }
    bool found() const noexcept { return code == TrustedMaterializationDecisionCode::Success; }
};

class ITrustedMaterializationDecisionSource {
public:
    virtual ~ITrustedMaterializationDecisionSource() = default;
    virtual TrustedMaterializationDecisionDiagnostic resolveCurrent(
        const MaterializationDecisionQuery& query,
        LocalMaterializationDecision& output) = 0;
};

class StoreBackedTrustedMaterializationDecisionSource final
    : public ITrustedMaterializationDecisionSource {
public:
    explicit StoreBackedTrustedMaterializationDecisionSource(std::filesystem::path path)
        : store_(std::move(path)) {}

    TrustedMaterializationDecisionDiagnostic resolveCurrent(
        const MaterializationDecisionQuery& query,
        LocalMaterializationDecision& output) override;

private:
    LocalMaterializationDecisionStore store_;
};

std::optional<std::filesystem::path> defaultLocalMaterializationDecisionStorePath(
    std::string* error = nullptr);
std::optional<std::filesystem::path> defaultInstanceMaterializationRoot(
    std::string* error = nullptr);
std::string_view materializationDecisionStoreCodeName(
    MaterializationDecisionStoreCode code) noexcept;
std::string_view trustedMaterializationDecisionCodeName(
    TrustedMaterializationDecisionCode code) noexcept;
std::string_view localMaterializationDecisionOriginName(
    LocalMaterializationDecisionOrigin origin) noexcept;

} // namespace hydra::materialization
