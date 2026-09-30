#pragma once

#include "hydra/installer_transaction.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hydra::update {

inline constexpr std::uint32_t kUpdateContractVersion = 1u;

enum class UpdateDirection : std::uint8_t { Upgrade = 0, Rollback = 1 };

struct ApplicationUpdateOffer {
    std::uint32_t schemaVersion{kUpdateContractVersion};
    std::string releaseNotesId;
    installer::InstallerPackage package;
    bool restartRecommended{false};

    bool operator==(const ApplicationUpdateOffer&) const = default;
};

struct UpdatePreview {
    UpdateDirection direction{UpdateDirection::Upgrade};
    std::string currentVersion;
    std::uint64_t currentRevision{0};
    std::string targetVersion;
    std::uint64_t targetRevision{0};
    std::string releaseNotesId;
    bool restartRecommended{false};
    std::string approvalIdentity;

    bool operator==(const UpdatePreview&) const = default;
};

struct UpdateApproval {
    bool userApproved{false};
    std::string approvalIdentity;
};

enum class UpdateCode : std::uint8_t {
    Success = 0,
    InvalidOffer,
    ArchitectureMismatch,
    NotNewer,
    InvalidRollbackTarget,
    TrustRejected,
    ApprovalRequired,
    ApprovalMismatch,
    StaleInstalledState,
    InstallerRejected,
};

struct UpdateDiagnostic {
    UpdateCode code{UpdateCode::Success};
    std::optional<installer::InstallerCode> installerCode;
    std::string message;

    bool succeeded() const noexcept { return code == UpdateCode::Success; }
};

// Check/preview is pure and never mutates install state. Application updates and
// executable rollback always require an exact target-bound approval and reuse the
// P8 installer transaction for trust, verification, and rollback.
UpdateDiagnostic previewApplicationUpdate(
    const installer::InstalledState& current,
    const ApplicationUpdateOffer& offer,
    UpdateDirection direction,
    const installer::InstallerEnvironment& environment,
    const trust::TrustPolicy& trustPolicy,
    UpdatePreview& preview);

UpdateDiagnostic applyApplicationUpdate(
    const installer::InstalledState& current,
    const ApplicationUpdateOffer& offer,
    UpdateDirection direction,
    const UpdateApproval& approval,
    const installer::InstallerEnvironment& environment,
    const installer::InstallerOptions& options,
    const trust::TrustPolicy& trustPolicy,
    installer::InstallerExecutor& executor,
    installer::InstallerReceipt& receipt);

std::string makeApprovalIdentity(const ApplicationUpdateOffer& offer);
std::string_view updateDirectionName(UpdateDirection value) noexcept;
std::string_view updateCodeName(UpdateCode value) noexcept;

} // namespace hydra::update
