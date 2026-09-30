#pragma once

#include "hydra/audio_endpoint_inventory.hpp"
#include "hydra/audio_routing.hpp"
#include "hydra/compatibility_local_store.hpp"
#include "hydra/controller_runtime.hpp"
#include "hydra/game_runtime_requirement_resolver.hpp"
#include "hydra/process_group.hpp"
#include "hydra/provider_launch_plan.hpp"
#include "hydra/runtime_state.hpp"
#include "hydra/two_seat_launch.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::production {

inline constexpr std::uint32_t kProviderPlanInstallSchemaVersion = 1u;
inline constexpr std::size_t kProviderPlanRegistryMaxEntries = 2u;

enum class ProviderPlanInstallCode : std::uint8_t {
    Ok = 0,
    AlreadySatisfied = 1,
    InvalidVersion = 2,
    InvalidPlan = 3,
    InvalidProfile = 4,
    InvalidSession = 5,
    InvalidSeat = 6,
    InvalidSeatGeneration = 7,
    StaleRegistryRevision = 8,
    ReplayRejected = 9,
    SeatNotIdle = 10,
    Unsupported = 11,
    BackendFailure = 12,
};

struct ProviderPlanRegistryEntry {
    SeatId seatId{0};
    std::uint64_t installedRevision{0};
    std::uint64_t planFingerprint{0};
    std::uint64_t planRevision{0};
    std::uint64_t profileFingerprint{0};
    runtime::RuntimeSessionId sessionId{};
    std::uint64_t sessionGeneration{0};
    std::uint64_t seatGameGeneration{0};
    std::string playerId;
    std::string gameId;

    bool operator==(const ProviderPlanRegistryEntry&) const = default;
};

struct ProviderPlanRegistrySnapshot {
    std::uint32_t schemaVersion{kProviderPlanInstallSchemaVersion};
    std::uint64_t registryRevision{0};
    std::uint64_t profileFingerprint{0};
    runtime::RuntimeSessionId sessionId{};
    std::uint64_t sessionGeneration{0};
    std::vector<ProviderPlanRegistryEntry> entries;

    bool operator==(const ProviderPlanRegistrySnapshot&) const = default;
};

struct ProviderPlanInstallRequest {
    std::uint32_t schemaVersion{kProviderPlanInstallSchemaVersion};
    SeatId seatId{0};
    std::uint64_t expectedRegistryRevision{0};
    std::uint64_t planFingerprint{0};
    std::uint64_t planRevision{0};
    std::uint64_t profileFingerprint{0};
    runtime::RuntimeSessionId sessionId{};
    std::uint64_t sessionGeneration{0};
    std::uint64_t seatGameGeneration{0};
    plan::ProviderAwareLaunchPlan plan;

    bool operator==(const ProviderPlanInstallRequest&) const = default;
};

struct ProviderPlanRemoveRequest {
    std::uint32_t schemaVersion{kProviderPlanInstallSchemaVersion};
    SeatId seatId{0};
    std::uint64_t expectedRegistryRevision{0};
    std::uint64_t planFingerprint{0};
    std::uint64_t planRevision{0};
    std::uint64_t profileFingerprint{0};
    runtime::RuntimeSessionId sessionId{};
    std::uint64_t sessionGeneration{0};
    std::uint64_t seatGameGeneration{0};

    bool operator==(const ProviderPlanRemoveRequest&) const = default;
};

struct ProviderPlanInstallResult {
    ProviderPlanInstallCode code{ProviderPlanInstallCode::BackendFailure};
    ProviderPlanRegistrySnapshot registry;
    std::string diagnostic;

    bool succeeded() const noexcept {
        return code == ProviderPlanInstallCode::Ok ||
               code == ProviderPlanInstallCode::AlreadySatisfied;
    }
};

// The host is the sole owner of this registry. Clients may submit immutable,
// already-compiled provider plans through the bounded host protocol, but the
// implementation re-hashes and re-binds every plan to the host's exact current
// profile/session/Seat generation before a factory can resolve it.
class IHostLaunchPlanRegistry : public runtime::ISeatGameInstanceFactory {
public:
    ~IHostLaunchPlanRegistry() override = default;

    virtual void resetContext(std::uint64_t profileFingerprint,
                              const runtime::RuntimeSessionId& sessionId,
                              std::uint64_t sessionGeneration,
                              std::span<const SeatConfig> configuredSeats) = 0;
    virtual ProviderPlanInstallResult install(
        const ProviderPlanInstallRequest& request) = 0;
    virtual ProviderPlanInstallResult remove(
        const ProviderPlanRemoveRequest& request) = 0;
    virtual ProviderPlanRegistrySnapshot registrySnapshot() const = 0;
};

