#pragma once

#include "hydra/provider_launch_plan.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::preflight {

inline constexpr std::size_t kMaximumPreflightMutations = 256u;

enum class Severity : std::uint8_t {
    Info = 0,
    Warning = 1,
    Blocking = 2,
};

enum class MutationKind : std::uint8_t {
    CreateDirectory = 0,
    WriteConfig = 1,
    DeviceRoute = 2,
    ControllerRoute = 3,
    AudioRoute = 4,
    DisplayPlacement = 5,
    OtherApproved = 6,
};

struct PlannedMutation {
    std::string mutationId;
    SeatId seatId{0};
    MutationKind kind{MutationKind::OtherApproved};
    bool requiresApproval{false};
    bool approved{false};

    bool operator==(const PlannedMutation&) const = default;
};

struct Message {
    Severity severity{Severity::Info};
    SeatId seatId{0};
    std::string code;
    std::string userMessage;
    std::string expertDetail;

    bool operator==(const Message&) const = default;
};

struct Summary {
    bool canActivate{false};
    std::uint64_t planFingerprint{0};
    std::vector<Message> messages;

    bool operator==(const Summary&) const = default;
};

// Produces deterministic user-facing and expert diagnostics from the plan
// compiler result plus typed mutation kinds. Mutation payload values are not
// representable here, so credentials/tokens/absolute private payload data cannot
// leak through this preview surface.
Summary buildSummary(const plan::PlanCompileResult& result,
                     std::span<const PlannedMutation> mutations = {});

std::string_view severityName(Severity severity) noexcept;
std::string_view mutationKindName(MutationKind kind) noexcept;

} // namespace hydra::preflight
