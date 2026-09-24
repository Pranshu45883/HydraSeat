#include "hydra/plan_preflight.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hydra::preflight {
namespace {

std::string userMessageFor(plan::PlanIssueCode code, SeatId seatId) {
    const std::string seat = seatId == 0u ? std::string{} : "Seat " + std::to_string(seatId) + ": ";
    switch (code) {
    case plan::PlanIssueCode::MissingDisplay:
        return seat + "a display is required for this game.";
    case plan::PlanIssueCode::MissingKeyboard:
        return seat + "a keyboard is required for this game.";
    case plan::PlanIssueCode::MissingMouse:
        return seat + "a pointing device is required for this game.";
    case plan::PlanIssueCode::MissingController:
        return seat + "a controller is required for this game.";
    case plan::PlanIssueCode::MissingAudioOutput:
        return seat + "an audio output is required for this game.";
    case plan::PlanIssueCode::MissingTwoPlayerSetup:
        return "This same-game session needs a two-player setup before Play.";
    case plan::PlanIssueCode::InvalidTwoPlayerSetup:
        return "The selected two-player setup needs review.";
    case plan::PlanIssueCode::HighRiskApprovalRequired:
        return seat + "this compatibility path is Protected / Experimental and needs approval.";
    case plan::PlanIssueCode::ProviderUnavailable:
    case plan::PlanIssueCode::MissingProvider:
        return seat + "the selected game provider is unavailable.";
    case plan::PlanIssueCode::ProviderLaunchRejected:
        return seat + "the provider cannot build the selected launch safely.";
    case plan::PlanIssueCode::AmbiguousAccountReference:
        return seat + "choose which provider account reference this Player should use.";
    case plan::PlanIssueCode::MissingCapability:
        return seat + "the current setup cannot provide a required runtime capability.";
    case plan::PlanIssueCode::DuplicateExclusiveHardware:
        return "A device is assigned exclusively to more than one selected Seat.";
    case plan::PlanIssueCode::StaleCompatibility:
        return seat + "game compatibility evidence changed; review the setup before Play.";
    case plan::PlanIssueCode::MissingRequirement:
    case plan::PlanIssueCode::DuplicateRequirement:
        return seat + "game requirements need review before Play.";
    case plan::PlanIssueCode::ActiveSeatCount:
        return "Choose one or two active Seats.";
    case plan::PlanIssueCode::InactiveSeat:
    case plan::PlanIssueCode::MissingSeat:
        return seat + "the selected Seat is not available.";
    case plan::PlanIssueCode::MissingPlayer:
        return seat + "the selected Player is not available.";
    case plan::PlanIssueCode::MissingGame:
        return seat + "the selected Game is not available.";
    case plan::PlanIssueCode::InvalidSeatDocument:
    case plan::PlanIssueCode::InvalidPlayerDocument:
    case plan::PlanIssueCode::InvalidGameDocument:
    case plan::PlanIssueCode::InvalidSetupDocument:
    case plan::PlanIssueCode::InvalidRuntimeSelection:
    case plan::PlanIssueCode::DuplicateProvider:
        return "The selected session data needs review before Play.";
    }
    return "The selected session cannot start safely.";
}

std::string mutationMessage(MutationKind kind, SeatId seatId, bool approved) {
    const std::string seat = seatId == 0u ? std::string{} : "Seat " + std::to_string(seatId) + ": ";
    const std::string action = approved ? "will " : "needs approval to ";
    switch (kind) {
    case MutationKind::CreateDirectory: return seat + action + "create an isolated data directory.";
    case MutationKind::WriteConfig: return seat + action + "write an approved game configuration.";
    case MutationKind::DeviceRoute: return seat + action + "apply the planned input-device route.";
    case MutationKind::ControllerRoute: return seat + action + "apply the planned controller route.";
    case MutationKind::AudioRoute: return seat + action + "apply the planned audio route.";
    case MutationKind::DisplayPlacement: return seat + action + "place the game on its assigned display.";
    case MutationKind::OtherApproved: return seat + action + "apply an explicitly declared setup change.";
    }
    return seat + "has a setup change to review.";
}

std::string boundedDetail(std::string value) {
    if (value.size() > provider::kMaximumProviderDiagnosticBytes) {
        value.resize(provider::kMaximumProviderDiagnosticBytes);
    }
    return value;
}

} // namespace

