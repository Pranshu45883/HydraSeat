#include "ui/host_control_client.hpp"

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

bool HostControlClient::ensureConnected(std::string* error) {
    if (client_.connected()) return true;

    ownedUiLeases_.fill(false);
    return client_.connect(
        hostipc::ClientRole::Control,
        hostipc::kDefaultHostPipeTimeoutMs,
        error);
}

bool HostControlClient::connected() const noexcept {
    return client_.connected();
}

void HostControlClient::close() noexcept {
    client_.close();
    ownedUiLeases_.fill(false);
}

bool HostControlClient::ownsUiLease(std::uint32_t seatId) const noexcept {
    if (seatId == 0 || seatId > ownedUiLeases_.size()) return false;
    return ownedUiLeases_[seatId - 1u];
}

std::optional<hostipc::HostSnapshot> HostControlClient::snapshot(
    std::string* error) {
    if (!ensureConnected(error)) return std::nullopt;
    return client_.getSnapshot(hostipc::kDefaultHostPipeTimeoutMs, error);
}

std::optional<hostipc::HostSnapshot> HostControlClient::acquireUiLease(
    std::uint32_t seatId,
    std::string* error) {
    if (seatId == 0 || seatId > ownedUiLeases_.size()) {
        setError(error, "invalid Seat id");
        return std::nullopt;
    }
    if (!ensureConnected(error)) return std::nullopt;
    if (ownedUiLeases_[seatId - 1u]) {
        return client_.getSnapshot(hostipc::kDefaultHostPipeTimeoutMs, error);
    }

    auto result = client_.acquireUiLease(
        seatId, hostipc::kDefaultHostPipeTimeoutMs, error);
    if (result) ownedUiLeases_[seatId - 1u] = true;
    return result;
}

std::optional<hostipc::HostSnapshot> HostControlClient::releaseUiLease(
    std::uint32_t seatId,
    std::string* error) {
    if (seatId == 0 || seatId > ownedUiLeases_.size()) {
        setError(error, "invalid Seat id");
        return std::nullopt;
    }
    if (!ensureConnected(error)) return std::nullopt;
    if (!ownedUiLeases_[seatId - 1u]) {
        setError(error, "this UI connection does not own the Seat UI lease");
        return std::nullopt;
    }

    auto result = client_.releaseUiLease(
        seatId, hostipc::kDefaultHostPipeTimeoutMs, error);
    if (result) ownedUiLeases_[seatId - 1u] = false;
    return result;
}

std::optional<hostipc::HostSnapshot> HostControlClient::pairController(
    std::uint32_t seatId,
    const std::string& persistentControllerId,
    std::uint8_t runtimeXInputSlot,
    std::string* error) {
    if (!ensureConnected(error)) return std::nullopt;

    if (!ownsUiLease(seatId)) {
        const auto current = client_.getSnapshot(
            hostipc::kDefaultHostPipeTimeoutMs, error);
        if (!current || seatId == 0 || seatId > current->seats.size()) {
            return std::nullopt;
        }
        const auto& seat = current->seats[seatId - 1u];
        if (seat.uiLeaseActive) {
            setError(error, "Seat UI lease is owned by another control connection");
            return std::nullopt;
        }
        if (!acquireUiLease(seatId, error)) return std::nullopt;
    }

    return client_.pairController(
        seatId,
        persistentControllerId,
        runtimeXInputSlot,
        hostipc::kDefaultHostPipeTimeoutMs,
        error);
}

std::optional<std::uint32_t> HostControlClient::seatForProcess(
    const hostipc::HostSnapshot& snapshot,
    std::uint32_t processId,
    std::uint64_t creationIdentity) const noexcept {
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
    if (!ensureUiLeaseForProcess(processId, creationIdentity, error)) {
        return std::nullopt;
    }
    return client_.routeAudio(
        processId,
        creationIdentity,
        endpointId,
        hostipc::kDefaultHostPipeTimeoutMs,
        error);
}

std::optional<hostipc::AudioMutationStatus> HostControlClient::resetAudio(
    std::uint32_t processId,
    std::uint64_t creationIdentity,
    std::string* error) {
    if (!ensureUiLeaseForProcess(processId, creationIdentity, error)) {
        return std::nullopt;
    }
    return client_.resetAudio(
        processId,
        creationIdentity,
        hostipc::kDefaultHostPipeTimeoutMs,
        error);
}

} // namespace hydra::ui