// Immutable host-owned activation epoch. This identity exists before Process is
// created and is the only context that Recovery/Input bridge resources may rely
// on during their initial create/prepare calls.
struct ProductionActivationEpoch {
    SeatId seatId{0};
    runtime::RuntimeSessionId sessionId{};
    std::uint64_t sessionGeneration{0};
    std::uint64_t seatGameGeneration{0};
    std::uint64_t activationFingerprint{0};

    bool valid() const noexcept {
        return seatId != 0 && !sessionId.empty() && sessionGeneration != 0 &&
               seatGameGeneration != 0 && activationFingerprint != 0;
    }

    bool operator==(const ProductionActivationEpoch&) const = default;
};

enum class ProductionActivationContextStage : std::uint8_t {
    PreProcess = 0,
    ProcessActive = 1,
    HandoffPending = 2,
    ProcessExited = 3,
    ProcessUnverifiable = 4,
    UnsupportedContainment = 5,
    ProcessInvalidated = 6,
};

// Published only after ProcessResource verifies the exact Seat-owned handoff.
// Consumers receive a value snapshot, never mutable process-group internals.
struct ProductionProcessActivatedContext {
    ProductionActivationEpoch epoch;
    process::ProcessIdentity authoritativeProcess;
    std::uint64_t handoffGeneration{0};
    std::uint64_t treeSequence{0};
    process::ProcessHandoffState handoffState{
        process::ProcessHandoffState::Unverifiable};

    bool valid() const noexcept {
        return epoch.valid() && authoritativeProcess.valid() &&
               (handoffState == process::ProcessHandoffState::RootActive ||
                handoffState == process::ProcessHandoffState::DescendantActive);
    }

    bool operator==(const ProductionProcessActivatedContext&) const = default;
};

struct ProductionActivationContextSnapshot {
    ProductionActivationEpoch epoch;
    ProductionActivationContextStage stage{
        ProductionActivationContextStage::PreProcess};
    std::optional<ProductionProcessActivatedContext> process;
    std::optional<process::ProcessHandoffState> handoffState;
    std::uint64_t handoffGeneration{0};
    std::uint64_t treeSequence{0};

    bool operator==(const ProductionActivationContextSnapshot&) const = default;
};

// Bridge consumers hold this read-only authority. They can validate a copied
// epoch/process snapshot but cannot publish arbitrary ProcessIdentity values.
class IProductionActivationContext {
public:
    virtual ~IProductionActivationContext() = default;
    virtual ProductionActivationContextSnapshot snapshot() const = 0;
    virtual bool validatesEpoch(const ProductionActivationEpoch& epoch) const noexcept = 0;
    virtual bool validatesCurrentProcess(
        const ProductionProcessActivatedContext& process) const noexcept = 0;
};

using ProductionActivationContextHandle =
    std::shared_ptr<const IProductionActivationContext>;

// P5 has a typed Input resource slot, but the concrete Gate-C activation path is
// not exposed as a production P5 public factory. The production launch runtime
// therefore requires an explicit bridge rather than pretending that a no-op is
// input isolation. A host composition that does not install this bridge fails
// closed only for plans that actually require keyboard/mouse isolation.
class IProductionInputResourceBridge {
public:
    virtual ~IProductionInputResourceBridge() = default;
    virtual std::unique_ptr<launch::ISeatActivationResource> createInputResource(
        const launch::SeatActivationPlan& plan,
        ProductionActivationContextHandle context,
        std::string& error) = 0;
};

// Recovery is an ordering-critical resource: it is created before Process.
// Its bridge therefore receives a PreProcess context first. The same read-only
// authority starts publishing an exact process only after Process verification.
class IProductionRecoveryResourceBridge {
public:
    virtual ~IProductionRecoveryResourceBridge() = default;
    virtual std::unique_ptr<launch::ISeatActivationResource> createRecoveryResource(
        const launch::SeatActivationPlan& plan,
        ProductionActivationContextHandle context,
        std::string& error) = 0;
};

struct ProductionLaunchServices {
    std::shared_ptr<IProductionRecoveryResourceBridge> recoveryBridge;
    std::shared_ptr<IProductionInputResourceBridge> inputBridge;
    std::shared_ptr<controller::SourceBackend> controllerBackend;
    std::shared_ptr<audio::EndpointSource> endpointSource;
    std::shared_ptr<audio::SessionSource> sessionSource;
    std::shared_ptr<audio::RouteBackend> audioRouteBackend;
};

