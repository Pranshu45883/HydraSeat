#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hydra::controller {

inline constexpr std::uint8_t kXInputSlotCount = 4;

enum class ApiSurface : std::uint8_t {
    XInput = 1,
    DirectInput = 2,
    GameInput = 3,
};

enum class IdentityQuality : std::uint8_t {
    RuntimeOnly = 0,
    Stable = 1,
};

struct SourceDescriptor {
    std::string runtimeKey;
    std::optional<std::wstring> persistentId;
    std::wstring displayName;
    ApiSurface api{ApiSurface::XInput};
    IdentityQuality identityQuality{IdentityQuality::RuntimeOnly};
    std::optional<std::uint8_t> runtimeXInputSlot;
    bool connected{false};

    bool operator==(const SourceDescriptor&) const = default;
};

struct SeatBindingRequest {
    std::uint32_t seatId{0};
    ApiSurface api{ApiSurface::XInput};
    std::optional<std::wstring> persistentControllerId;
    std::optional<std::uint8_t> runtimeXInputSlot;

    bool operator==(const SeatBindingRequest&) const = default;
};

struct SeatBinding {
    std::uint32_t seatId{0};
    ApiSurface api{ApiSurface::XInput};
    std::string runtimeKey;
    std::optional<std::wstring> persistentControllerId;
    std::optional<std::uint8_t> runtimeXInputSlot;

    bool operator==(const SeatBinding&) const = default;
};

enum class BindingIssueCode : std::uint8_t {
    InvalidSeat = 0,
    DuplicateSeat = 1,
    V1SeatLimitExceeded = 2,
    MissingPersistentIdentity = 3,
    SourceNotFound = 4,
    SourceDisconnected = 5,
    SourceApiMismatch = 6,
    SourceAlreadyAssigned = 7,
    RuntimeSlotOutOfRange = 8,
    AmbiguousSource = 9,
};

struct BindingIssue {
    BindingIssueCode code{BindingIssueCode::InvalidSeat};
    std::uint32_t seatId{0};
    std::wstring controllerId;

    bool operator==(const BindingIssue&) const = default;
};

struct BindingPlan {
    bool valid{true};
    std::vector<SeatBinding> bindings;
    std::vector<BindingIssue> issues;
};

// Stable physical IDs and runtime-only XInput slots are intentionally distinct.
// XInput user indices are accepted only as explicit current-session hints.
BindingPlan planSeatBindings(std::span<const SeatBindingRequest> requests,
                             std::span<const SourceDescriptor> sources);

} // namespace hydra::controller
