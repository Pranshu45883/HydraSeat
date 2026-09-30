#pragma once

#include "hydra/gate_c_protocol.hpp"
#include "hydra/hidhide_session_backend.hpp"
#include "hydra/input_metrics.hpp"
#include "hydra/production_launch_runtime.hpp"
#include "hydra/recovery_process_attachment.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::production {

inline constexpr std::size_t kMaximumProductionGateCProfiles = 64u;
inline constexpr std::size_t kMaximumProductionGateCExecutablePaths = 32u;
inline constexpr std::uint32_t kDefaultProductionRecoveryLeaseTimeoutMs = 20'000u;
inline constexpr std::uint32_t kDefaultProductionRecoveryRollbackTimeoutMs = 6'000u;
inline constexpr std::uint32_t kDefaultProductionRecoveryActionTimeoutMs = 2'000u;
inline constexpr std::uint32_t kDefaultProductionGateCHandshakeTimeoutMs = 5'000u;
inline constexpr std::uint32_t kDefaultProductionGateCIoTimeoutMs = 500u;
inline constexpr std::uint32_t kDefaultProductionBridgeMonitorIntervalMs = 25u;

enum class ProductionInputEvidenceClass : std::uint8_t {
    None = 0,
    Controlled = 1,
    Synthetic = 2,
    Physical = 3,
};

// Trusted production composition supplies one profile per supported game/version
// family. Runtime attach is explicit because the existing P3-E harness exposes no
// general attach command. A profile never authorizes protected/anti-cheat bypass;
// those targets remain outside PRODUCT_V1.
struct ProductionGateCProfile {
    std::string gameId;
    std::uint32_t requiredApiMask{0};
    std::vector<std::wstring> allowedProcessExecutablePaths;
    std::filesystem::path bridgeLibraryPath;
    bool runtimeAttachApproved{false};
    bool physicalCloakingRequired{true};
    bool nativeHidHideMutationApproved{false};
    bool spareRecoveryInputPresent{false};
    std::uint32_t hidHideExpiryMilliseconds{10'000u};
    std::vector<std::wstring> hidHideAllowedApplications;

    bool operator==(const ProductionGateCProfile&) const = default;
};

struct ProductionActivationBridgeConfig {
    std::vector<ProductionGateCProfile> gateCProfiles;
    ProductionInputEvidenceClass inputEvidenceClass{
        ProductionInputEvidenceClass::None};
    std::optional<Phase3HardwareAcceptanceEvidence> physicalAcceptanceEvidence;

    // Base root. The implementation derives one bounded Seat-local recovery
    // directory so two independent watchdog/reset registrations cannot overwrite
    // each other. hydra_reset already accepts --recovery-dir for these roots.
    std::filesystem::path recoveryRoot;
    std::filesystem::path watchdogExecutablePath;

    std::shared_ptr<HidHideSessionPlatform> hidHidePlatform;
    std::uint32_t recoveryLeaseTimeoutMilliseconds{
        kDefaultProductionRecoveryLeaseTimeoutMs};
    std::uint32_t recoveryRollbackTimeoutMilliseconds{
        kDefaultProductionRecoveryRollbackTimeoutMs};
    std::uint32_t recoveryActionTimeoutMilliseconds{
        kDefaultProductionRecoveryActionTimeoutMs};
    std::uint32_t gateCHandshakeTimeoutMilliseconds{
        kDefaultProductionGateCHandshakeTimeoutMs};
    std::uint32_t gateCIoTimeoutMilliseconds{kDefaultProductionGateCIoTimeoutMs};
    std::uint32_t monitorIntervalMilliseconds{
        kDefaultProductionBridgeMonitorIntervalMs};
};

struct ProductionGateCSessionRequest {
    ProductionActivationEpoch epoch;
    ProductionProcessActivatedContext process;
    SeatConfig seat;
    ProductionGateCProfile profile;
    std::vector<std::wstring> assignedStableDeviceIds;

    bool valid() const noexcept {
        return epoch.valid() && process.valid() && process.epoch == epoch &&
               seat.seatId == epoch.seatId && profile.gameId.size() != 0 &&
               !assignedStableDeviceIds.empty();
    }
};

struct ProductionGateCSessionStatus {
    bool active{false};
    bool receiverVerified{false};
    bool assignedDevicesPresent{false};
    process::ProcessIdentity process;
    std::uint64_t handoffGeneration{0};
    std::uint64_t receiverSequence{0};

    bool operator==(const ProductionGateCSessionStatus&) const = default;
};

// Test seam around the real watchdog/journal/reset transport. Identity and lease
// authority remain RecoveryProcessAttachmentAuthority; implementations may not
// manufacture or replace that authority.
class IProductionRecoveryLeaseRuntime {
public:
    virtual ~IProductionRecoveryLeaseRuntime() = default;