// Production resource factories consume the exact host-owned activation epoch
// before creating Seat resources. Keeping this contract explicit avoids runtime
// downcasts from the generic launch factory interface.
class IProductionSeatActivationResourceFactory
    : public launch::ISeatActivationResourceFactory {
public:
    ~IProductionSeatActivationResourceFactory() override = default;

    virtual bool bindActivationEpoch(
        const launch::SeatActivationPlan& plan,
        const ProductionActivationEpoch& epoch,
        std::string& error) = 0;
    virtual bool bindTrustedHandoffExecutables(
        const launch::SeatActivationPlan& plan,
        std::vector<std::wstring> executablePaths,
        std::string& error) = 0;
};

// Reuses the existing P5 transaction interface. The default implementation uses
// real ProcessLauncher/WindowTracker/display inventory/controller inventory/Core
// Audio observation and the injected Gate-C input bridge. Unsupported mutation
// paths fail closed; they are never silently replaced with synthetic resources.
std::shared_ptr<IProductionSeatActivationResourceFactory>
makeProductionSeatActivationResourceFactory(
    ProductionLaunchServices services = {});

class HostProviderPlanRegistry final : public IHostLaunchPlanRegistry {
public:
    explicit HostProviderPlanRegistry(
        std::shared_ptr<IProductionSeatActivationResourceFactory> resources,
        std::shared_ptr<requirement::ITrustedRequirementSource> trustedRequirements = {},
        std::shared_ptr<materialization::ITrustedMaterializationDecisionSource>
            trustedMaterializations = {},
        std::filesystem::path materializationInstancesRoot = {});
    explicit HostProviderPlanRegistry(
        ProductionLaunchServices services,
        std::shared_ptr<requirement::ITrustedRequirementSource> trustedRequirements = {},
        std::shared_ptr<materialization::ITrustedMaterializationDecisionSource>
            trustedMaterializations = {},
        std::filesystem::path materializationInstancesRoot = {});

    void resetContext(std::uint64_t profileFingerprint,
                      const runtime::RuntimeSessionId& sessionId,
                      std::uint64_t sessionGeneration,
                      std::span<const SeatConfig> configuredSeats) override;
    ProviderPlanInstallResult install(
        const ProviderPlanInstallRequest& request) override;
    ProviderPlanInstallResult remove(
        const ProviderPlanRemoveRequest& request) override;
    ProviderPlanRegistrySnapshot registrySnapshot() const override;

    std::unique_ptr<runtime::ISeatGameInstance> create(
        SeatId seatId, std::string& error) override;
    std::unique_ptr<runtime::ISeatGameInstance> createForBinding(
        SeatId seatId, const runtime::SeatGameBinding& binding,
        std::uint64_t expectedGeneration, std::string& error) override;

private:
    struct StoredPlan {
        ProviderPlanRegistryEntry entry;
        plan::ProviderAwareLaunchPlan providerPlan;
        plan::SeatProviderLaunchPlan seatPlan;
        SeatConfig seatConfig;
    };

    ProviderPlanRegistrySnapshot snapshotLocked() const;

    mutable std::mutex mutex_;
    std::uint64_t registryRevision_{0};
    std::uint64_t profileFingerprint_{0};
    runtime::RuntimeSessionId sessionId_{};
    std::uint64_t sessionGeneration_{0};
    std::vector<SeatConfig> configuredSeats_;
    std::vector<StoredPlan> plans_;
    std::shared_ptr<IProductionSeatActivationResourceFactory> resources_;
    std::shared_ptr<requirement::ITrustedRequirementSource> trustedRequirements_;
    std::shared_ptr<materialization::ITrustedMaterializationDecisionSource>
        trustedMaterializations_;
    std::filesystem::path materializationInstancesRoot_;
};

// Canonical validators shared by the host, installer and protocol tests. They
// intentionally reproduce the immutable P6 compiler hash rather than trusting a
// client-provided numeric fingerprint.
std::uint64_t providerPlanFingerprint(
    const plan::ProviderAwareLaunchPlan& plan) noexcept;
std::uint64_t providerPlanRevision(
    const plan::SeatProviderLaunchPlan& seat) noexcept;
std::uint64_t seatHardwareFingerprint(const SeatConfig& seat) noexcept;

std::string_view providerPlanInstallCodeName(
    ProviderPlanInstallCode code) noexcept;

} // namespace hydra::production
