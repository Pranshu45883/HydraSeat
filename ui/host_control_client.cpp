#include "ui/host_control_client.hpp"
#include "ui/host_bootstrap.hpp"

#include <utility>

namespace hydra::ui {
namespace {

void setError(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

} // namespace

HostControlClient::~HostControlClient() {
    close();
}

void HostControlClient::clearOwnedUiLeases() noexcept {
    for (auto& owned : ownedUiLeases_) {
        owned.store(false, std::memory_order_release);
    }
}

void HostControlClient::clearOwnedUiLeasesIfDisconnected() noexcept {
    if (!client_.connected()) {
        clearOwnedUiLeases();
    }
}

bool HostControlClient::ensureConnected(std::string* error) {
    std::lock_guard lock(mutex_);
    if (client_.connected()) return true;

    clearOwnedUiLeases();
    return connectCanonicalHost(
        client_, hostipc::ClientRole::Control, error);
}

bool HostControlClient::connected() const noexcept {
    std::lock_guard lock(mutex_);
    return client_.connected();
}

void HostControlClient::close() noexcept {
    std::lock_guard lock(mutex_);
    client_.close();
    clearOwnedUiLeases();
}

bool HostControlClient::ownsUiLease(std::uint32_t seatId) const noexcept {
    if (seatId == 0 || seatId > ownedUiLeases_.size()) return false;
    return ownedUiLeases_[seatId - 1u].load(std::memory_order_acquire);
}

std::optional<hostipc::HostSnapshot> HostControlClient::snapshot(
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (!ensureConnected(error)) return std::nullopt;
    auto result =
        client_.getSnapshot(hostipc::kDefaultHostPipeTimeoutMs, error);
    clearOwnedUiLeasesIfDisconnected();
    return result;
}

std::optional<hostipc::HostSnapshot> HostControlClient::acquireUiLease(
    std::uint32_t seatId,
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (seatId == 0 || seatId > ownedUiLeases_.size()) {
        setError(error, "invalid Seat id");
        return std::nullopt;
    }
    if (!ensureConnected(error)) return std::nullopt;
    if (ownedUiLeases_[seatId - 1u].load(std::memory_order_acquire)) {
        return client_.getSnapshot(hostipc::kDefaultHostPipeTimeoutMs, error);
    }

    auto result = client_.acquireUiLease(
        seatId, hostipc::kDefaultHostPipeTimeoutMs, error);
    clearOwnedUiLeasesIfDisconnected();
    if (result) {
        ownedUiLeases_[seatId - 1u].store(true, std::memory_order_release);
    }
    return result;
}

std::optional<hostipc::HostSnapshot> HostControlClient::releaseUiLease(
    std::uint32_t seatId,
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (seatId == 0 || seatId > ownedUiLeases_.size()) {
        setError(error, "invalid Seat id");
        return std::nullopt;
    }
    if (!ensureConnected(error)) return std::nullopt;
    if (!ownedUiLeases_[seatId - 1u].load(std::memory_order_acquire)) {
        setError(error, "this UI connection does not own the Seat UI lease");
        return std::nullopt;
    }

    auto result = client_.releaseUiLease(
        seatId, hostipc::kDefaultHostPipeTimeoutMs, error);
    clearOwnedUiLeasesIfDisconnected();
    if (result) {
        ownedUiLeases_[seatId - 1u].store(false, std::memory_order_release);
    }
    return result;
}

bool HostControlClient::ensureUiLeaseForSeat(
    std::uint32_t seatId,
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (seatId == 0 || seatId > ownedUiLeases_.size()) {
        setError(error, "invalid Seat id");
        return false;
    }
    if (!ensureConnected(error)) return false;
    if (ownsUiLease(seatId)) return true;

    const auto current = client_.getSnapshot(
        hostipc::kDefaultHostPipeTimeoutMs, error);
    clearOwnedUiLeasesIfDisconnected();
    if (!current || seatId > current->seats.size()) return false;

    const auto& seat = current->seats[seatId - 1u];
    if (seat.uiLeaseActive) {
        setError(error, "Seat UI lease is owned by another control connection");
        return false;
    }
    return acquireUiLease(seatId, error).has_value();
}

std::optional<hostipc::SeatHardwareAssignment> HostControlClient::seatHardware(
    std::uint32_t seatId,
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (seatId == 0 || seatId > ownedUiLeases_.size()) {
        setError(error, "invalid Seat id");
        return std::nullopt;
    }
    if (!ensureConnected(error)) return std::nullopt;
    auto result = client_.getSeatHardware(
        seatId, hostipc::kDefaultHostPipeTimeoutMs, error);
    clearOwnedUiLeasesIfDisconnected();
    return result;
}

std::optional<hostipc::SeatHardwareAssignment>
HostControlClient::assignSeatHardware(
    std::uint32_t seatId,
    const std::string& displayIdUtf8,
    const std::string& keyboardIdUtf8,
    const std::string& mouseIdUtf8,
    const std::string& controllerIdUtf8,
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (!ensureUiLeaseForSeat(seatId, error)) return std::nullopt;

    hostipc::SeatHardwareAssignment assignment;
    assignment.seatId = seatId;
    assignment.displayIdUtf8 = displayIdUtf8;
    assignment.keyboardIdUtf8 = keyboardIdUtf8;
    assignment.mouseIdUtf8 = mouseIdUtf8;
    assignment.controllerIdUtf8 = controllerIdUtf8;
    auto result = client_.assignSeatHardware(
        assignment, hostipc::kDefaultHostPipeTimeoutMs, error);
    clearOwnedUiLeasesIfDisconnected();
    return result;
}

std::optional<hostipc::HostSnapshot> HostControlClient::pairController(
    std::uint32_t seatId,
    const std::string& persistentControllerId,
    std::uint8_t runtimeXInputSlot,
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (!ensureUiLeaseForSeat(seatId, error)) return std::nullopt;

    auto result = client_.pairController(
        seatId,
        persistentControllerId,
        runtimeXInputSlot,
        hostipc::kDefaultHostPipeTimeoutMs,
        error);
    clearOwnedUiLeasesIfDisconnected();
    return result;
}

std::optional<std::uint32_t> HostControlClient::seatForProcess(
    const hostipc::HostSnapshot& snapshot,
    std::uint32_t processId,
    std::uint64_t creationIdentity) const noexcept {
    std::lock_guard lock(mutex_);
    for (const auto& seat : snapshot.seats) {
        if (seat.processOwned &&
            seat.processId == processId &&
            seat.processCreationIdentity == creationIdentity) {
            return seat.seatId;
        }
    }
    return std::nullopt;
}

bool HostControlClient::ensureUiLeaseForProcess(
    std::uint32_t processId,
    std::uint64_t creationIdentity,
    std::string* error) {
    std::lock_guard lock(mutex_);
    const auto current = snapshot(error);
    if (!current) return false;

    const auto seatId = seatForProcess(*current, processId, creationIdentity);
    if (!seatId) {
        setError(error, "process is not owned by any active Seat");
        return false;
    }
    if (ownsUiLease(*seatId)) return true;

    const auto& seat = current->seats[*seatId - 1u];
    if (seat.uiLeaseActive) {
        setError(error, "Seat UI lease is owned by another control connection");
        return false;
    }
    return acquireUiLease(*seatId, error).has_value();
}

std::optional<hostipc::AudioMutationStatus> HostControlClient::routeAudio(
    std::uint32_t processId,
    std::uint64_t creationIdentity,
    const std::string& endpointId,
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (!ensureUiLeaseForProcess(processId, creationIdentity, error)) {
        return std::nullopt;
    }
    auto result = client_.routeAudio(
        processId,
        creationIdentity,
        endpointId,
        hostipc::kDefaultHostPipeTimeoutMs,
        error);
    clearOwnedUiLeasesIfDisconnected();
    return result;
}

std::optional<hostipc::AudioMutationStatus> HostControlClient::resetAudio(
    std::uint32_t processId,
    std::uint64_t creationIdentity,
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (!ensureUiLeaseForProcess(processId, creationIdentity, error)) {
        return std::nullopt;
    }
    auto result = client_.resetAudio(
        processId,
        creationIdentity,
        hostipc::kDefaultHostPipeTimeoutMs,
        error);
    clearOwnedUiLeasesIfDisconnected();
    return result;
}

std::optional<hostipc::HostSnapshot> HostControlClient::launchGame(
    std::uint32_t seatId,
    const std::string& titleUtf8,
    const std::string& executablePathUtf8,
    const std::string& launchArgumentsUtf8,
    const std::string& workingDirectoryUtf8,
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (!ensureUiLeaseForSeat(seatId, error)) return std::nullopt;

    hostipc::LaunchGameRequest request;
    request.seatId = seatId;
    request.titleUtf8 = titleUtf8;
    request.executablePathUtf8 = executablePathUtf8;
    request.launchArgumentsUtf8 = launchArgumentsUtf8;
    request.workingDirectoryUtf8 = workingDirectoryUtf8;
    auto result = client_.launchGame(
        request,
        hostipc::kHostLaunchTimeoutMs,
        error);
    clearOwnedUiLeasesIfDisconnected();
    return result;
}

std::optional<hostipc::HostSnapshot> HostControlClient::stopGame(
    std::uint32_t seatId,
    std::string* error) {
    std::lock_guard lock(mutex_);
    if (!ensureUiLeaseForSeat(seatId, error)) return std::nullopt;
    auto result = client_.stopGame(
        seatId,
        hostipc::kHostStopTimeoutMs,
        error);
    clearOwnedUiLeasesIfDisconnected();
    return result;
}

} // namespace hydra::ui
