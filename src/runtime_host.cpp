#include "hydra/runtime_host.hpp"
#include "hydra/raw_input_utils.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace hydra::runtime {

namespace {

hostipc::SeatSnapshot toHostSnapshot(const SeatRuntimeSnapshot& snapshot) noexcept {
    return {
        snapshot.seatId,
        snapshot.generation,
        snapshot.active,
        snapshot.uiLeaseActive,
        snapshot.gameLeaseActive,
        snapshot.process.has_value(),
        snapshot.targetHwnd != 0,
        snapshot.controllerBinding.has_value(),
        snapshot.process ? snapshot.process->pid : 0u,
        snapshot.process ? snapshot.process->creationIdentity : 0u,
        static_cast<std::uint64_t>(snapshot.targetHwnd),
    };
}

} // namespace

RuntimeHost::RuntimeHost(std::filesystem::path seatConfigPath) {
    seatHardwareStore_.emplace(std::move(seatConfigPath));
}

bool RuntimeHost::loadPersistentSeatHardware(std::string* error) {
    std::lock_guard lock(mutex_);
    if (!seatHardwareStore_) return true;

    auto candidate = hardwareConfigurations_;
    if (!seatHardwareStore_->load(candidate, error)) {
        return false;
    }

    hardwareConfigurations_ = std::move(candidate);
    return true;
}

hostipc::HostSnapshot RuntimeHost::snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    hostipc::HostSnapshot result;
    result.authorityRevision = authorityRevision_;
    const auto seat1 = controller_.snapshot(1);
    const auto seat2 = controller_.snapshot(2);
    if (seat1) result.seats[0] = toHostSnapshot(*seat1);
    if (seat2) result.seats[1] = toHostSnapshot(*seat2);
    return result;
}

std::optional<SeatRuntimeSnapshot> RuntimeHost::seatSnapshot(
    std::uint32_t seatId) const noexcept {
    std::lock_guard lock(mutex_);
    return controller_.snapshot(seatId);
}

controller::InventorySnapshot RuntimeHost::controllerInventorySnapshot() noexcept {
    // XInput/SetupAPI enumeration can be slow on device churn. Keep it off the
    // authority mutex so read-only snapshots cannot be starved by enumeration.
    std::lock_guard lock(controllerInventoryMutex_);
    return controllerInventory_.scan();
}

std::vector<DeviceInfo> RuntimeHost::hardwareInventory() {
    // Enumerate Windows devices without holding runtime authority. Only the
    // tiny activity-confirmation filter needs the authority mutex.
    auto displays = hardwareDetector_.detectDisplays();
    auto keyboards = hardwareDetector_.detectKeyboards();
    auto mice = hardwareDetector_.detectMice();
    auto controllers = hardwareDetector_.detectControllers();

    {
        std::lock_guard lock(mutex_);
        const auto isPersistedKeyboard = [this](const std::wstring& id) {
            return std::any_of(
                hardwareConfigurations_.cbegin(),
                hardwareConfigurations_.cend(),
                [&](const SeatHardwareConfiguration& configuration) {
                    return !id.empty() && configuration.keyboardId == id;
                });
        };
        const auto isPersistedMouse = [this](const std::wstring& id) {
            return std::any_of(
                hardwareConfigurations_.cbegin(),
                hardwareConfigurations_.cend(),
                [&](const SeatHardwareConfiguration& configuration) {
                    return !id.empty() && configuration.mouseId == id;
                });
        };

        // Activity confirmation prevents composite HID receivers from exposing
        // phantom roles. A previously persisted assignment is already explicit
        // user intent, so do not make it disappear after every host restart.
        std::erase_if(
            keyboards,
            [this, &isPersistedKeyboard](const DeviceInfo& device) {
                return device.requiresActivityConfirmation &&
                       !confirmedKeyboardIds_.contains(device.id) &&
                       !isPersistedKeyboard(device.id);
            });
        std::erase_if(
            mice,
            [this, &isPersistedMouse](const DeviceInfo& device) {
                return device.requiresActivityConfirmation &&
                       !confirmedMouseIds_.contains(device.id) &&
                       !isPersistedMouse(device.id);
            });
    }

    std::vector<DeviceInfo> result;
    const auto append = [&result](std::vector<DeviceInfo> devices) {
        result.insert(
            result.end(),
            std::make_move_iterator(devices.begin()),
            std::make_move_iterator(devices.end()));
    };

    append(std::move(displays));
    append(std::move(keyboards));
    append(std::move(mice));
    append(std::move(controllers));
    return result;
}