Summary buildSummary(const plan::PlanCompileResult& result,
                     std::span<const PlannedMutation> mutations) {
    Summary summary;
    if (result.plan) {
        summary.planFingerprint = result.plan->fingerprint;
        summary.canActivate = true;
        for (const auto& seat : result.plan->seats) {
            if (seat.requirements.controller) {
                summary.messages.push_back({Severity::Info, seat.seatId, "requires.controller",
                    "Seat " + std::to_string(seat.seatId) + " needs a controller for this game.",
                    "controller requirement is pinned in plan " +
                        std::to_string(result.plan->fingerprint)});
            }
            if (seat.requirements.audioOutput) {
                summary.messages.push_back({Severity::Info, seat.seatId, "requires.audio",
                    "Seat " + std::to_string(seat.seatId) + " uses its assigned audio output.",
                    "audio output is required by the selected runtime requirement revision"});
            }
            if (seat.setupId) {
                summary.messages.push_back({Severity::Info, seat.seatId, "setup.two-player",
                    "A reviewed two-player setup will be used for this Seat.",
                    "setup id is pinned in the immutable plan; instance index=" +
                        std::to_string(seat.instanceIndex)});
            }
            if (seat.requirements.highRisk) {
                summary.messages.push_back({Severity::Warning, seat.seatId, "risk.protected",
                    "This title/setup is Protected / Experimental.",
                    "high-risk compatibility path was explicitly approved before plan compilation"});
            }
        }
    } else {
        summary.canActivate = false;
        for (const auto& issue : result.issues) {
            summary.messages.push_back({
                Severity::Blocking,
                issue.seatId,
                std::string("plan.") + std::string(plan::planIssueCodeName(issue.code)),
                userMessageFor(issue.code, issue.seatId),
                boundedDetail(std::string(plan::planIssueCodeName(issue.code)) + ": " +
                              issue.detail),
            });
        }
        if (result.issues.empty()) {
            summary.messages.push_back({Severity::Blocking, 0u, "plan.missing",
                "No safe launch plan is available.",
                "plan compiler returned neither a plan nor a diagnostic"});
        }
    }

    if (mutations.size() > kMaximumPreflightMutations) {
        summary.canActivate = false;
        summary.messages.push_back({Severity::Blocking, 0u, "mutation.bounds",
            "Too many setup changes are queued; review the setup.",
            "typed mutation preview exceeds the bounded maximum"});
    } else {
        std::set<std::string> ids;
        std::vector<PlannedMutation> ordered(mutations.begin(), mutations.end());
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            if (left.seatId != right.seatId) return left.seatId < right.seatId;
            if (left.kind != right.kind) {
                return static_cast<std::uint8_t>(left.kind) <
                       static_cast<std::uint8_t>(right.kind);
            }
            return left.mutationId < right.mutationId;
        });
        for (const auto& mutation : ordered) {
            const bool validId = !mutation.mutationId.empty() &&
                                 mutation.mutationId.size() <= profile::kMaximumIdentifierBytes;
            const bool unique = validId && ids.insert(mutation.mutationId).second;
            if (!unique) {
                summary.canActivate = false;
                summary.messages.push_back({Severity::Blocking, mutation.seatId,
                    "mutation.invalid",
                    "A planned setup change has invalid or duplicate identity.",
                    "mutation identifiers must be unique bounded opaque IDs"});
                continue;
            }
            const bool approved = !mutation.requiresApproval || mutation.approved;
            if (!approved) summary.canActivate = false;
            summary.messages.push_back({
                approved ? Severity::Warning : Severity::Blocking,
                mutation.seatId,
                std::string("mutation.") + std::string(mutationKindName(mutation.kind)),
                mutationMessage(mutation.kind, mutation.seatId, approved),
                "mutation " + mutation.mutationId + "; kind=" +
                    std::string(mutationKindName(mutation.kind)) +
                    "; approval=" + (approved ? "present" : "required"),
            });
        }
    }

    std::stable_sort(summary.messages.begin(), summary.messages.end(),
                     [](const auto& left, const auto& right) {
        if (left.severity != right.severity) {
            return static_cast<std::uint8_t>(left.severity) >
                   static_cast<std::uint8_t>(right.severity);
        }
        if (left.seatId != right.seatId) return left.seatId < right.seatId;
        if (left.code != right.code) return left.code < right.code;
        return left.expertDetail < right.expertDetail;
    });
    return summary;
}

std::string_view severityName(Severity severity) noexcept {
    switch (severity) {
    case Severity::Info: return "Info";
    case Severity::Warning: return "Warning";
    case Severity::Blocking: return "Blocking";
    }
    return "Unknown";
}

std::string_view mutationKindName(MutationKind kind) noexcept {
    switch (kind) {
    case MutationKind::CreateDirectory: return "CreateDirectory";
    case MutationKind::WriteConfig: return "WriteConfig";
    case MutationKind::DeviceRoute: return "DeviceRoute";
    case MutationKind::ControllerRoute: return "ControllerRoute";
    case MutationKind::AudioRoute: return "AudioRoute";
    case MutationKind::DisplayPlacement: return "DisplayPlacement";
    case MutationKind::OtherApproved: return "OtherApproved";
    }
    return "Unknown";
}

} // namespace hydra::preflight
