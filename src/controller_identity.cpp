#include "hydra/controller_identity.hpp"

#include <algorithm>
#include <cwctype>
#include <set>
#include <tuple>

namespace hydra::controller {
namespace {

std::wstring canonicalId(std::wstring value) {
    for (auto& ch : value) ch = static_cast<wchar_t>(std::towupper(ch));
    return value;
}

void addIssue(BindingPlan& plan, BindingIssueCode code, std::uint32_t seatId,
              std::wstring controllerId = {}) {
    plan.valid = false;
    plan.issues.push_back({code, seatId, std::move(controllerId)});
}

const SourceDescriptor* findPersistentSource(
    std::span<const SourceDescriptor> sources,
    ApiSurface api,
    const std::wstring& persistentId,
    std::size_t& matches) {
    const auto wanted = canonicalId(persistentId);
    const SourceDescriptor* result = nullptr;
    matches = 0;
    for (const auto& source : sources) {
        if (source.api != api || source.identityQuality != IdentityQuality::Stable ||
            !source.persistentId || canonicalId(*source.persistentId) != wanted) {
            continue;
        }
        ++matches;
        result = &source;
    }
    return result;
}

const SourceDescriptor* findRuntimeXInputSource(
    std::span<const SourceDescriptor> sources,
    std::uint8_t slot,
    std::size_t& matches) {
    const SourceDescriptor* result = nullptr;
    matches = 0;
    for (const auto& source : sources) {
        if (source.api != ApiSurface::XInput ||
            source.identityQuality != IdentityQuality::RuntimeOnly ||
            !source.runtimeXInputSlot || *source.runtimeXInputSlot != slot) {
            continue;
        }
        ++matches;
        result = &source;
    }
    return result;
}

bool persistentIdExistsOnAnotherApi(std::span<const SourceDescriptor> sources,
                                    ApiSurface requestedApi,
                                    const std::wstring& persistentId) {
    const auto wanted = canonicalId(persistentId);
    for (const auto& source : sources) {
        if (source.api == requestedApi ||
            source.identityQuality != IdentityQuality::Stable ||
            !source.persistentId) {
            continue;
        }
        if (canonicalId(*source.persistentId) == wanted) return true;
    }
    return false;
}

} // namespace

BindingPlan planSeatBindings(std::span<const SeatBindingRequest> requests,
                             std::span<const SourceDescriptor> sources) {
    BindingPlan plan;
    if (requests.size() > 2u) {
        addIssue(plan, BindingIssueCode::V1SeatLimitExceeded, 0);
        return plan;
    }

    std::set<std::uint32_t> seats;
    std::set<std::string> assignedRuntimeKeys;

    for (const auto& request : requests) {
        if (request.seatId != 1 && request.seatId != 2) {
            addIssue(plan, BindingIssueCode::InvalidSeat, request.seatId);
            continue;
        }
        if (!seats.insert(request.seatId).second) {
            addIssue(plan, BindingIssueCode::DuplicateSeat, request.seatId);
            continue;
        }

        const SourceDescriptor* source = nullptr;
        std::size_t matches = 0;
        std::wstring issueId;

        if (request.persistentControllerId && !request.persistentControllerId->empty()) {
            issueId = *request.persistentControllerId;
            source = findPersistentSource(sources, request.api, issueId, matches);
            if (matches == 0) {
                addIssue(plan,
                         persistentIdExistsOnAnotherApi(sources, request.api, issueId)
                             ? BindingIssueCode::SourceApiMismatch
                             : BindingIssueCode::SourceNotFound,
                         request.seatId, issueId);
                continue;
            }
        } else if (request.api == ApiSurface::XInput && request.runtimeXInputSlot) {
            if (*request.runtimeXInputSlot >= kXInputSlotCount) {
                addIssue(plan, BindingIssueCode::RuntimeSlotOutOfRange,
                         request.seatId);
                continue;
            }
            source = findRuntimeXInputSource(sources, *request.runtimeXInputSlot,
                                             matches);
            if (matches == 0) {
                addIssue(plan, BindingIssueCode::SourceNotFound, request.seatId);
                continue;
            }
        } else {
            addIssue(plan, BindingIssueCode::MissingPersistentIdentity,
                     request.seatId);
            continue;
        }

        if (matches != 1 || source == nullptr || source->runtimeKey.empty()) {
            addIssue(plan, BindingIssueCode::AmbiguousSource,
                     request.seatId, issueId);
            continue;
        }
        if (source->api != request.api) {
            addIssue(plan, BindingIssueCode::SourceApiMismatch,
                     request.seatId, issueId);
            continue;
        }
        if (!source->connected) {
            addIssue(plan, BindingIssueCode::SourceDisconnected,
                     request.seatId, issueId);
            continue;
        }
        if (!assignedRuntimeKeys.insert(source->runtimeKey).second) {
            addIssue(plan, BindingIssueCode::SourceAlreadyAssigned,
                     request.seatId, issueId);
            continue;
        }

        plan.bindings.push_back({request.seatId,
                                 source->api,
                                 source->runtimeKey,
                                 request.persistentControllerId
                                     ? source->persistentId
                                     : std::nullopt,
                                 source->runtimeXInputSlot});
    }

    std::sort(plan.bindings.begin(), plan.bindings.end(),
              [](const SeatBinding& left, const SeatBinding& right) {
                  return left.seatId < right.seatId;
              });
    std::sort(plan.issues.begin(), plan.issues.end(),
              [](const BindingIssue& left, const BindingIssue& right) {
                  return std::tie(left.seatId, left.code, left.controllerId) <
                         std::tie(right.seatId, right.code, right.controllerId);
              });
    return plan;
}

bool sameControllerSource(const SeatBinding& left,
                          const SeatBinding& right) noexcept {
    if (left.persistentControllerId && right.persistentControllerId) {
        return canonicalId(*left.persistentControllerId) ==
               canonicalId(*right.persistentControllerId);
    }
    return !left.runtimeKey.empty() && left.runtimeKey == right.runtimeKey;
}

} // namespace hydra::controller
