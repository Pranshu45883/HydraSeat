#pragma once

#include "hydra/artifact_trust.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::data {

inline constexpr std::size_t kMaximumCatalogCacheHistory = 8u;

struct CatalogArtifact {
    std::string catalogId;
    std::uint64_t revision{0};
    std::string version;
    std::string expectedSha256;
    std::string observedSha256;
    std::string sourceId;
    std::string licenseId;
    bool redistributionAllowed{false};

    bool operator==(const CatalogArtifact&) const = default;
};

struct CatalogRefreshPolicy {
    bool refreshChecksEnabled{true};
    bool downloadsEnabled{true};
    bool sourceConfigured{true};
    bool requireRedistributionPermission{false};
};

enum class CatalogRefreshState : std::uint8_t {
    Applied = 0,
    UpToDate,
    Disabled,
    OfflineUsingCache,
    OfflineNoCache,
    NoSourceConfigured,
    DownloadDisabled,
    Rejected,
    RolledBack,
};

enum class CatalogRefreshCode : std::uint8_t {
    Success = 0,
    InvalidArtifact,
    StaleRevision,
    TrustRejected,
    NoCurrentCache,
    NoRollbackVersion,
};

struct CatalogRefreshResult {
    CatalogRefreshState state{CatalogRefreshState::Rejected};
    CatalogRefreshCode code{CatalogRefreshCode::InvalidArtifact};
    std::string message;

    bool succeeded() const noexcept {
        return state != CatalogRefreshState::Rejected;
    }
};

struct CatalogRefreshInput {
    bool networkAvailable{false};
    std::optional<CatalogArtifact> downloaded;
};

// Pure cache lifecycle. Network/file code supplies observations; this model never
// performs I/O. Failed/offline/disabled refreshes preserve the last valid cache,
// and the data artifact has no executable/script capability.
class CatalogCacheModel {
public:
    CatalogRefreshResult refresh(const CatalogRefreshInput& input,
                                 const CatalogRefreshPolicy& policy,
                                 const trust::TrustPolicy& trustPolicy);
    CatalogRefreshResult rollback();
    void clear() noexcept;

    const std::optional<CatalogArtifact>& current() const noexcept { return current_; }
    const std::vector<CatalogArtifact>& history() const noexcept { return history_; }

private:
    std::optional<CatalogArtifact> current_;
    std::vector<CatalogArtifact> history_;
};

std::string_view catalogRefreshStateName(CatalogRefreshState value) noexcept;
std::string_view catalogRefreshCodeName(CatalogRefreshCode value) noexcept;

} // namespace hydra::data