void RuntimeHost::observePhysicalInput(
    DeviceType type,
    const std::wstring& devicePath) {
#if defined(_WIN32)
    if (devicePath.empty() ||
        (type != DeviceType::Keyboard && type != DeviceType::Mouse)) {
        return;
    }

    const auto category = type == DeviceType::Keyboard
        ? std::wstring_view(L"keyboard")
        : std::wstring_view(L"mouse");
    const auto stableId =
        win32::makeStableRawInputDeviceId(category, devicePath);
    if (stableId.empty()) return;

    std::lock_guard lock(mutex_);
    auto& confirmed = type == DeviceType::Keyboard
        ? confirmedKeyboardIds_
        : confirmedMouseIds_;
    const bool changed = confirmed.insert(stableId).second;
    noteMutationLocked(changed);
#else
    (void)type;
    (void)devicePath;
#endif
}

std::optional<SeatHardwareConfiguration>
RuntimeHost::seatHardwareConfiguration(std::uint32_t seatId) const {
    if (seatId == 0 || seatId > hardwareConfigurations_.size()) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    return hardwareConfigurations_[seatId - 1u];
}

bool RuntimeHost::configureSeatHardware(
    const ActivationToken& uiLease,
    const SeatHardwareConfiguration& configuration,
    std::string* error) {
    const auto fail = [error](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };

    if (uiLease.leaseClass != LeaseClass::UiConfiguration ||
        configuration.seatId != uiLease.seatId ||
        configuration.seatId == 0 ||
        configuration.seatId > hardwareConfigurations_.size()) {
        return fail("invalid Seat hardware mutation authority");
    }

    // Hardware discovery is not authority mutation and can involve SetupAPI/HID
    // I/O. Do it before taking the host mutex so polling remains responsive.
    const auto displays = hardwareDetector_.detectDisplays();
    auto keyboards = hardwareDetector_.detectKeyboards();
    auto mice = hardwareDetector_.detectMice();
    const auto controllers = hardwareDetector_.detectControllers();

    std::lock_guard lock(mutex_);
    const auto seat = controller_.snapshot(uiLease.seatId);
    if (!seat || !seat->uiLeaseActive ||
        seat->generation != uiLease.generation) {
        return fail("Seat UI lease is stale or no longer owned");
    }
    if (seat->gameLeaseActive) {
        return fail(
            "stop the running Seat game before changing its hardware assignment");
    }

    const auto currentIndex = configuration.seatId - 1u;
    const auto& currentConfiguration = hardwareConfigurations_[currentIndex];
    std::erase_if(
        keyboards,
        [this, &currentConfiguration](const DeviceInfo& device) {
            return device.requiresActivityConfirmation &&
                   !confirmedKeyboardIds_.contains(device.id) &&
                   device.id != currentConfiguration.keyboardId;
        });
    std::erase_if(
        mice,
        [this, &currentConfiguration](const DeviceInfo& device) {
            return device.requiresActivityConfirmation &&
                   !confirmedMouseIds_.contains(device.id) &&
                   device.id != currentConfiguration.mouseId;
        });

    const auto contains = [](const std::vector<DeviceInfo>& devices,
                             const std::wstring& stableId) {
        if (stableId.empty()) return true;
        for (const auto& device : devices) {
            if (device.id == stableId) return true;
        }
        return false;
    };

    if (!contains(displays, configuration.displayId)) {
        return fail("selected display is no longer connected");
    }
    if (!contains(keyboards, configuration.keyboardId)) {
        return fail("selected keyboard is no longer connected");
    }
    if (!contains(mice, configuration.mouseId)) {
        return fail("selected mouse is no longer connected");
    }
    if (!contains(controllers, configuration.controllerId)) {
        return fail("selected controller is no longer connected");
    }

    const auto otherIndex = configuration.seatId == 1u ? 1u : 0u;
    const auto& other = hardwareConfigurations_[otherIndex];
    const auto conflicts = [](const std::wstring& requested,
                              const std::wstring& existing) {
        return !requested.empty() && requested == existing;
    };
    if (conflicts(configuration.displayId, other.displayId)) {
        return fail("selected display already belongs to the other Seat");
    }
    if (conflicts(configuration.keyboardId, other.keyboardId)) {
        return fail("selected keyboard already belongs to the other Seat");
    }
    if (conflicts(configuration.mouseId, other.mouseId)) {
        return fail("selected mouse already belongs to the other Seat");
    }
    if (conflicts(configuration.controllerId, other.controllerId)) {
        return fail("selected controller already belongs to the other Seat");
    }

    const auto index = configuration.seatId - 1u;
    if (hardwareConfigurations_[index] == configuration) return true;

    const bool controllerChanged =
        hardwareConfigurations_[index].controllerId != configuration.controllerId;
    const auto previous = hardwareConfigurations_;
    auto candidate = hardwareConfigurations_;
    candidate[index] = configuration;

    // Persist before publishing the new in-memory state. A failed disk write
    // must not leave the live authority ahead of the last durable config.
    if (seatHardwareStore_ &&
        !seatHardwareStore_->save(candidate, error)) {
        return false;
    }

    // Runtime XInput identity is intentionally not persisted. If the stable
    // physical controller assignment changed (including to None), discard any
    // old runtime slot binding before publishing the new configuration.
    //
    // clearController() should be guaranteed by the validated UI lease while
    // this host mutex is held. Still, fail closed if that invariant is ever
    // violated and roll durable state back to the previous configuration.
    if (controllerChanged && !controller_.clearController(uiLease)) {
        if (seatHardwareStore_) {
            std::string rollbackError;
            if (seatHardwareStore_->save(previous, &rollbackError)) {
                return fail(
                    "controller assignment was not changed because the stale runtime binding could not be cleared");
            }

            // The durable rollback itself failed. Match in-memory state to the
            // last successful durable write so later reads never claim the old
            // configuration. Launch still fails closed on a stale/mismatched
            // controller binding.
            hardwareConfigurations_ = std::move(candidate);
            noteMutationLocked(true);
            return fail(
                "controller assignment changed on disk but the stale runtime binding could not be cleared; restart hydra_host before launching");
        }

        return fail(
            "controller assignment was not changed because the stale runtime binding could not be cleared");
    }

    hardwareConfigurations_ = std::move(candidate);
    noteMutationLocked(true);
    return true;
}

