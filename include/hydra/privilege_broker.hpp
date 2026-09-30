#pragma once

#include "hydra/artifact_trust.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hydra::privilege {

inline constexpr std::uint32_t kPrivilegeBrokerSchemaVersion = 1u;
inline constexpr std::size_t kMaximumBrokerIdentityBytes = 184u;
inline constexpr std::size_t kMaximumBrokerRequestIdBytes = 96u;

// Resource identifiers are compiled into the broker. A caller cannot provide a
// filesystem/registry/service/driver path or an executable command line.
enum class BrokerResource : std::uint8_t {
    RuntimeService = 0,
    WatchdogService = 1,
    RecoveryTool = 2,
};

enum class BrokerOperation : std::uint8_t {
    Install = 0,
    Repair = 1,
    Remove = 2,
};

struct BrokerRequest {
    std::uint32_t schemaVersion{kPrivilegeBrokerSchemaVersion};
    std::string requestId;
    std::uint64_t channelNonce{0};
    std::uint64_t sequence{0};
    BrokerResource resource{BrokerResource::RuntimeService};
    BrokerOperation operation{BrokerOperation::Install};
    std::optional<trust::ArtifactManifest> artifactManifest;
    std::optional<trust::ArtifactObservation> artifactObservation;
};

// Filled by the authenticated IPC/native boundary, not from request payload.
struct AuthenticatedPeer {
    std::string callerUserSid;
    std::string brokerOwnerUserSid;
    std::uint64_t channelNonce{0};
    bool brokerProcessElevated{false};
};

struct BrokerPolicy {
    trust::TrustPolicy artifactTrust;
};

enum class BrokerCode : std::uint8_t {
    Success = 0,
    InvalidSchema,
    InvalidRequestIdentity,
    InvalidPeerIdentity,
    BrokerNotElevated,
    WrongUser,
    WrongChannel,
    ReplayOrOutOfOrder,
    InvalidResource,
    InvalidOperation,
    UnexpectedArtifact,
    MissingArtifact,
    ArtifactIdentityMismatch,
    ArtifactClassMismatch,
    ArtifactCapabilityMismatch,
    ArtifactTrustRejected,
    CaptureFailed,
    ApplyFailedRolledBack,
    VerifyFailedRolledBack,
    RollbackFailed,
    RollbackVerifyFailed,
    ArtifactAuthorityUnavailable,
    ArtifactAuthorityMismatch,
};

struct BrokerDiagnostic {
    BrokerCode code{BrokerCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == BrokerCode::Success; }
};

enum class BrokerReceiptStatus : std::uint8_t {
    Applied = 0,
    Removed = 1,
    RolledBack = 2,
    RecoveryRequired = 3,
};

struct BrokerReceipt {
    std::string requestId;
    std::uint64_t sequence{0};
    BrokerResource resource{BrokerResource::RuntimeService};
    BrokerOperation operation{BrokerOperation::Install};
    BrokerReceiptStatus status{BrokerReceiptStatus::Applied};
    bool rollbackAttempted{false};
    bool rollbackVerified{false};

    bool operator==(const BrokerReceipt&) const = default;
};

// Resolves the broker-owned release manifest and observes the actual fixed
// resource. Implementations must not derive either result from caller payload.
// Paths, signature verification, and hashing stay behind this elevated/native
// boundary; false means the resource could not be observed safely.
class PrivilegedArtifactAuthority {
public:
    virtual ~PrivilegedArtifactAuthority() = default;
    virtual bool resolve(BrokerResource resource,
                         trust::ArtifactManifest& manifest,
                         trust::ArtifactObservation& observation) noexcept = 0;
};

// The native/elevated executor resolves the enum resource to broker-owned paths,
// SCM/registry/device identifiers. None of those authorities are supplied by the
// unelevated caller. Every mutation is snapshot/capture-first and rollback-capable.
class PrivilegedMutationExecutor {
public:
    virtual ~PrivilegedMutationExecutor() = default;
    virtual bool capture(BrokerResource resource, std::string& snapshotId) noexcept = 0;
    virtual bool apply(BrokerResource resource,
                       BrokerOperation operation,
                       const std::optional<trust::ArtifactManifest>& manifest,
                       const std::optional<trust::ArtifactObservation>& observation,
                       std::string_view snapshotId) noexcept = 0;
    virtual bool verify(BrokerResource resource, BrokerOperation operation) noexcept = 0;
    virtual bool rollback(BrokerResource resource, std::string_view snapshotId) noexcept = 0;
    virtual bool verifyRollback(BrokerResource resource, std::string_view snapshotId) noexcept = 0;
};

class PrivilegeBrokerSession final {
public:
    explicit PrivilegeBrokerSession(BrokerPolicy policy,
                                    std::uint64_t firstExpectedSequence = 1u)
        : policy_(std::move(policy)), nextSequence_(firstExpectedSequence) {}

    BrokerDiagnostic execute(const BrokerRequest& request,
                             const AuthenticatedPeer& peer,
                             PrivilegedArtifactAuthority& artifactAuthority,
                             PrivilegedMutationExecutor& executor,
                             BrokerReceipt& receipt);

    std::uint64_t nextExpectedSequence() const noexcept { return nextSequence_; }

private:
    BrokerDiagnostic authorize(const BrokerRequest& request,
                               const AuthenticatedPeer& peer) const;

    BrokerPolicy policy_;
    std::uint64_t nextSequence_{1u};
};

std::string_view brokerResourceName(BrokerResource value) noexcept;
std::string_view brokerOperationName(BrokerOperation value) noexcept;
std::string_view brokerCodeName(BrokerCode value) noexcept;
std::string_view brokerReceiptStatusName(BrokerReceiptStatus value) noexcept;

} // namespace hydra::privilege