    virtual bool arm(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::string& error) = 0;
    virtual bool verifyArmed(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::string& error) = 0;
    virtual bool markActionActive(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::uint32_t actionId,
        std::string& error) = 0;
    virtual bool commit(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::string& error) = 0;
    virtual bool disarm(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::span<const std::uint32_t> verifiedRolledBackActionIds,
        std::string& error) = 0;
    virtual bool verifyDisarmed(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::string& error) = 0;
};

// Test seam around the real exact-process Gate-C transport. The native runtime
// verifies PID+creation-time again before mutation, performs authenticated
// receiver handshake/query verification, and owns the Seat-local Raw Input sink.
class IProductionGateCSessionRuntime {
public:
    virtual ~IProductionGateCSessionRuntime() = default;

    virtual bool start(const ProductionGateCSessionRequest& request,
                       std::string& error) = 0;
    virtual bool verify(const ProductionGateCSessionRequest& request,
                        ProductionGateCSessionStatus& status,
                        std::string& error) = 0;
    // Read-only, bounded trace of real Raw Input delivery through this exact
    // receiver session. Implementations must not synthesize missing receiver
    // stages or treat mere device presence as applied input evidence.
    virtual bool inputMetricsSnapshot(
        const ProductionGateCSessionRequest& request,
        InputMetricsSnapshot& snapshot,
        std::string& error) = 0;
    virtual bool stop(const ProductionGateCSessionRequest& request,
                      std::string& error) noexcept = 0;
};

struct ProductionActivationBridgeDependencies {
    std::shared_ptr<recovery::RecoveryProcessAttachmentAuthority>
        recoveryAttachmentAuthority;
    std::shared_ptr<IProductionRecoveryLeaseRuntime> recoveryLeaseRuntime;
    std::shared_ptr<IProductionGateCSessionRuntime> gateCSessionRuntime;
};

// One bounded bridge object implements both production slots so Recovery and
// Input for the same immutable activation epoch share only the narrow state they
// must coordinate. It is not a second launch/input manager.
class ProductionActivationResourceBridges final
    : public IProductionRecoveryResourceBridge,
      public IProductionInputResourceBridge {
public:
    ProductionActivationResourceBridges(
        ProductionActivationBridgeConfig config,
        ProductionActivationBridgeDependencies dependencies = {});
    ~ProductionActivationResourceBridges() override;

    ProductionActivationResourceBridges(
        const ProductionActivationResourceBridges&) = delete;
    ProductionActivationResourceBridges& operator=(
        const ProductionActivationResourceBridges&) = delete;

    std::unique_ptr<launch::ISeatActivationResource> createRecoveryResource(
        const launch::SeatActivationPlan& plan,
        ProductionActivationContextHandle context,
        std::string& error) override;

    std::unique_ptr<launch::ISeatActivationResource> createInputResource(
        const launch::SeatActivationPlan& plan,
        ProductionActivationContextHandle context,
        std::string& error) override;

    // Read-only observation seam for guided Physical validation. This returns
    // receiver-aware metrics only for the exact currently active Seat/fingerprint
    // session already owned by this bridge. It never creates a resource, starts a
    // process/session, or synthesizes missing receiver evidence.
    bool inputMetricsSnapshot(
        const launch::SeatActivationPlan& plan,
        InputMetricsSnapshot& snapshot,
        std::string& error);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::shared_ptr<ProductionActivationResourceBridges>
makeProductionActivationResourceBridges(
    ProductionActivationBridgeConfig config,
    ProductionActivationBridgeDependencies dependencies = {});

// Base Host composition. It resolves the canonical per-user recovery root and
// sibling watchdog, then freshly reloads any user-selected P3-HW manifest through
// the typed production-input-authority source. Exact Game Gate-C profiles remain
// release-owned; without a reviewed exact profile, Input still fails closed even
// when accepted Physical hardware evidence is present.
std::shared_ptr<ProductionActivationResourceBridges>
makeDefaultProductionActivationResourceBridges(std::string* error = nullptr);

std::shared_ptr<IProductionRecoveryLeaseRuntime>
makeNativeProductionRecoveryLeaseRuntime(
    std::filesystem::path recoveryRoot,
    std::filesystem::path watchdogExecutablePath,
    std::uint32_t watchdogHandshakeTimeoutMilliseconds =
        kDefaultProductionGateCHandshakeTimeoutMs);

std::shared_ptr<IProductionGateCSessionRuntime>
makeNativeProductionGateCSessionRuntime(
    std::uint32_t handshakeTimeoutMilliseconds =
        kDefaultProductionGateCHandshakeTimeoutMs,
    std::uint32_t ioTimeoutMilliseconds = kDefaultProductionGateCIoTimeoutMs);

std::string_view productionInputEvidenceClassName(
    ProductionInputEvidenceClass value) noexcept;

} // namespace hydra::production