std::optional<std::uint32_t> RuntimeHost::seatForProcess(
    const ProcessIdentity& process) const noexcept {
    if (!process.valid()) return std::nullopt;

    std::lock_guard lock(mutex_);
    std::optional<std::uint32_t> owner;
    for (const std::uint32_t seatId : {1u, 2u}) {
        const auto snapshot = controller_.snapshot(seatId);
        if (!snapshot || !snapshot->gameLeaseActive || !snapshot->process ||
            *snapshot->process != process) {
            continue;
        }
        if (owner) return std::nullopt;
        owner = seatId;
    }
    return owner;
}

ActivationToken RuntimeHost::acquireUiLease(std::uint32_t seatId) noexcept {
    std::lock_guard lock(mutex_);
    const auto lease =
        controller_.acquireSeatLease(seatId, LeaseClass::UiConfiguration);
    noteMutationLocked(lease.valid());
    return lease;
}

bool RuntimeHost::releaseUiLease(const ActivationToken& token) noexcept {
    if (token.leaseClass != LeaseClass::UiConfiguration) return false;
    std::lock_guard lock(mutex_);
    const bool changed = controller_.releaseSeatLease(token);
    noteMutationLocked(changed);
    return changed;
}

bool RuntimeHost::pairController(
    const ActivationToken& uiLease,
    const std::string& persistentControllerId,
    std::uint8_t runtimeXInputSlot) noexcept {
    if (uiLease.leaseClass != LeaseClass::UiConfiguration ||
        persistentControllerId.empty() ||
        runtimeXInputSlot >= controller::kXInputSlotCount) {
        return false;
    }

    controller::InventorySnapshot snapshot;
    {
        std::lock_guard inventoryLock(controllerInventoryMutex_);
        snapshot = controllerInventory_.scan();
    }
    if (!snapshot.authoritative) return false;

    std::lock_guard lock(mutex_);
    const auto seat = controller_.snapshot(uiLease.seatId);
    if (!seat || !seat->uiLeaseActive ||
        seat->generation != uiLease.generation ||
        seat->gameLeaseActive) {
        return false;
    }

    std::wstring persistentId(
        persistentControllerId.begin(), persistentControllerId.end());
    const auto configuredIndex = uiLease.seatId - 1u;
    if (configuredIndex >= hardwareConfigurations_.size() ||
        hardwareConfigurations_[configuredIndex].controllerId.empty() ||
        hardwareConfigurations_[configuredIndex].controllerId != persistentId) {
        return false;
    }

    const auto paired = controller::pairPhysicalControllerToXInput(
        uiLease.seatId, persistentId, runtimeXInputSlot, snapshot);
    if (paired.status != controller::PairingStatus::Ok || !paired.binding) {
        return false;
    }

    const bool changed =
        controller_.bindController(uiLease, *paired.binding, snapshot);
    noteMutationLocked(changed);
    return changed;
}

