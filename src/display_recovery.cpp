#include "hydra/display_recovery.hpp"

#include <algorithm>
#include <limits>

namespace hydra::display {
namespace {

const DisplayOutput* findActive(const DisplayTopologySnapshot& topology,
                                const std::string& outputId) {
    const auto found = std::find_if(topology.outputs.begin(), topology.outputs.end(),
                                    [&](const DisplayOutput& output) {
                                        return output.active &&
                                               output.identity.stableKey() == outputId;
                                    });
    return found == topology.outputs.end() ? nullptr : &*found;
}

bool sameResolvedLayout(const SeatDisplayGroup& left,
                        const SeatDisplayGroup& right) {
    if (left.primaryOutputId != right.primaryOutputId ||
        left.outputs.size() != right.outputs.size()) {
        return false;
    }
    for (const auto& output : left.outputs) {
        const auto match = std::find_if(right.outputs.begin(), right.outputs.end(),
                                        [&](const SeatDisplayOutput& candidate) {
                                            return candidate.outputId == output.outputId;
                                        });
        if (match == right.outputs.end() ||
            match->globalBounds != output.globalBounds ||
            match->dpiX != output.dpiX || match->dpiY != output.dpiY ||
            match->orientation != output.orientation) {
            return false;
        }
    }
    return true;
}

} // namespace

SeatDisplayRecoveryDecision planSeatDisplayRecovery(
    const DisplayTopologySnapshot& previousTopology,
    const DisplayTopologySnapshot& currentTopology,
    const SeatDisplayRecoveryProfile& profile,
    const SeatDisplayGroup* previousResolvedGroup) {
    SeatDisplayRecoveryDecision result;
    result.seatId = profile.request.seatId;
    result.topologyChanged = previousTopology.generation != currentTopology.generation;

    if (profile.request.seatId == 0 || !currentTopology.querySucceeded) {
        result.diagnostics.push_back("display recovery requires a valid Seat and successful topology query");
        return result;
    }

    bool primaryMissing = false;
    for (const auto& selection : profile.request.outputs) {
        if (findActive(currentTopology, selection.outputId) != nullptr) continue;
        if (selection.required) {
            result.missingRequiredOutputs.push_back(selection.outputId);
        } else {
            result.missingOptionalOutputs.push_back(selection.outputId);
        }
        if (selection.outputId == profile.request.primaryOutputId) primaryMissing = true;
    }

    if (primaryMissing) {
        result.degraded = true;
        result.disposition = profile.primaryLossPolicy == RequiredPrimaryLossPolicy::PauseSession
            ? DisplayRecoveryDisposition::PauseRequired
            : DisplayRecoveryDisposition::StopRequired;
        result.diagnostics.push_back(
            "required Seat primary output is missing; cross-Seat fallback is forbidden");
        return result;
    }

    SeatDisplayRequest recoveryRequest = profile.request;
    if (!result.missingRequiredOutputs.empty()) {
        recoveryRequest.missingOutputPolicy = MissingOutputPolicy::Degrade;
    }
    const auto validation = buildSeatDisplayLayouts(currentTopology, {recoveryRequest});
    if (!validation.valid || validation.groups.empty()) {
        result.disposition = DisplayRecoveryDisposition::Invalid;
        result.diagnostics.insert(result.diagnostics.end(), validation.errors.begin(), validation.errors.end());
        result.diagnostics.insert(result.diagnostics.end(), validation.warnings.begin(), validation.warnings.end());
        return result;
    }

    result.resolvedGroup = validation.groups.front();
    result.degraded = validation.degraded || !result.missingOptionalOutputs.empty() ||
                      !result.missingRequiredOutputs.empty();
    result.stableIdentityConfirmed = std::all_of(
        result.resolvedGroup.outputs.begin(), result.resolvedGroup.outputs.end(),
        [&](const SeatDisplayOutput& output) {
            return findActive(currentTopology, output.outputId) != nullptr;
        });

    if (result.degraded) {
        result.disposition = DisplayRecoveryDisposition::DegradedToSeatPrimary;
        result.diagnostics.push_back(
            "missing secondary output resolved only within the same Seat display group");
        return result;
    }

    if (previousResolvedGroup != nullptr && result.stableIdentityConfirmed &&
        !sameResolvedLayout(*previousResolvedGroup, result.resolvedGroup)) {
        result.disposition = DisplayRecoveryDisposition::RestoreStableLayout;
        result.diagnostics.push_back(
            "stable output identities returned with changed coordinates, DPI, or orientation");
        return result;
    }

    result.disposition = DisplayRecoveryDisposition::Stable;
    return result;
}

DisplayTopologyDebouncer::DisplayTopologyDebouncer(std::uint32_t quietPeriodMs)
    : quietPeriodMs_(std::min<std::uint32_t>(quietPeriodMs, 5000u)) {}

void DisplayTopologyDebouncer::observe(std::uint64_t generation,
                                       std::uint64_t tickMs) noexcept {
    // Display topology generations are monotonic. A delayed callback for an
    // older topology must never replace a newer pending or accepted snapshot.
    if (generation == 0 || generation <= state_.lastObservedGeneration ||
        generation <= state_.lastAcceptedGeneration) {
        return;
    }
    state_.lastObservedGeneration = generation;
    state_.lastChangeTickMs = tickMs;
}

bool DisplayTopologyDebouncer::ready(std::uint64_t tickMs) const noexcept {
    if (state_.lastObservedGeneration == 0 ||
        state_.lastObservedGeneration == state_.lastAcceptedGeneration ||
        tickMs < state_.lastChangeTickMs) {
        return false;
    }
    return tickMs - state_.lastChangeTickMs >= quietPeriodMs_;
}

bool DisplayTopologyDebouncer::accept(std::uint64_t tickMs,
                                      std::uint64_t& generation) noexcept {
    if (!ready(tickMs)) return false;
    state_.lastAcceptedGeneration = state_.lastObservedGeneration;
    generation = state_.lastAcceptedGeneration;
    return true;
}

} // namespace hydra::display
