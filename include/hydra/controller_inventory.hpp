#pragma once

#include "hydra/controller_identity.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hydra::controller {

struct PhysicalControllerDescriptor {
    std::wstring persistentId;
    std::wstring displayName;
    std::wstring devicePath;
    std::uint16_t vendorId{0};
    std::uint16_t productId{0};

    bool operator==(const PhysicalControllerDescriptor&) const = default;
};

struct InventorySnapshot {
    bool authoritative{false};
    std::vector<SourceDescriptor> sources;
    std::vector<PhysicalControllerDescriptor> physicalControllers;
    std::string error;
};

enum class PairingStatus : std::uint8_t {
    Ok = 0,
    InvalidSeat = 1,
    InvalidPersistentId = 2,
    PhysicalControllerNotFound = 3,
    AmbiguousPhysicalController = 4,
    RuntimeSlotOutOfRange = 5,
    RuntimeSourceNotFound = 6,
    RuntimeSourceDisconnected = 7,
    PairingGestureNotDetected = 8,
    PairingGestureAmbiguous = 9,
    InventoryNotAuthoritative = 10,
};

struct PairingResult {
    PairingStatus status{PairingStatus::InvalidPersistentId};
    std::optional<SeatBinding> binding;
};

class ControllerInventory final {
public:
    InventorySnapshot scan() noexcept;

private:
    std::array<bool, kXInputSlotCount> seenSlots_{};
    std::array<bool, kXInputSlotCount> previousConnected_{};
    std::array<std::uint64_t, kXInputSlotCount> generations_{};
};

// Stateless convenience scan. Runtime code that needs reconnect generations
// should keep one ControllerInventory instance alive and call scan().
InventorySnapshot scanControllerSources() noexcept;

// Explicitly pairs one selected stable physical identity with one current XInput
// slot. No physical-to-XInput inference is performed.
PairingResult pairPhysicalControllerToXInput(
    std::uint32_t seatId,
    const std::wstring& persistentControllerId,
    std::uint8_t runtimeSlot,
    const InventorySnapshot& inventory) noexcept;

bool bindingMatchesInventory(const SeatBinding& binding,
                             const InventorySnapshot& inventory) noexcept;

} // namespace hydra::controller