AudioRouteStatus RuntimeHost::routeAudio(
    const ActivationToken& uiLease,
    const ProcessIdentity& expectedProcess,
    const AudioEndpointIdentity& endpoint,
    AudioRouter& router) noexcept {
    if (uiLease.leaseClass != LeaseClass::UiConfiguration ||
        !expectedProcess.valid() || !endpoint.valid()) {
        return AudioRouteStatus::InvalidProcess;
    }

    const auto stillOwned = [&]() {
        std::lock_guard lock(mutex_);
        const auto snapshot = controller_.snapshot(uiLease.seatId);
        return snapshot && snapshot->uiLeaseActive &&
               snapshot->gameLeaseActive &&
               snapshot->generation == uiLease.generation &&
               snapshot->process &&
               *snapshot->process == expectedProcess;
    };

    if (!stillOwned()) {
        return AudioRouteStatus::InvalidProcess;
    }

    // Windows AudioPolicyConfig can block inside COM/device policy code. Keep it
    // off the authority mutex so read-only snapshots and unrelated Seat state
    // remain responsive. Audio mutations themselves stay serialized.
    std::lock_guard audioLock(audioMutationMutex_);

    // Revalidate after waiting for the audio lane; the Seat may have stopped or
    // changed while another audio request was in flight.
    if (!stillOwned()) {
        return AudioRouteStatus::InvalidProcess;
    }

    const auto status = router.assignEndpoint(expectedProcess, endpoint);
    if (status != AudioRouteStatus::Success) {
        return status;
    }

    // If the exact Seat generation/process disappeared during the OS call,
    // remove the just-written assignment for that exact process identity. The
    // router revalidates PID+creation identity, so this cannot clear a reused PID.
    if (!stillOwned()) {
        const auto rollback = router.clearAssignment(expectedProcess);
        return rollback == AudioRouteStatus::Success
            ? AudioRouteStatus::IdentityMismatch
            : AudioRouteStatus::RoutingFailed;
    }

    return AudioRouteStatus::Success;
}

AudioRouteStatus RuntimeHost::resetAudio(
    const ActivationToken& uiLease,
    const ProcessIdentity& expectedProcess,
    AudioRouter& router) noexcept {
    if (uiLease.leaseClass != LeaseClass::UiConfiguration ||
        !expectedProcess.valid()) {
        return AudioRouteStatus::InvalidProcess;
    }

    const auto stillOwned = [&]() {
        std::lock_guard lock(mutex_);
        const auto snapshot = controller_.snapshot(uiLease.seatId);
        return snapshot && snapshot->uiLeaseActive &&
               snapshot->gameLeaseActive &&
               snapshot->generation == uiLease.generation &&
               snapshot->process &&
               *snapshot->process == expectedProcess;
    };

    if (!stillOwned()) {
        return AudioRouteStatus::InvalidProcess;
    }

    std::lock_guard audioLock(audioMutationMutex_);
    if (!stillOwned()) {
        return AudioRouteStatus::InvalidProcess;
    }

    const auto status = router.clearAssignment(expectedProcess);
    if (status != AudioRouteStatus::Success) {
        return status;
    }

    // Clearing an assignment for the exact process is safe even if it exits
    // during the call, but report the authority race instead of claiming the
    // mutation belongs to a still-active Seat.
    return stillOwned()
        ? AudioRouteStatus::Success
        : AudioRouteStatus::IdentityMismatch;
}

ActivationToken RuntimeHost::beginSeatActivation(std::uint32_t seatId) noexcept {
    std::lock_guard lock(mutex_);
    const auto activation = controller_.beginSeatActivation(seatId);
    noteMutationLocked(activation.valid());
    return activation;
}

bool RuntimeHost::publishProcess(const ActivationToken& activation,
                                 const ProcessIdentity& process) noexcept {
    std::lock_guard lock(mutex_);
    const bool changed = controller_.publishProcess(activation, process);
    noteMutationLocked(changed);
    return changed;
}

bool RuntimeHost::bindTargetWindow(const ActivationToken& activation,
                                   const ProcessIdentity& owner,
                                   std::uintptr_t hwnd) noexcept {
    std::lock_guard lock(mutex_);
    const bool changed = controller_.bindTargetWindow(activation, owner, hwnd);
    noteMutationLocked(changed);
    return changed;
}

bool RuntimeHost::clearTargetWindow(
    const ActivationToken& activation,
    const ProcessIdentity& owner,
    std::uintptr_t expectedHwnd) noexcept {
    std::lock_guard lock(mutex_);
    const bool changed =
        controller_.clearTargetWindow(activation, owner, expectedHwnd);
    noteMutationLocked(changed);
    return changed;
}

bool RuntimeHost::bindController(
    const ActivationToken& activation,
    const controller::SeatBinding& binding,
    const controller::InventorySnapshot& inventory) noexcept {
    std::lock_guard lock(mutex_);
    const bool changed = controller_.bindController(activation, binding, inventory);
    noteMutationLocked(changed);
    return changed;
}

controller::PollResult RuntimeHost::pollController(
    const ActivationToken& activation,
    const controller::InventorySnapshot& inventory) noexcept {
    std::lock_guard lock(mutex_);
    return controller_.pollController(activation, inventory);
}

controller::IoStatus RuntimeHost::setControllerVibration(
    const ActivationToken& activation,
    const controller::InventorySnapshot& inventory,
    std::uint16_t lowFrequencyMotor,
    std::uint16_t highFrequencyMotor) noexcept {
    std::lock_guard lock(mutex_);
    return controller_.setControllerVibration(
        activation, inventory, lowFrequencyMotor, highFrequencyMotor);
}

std::optional<controller::VirtualXInputMapping>
RuntimeHost::virtualXInputMapping(const ActivationToken& activation) const noexcept {
    std::lock_guard lock(mutex_);
    return controller_.virtualXInputMapping(activation);
}

bool RuntimeHost::endSeatActivation(const ActivationToken& activation) noexcept {
    std::lock_guard lock(mutex_);
    const bool changed = controller_.endSeatActivation(activation);
    noteMutationLocked(changed);
    return changed;
}

void RuntimeHost::noteMutationLocked(bool changed) noexcept {
    if (changed && authorityRevision_ != UINT64_MAX) {
        ++authorityRevision_;
    }
}

} // namespace hydra::runtime
