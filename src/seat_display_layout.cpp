#include "hydra/seat_display_layout.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace hydra::display {
namespace {

[[maybe_unused]] const DisplayOutput* findActiveOutput(const DisplayTopologySnapshot& topology,
                                      const std::string& outputId) {
    const auto found = std::find_if(topology.outputs.begin(), topology.outputs.end(),
                                    [&](const DisplayOutput& output) {
                                        return output.active && output.identity.stableKey() == outputId;
                                    });
    return found == topology.outputs.end() ? nullptr : &*found;
}

DisplayRect unionBounds(const std::vector<SeatDisplayOutput>& outputs) {
    if (outputs.empty()) return {};
    DisplayRect bounds = outputs.front().globalBounds;
    for (std::size_t index = 1; index < outputs.size(); ++index) {
        bounds.left = std::min(bounds.left, outputs[index].globalBounds.left);
        bounds.top = std::min(bounds.top, outputs[index].globalBounds.top);
        bounds.right = std::max(bounds.right, outputs[index].globalBounds.right);
        bounds.bottom = std::max(bounds.bottom, outputs[index].globalBounds.bottom);
    }
    return bounds;
}

std::string seatPrefix(SeatId seatId) {
    return "Seat " + std::to_string(seatId) + ": ";
}

void addWarning(DisplayLayoutValidation& result, SeatDisplayGroup& group,
                std::string message) {
    group.diagnostics.push_back(message);
    result.warnings.push_back(seatPrefix(group.seatId) + std::move(message));
}

bool rectHasArea(const DisplayRect& rect) noexcept {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool rectsOverlap(const DisplayRect& left, const DisplayRect& right) noexcept {
    return std::max(left.left, right.left) < std::min(left.right, right.right) &&
           std::max(left.top, right.top) < std::min(left.bottom, right.bottom);
}

} // namespace

CoordinateTransform::CoordinateTransform(SeatDisplayGroup group)
    : group_(std::move(group)) {}

const SeatDisplayOutput* CoordinateTransform::findOutput(
    const std::string& outputId) const noexcept {
    const auto found = std::find_if(group_.outputs.begin(), group_.outputs.end(),
                                    [&](const SeatDisplayOutput& output) {
                                        return output.outputId == outputId;
                                    });
    return found == group_.outputs.end() ? nullptr : &*found;
}

CoordinatePoint CoordinateTransform::globalToSeat(CoordinatePoint point) const noexcept {
    return {point.x - static_cast<double>(group_.primaryOriginX),
            point.y - static_cast<double>(group_.primaryOriginY)};
}

CoordinatePoint CoordinateTransform::seatToGlobal(CoordinatePoint point) const noexcept {
    return {point.x + static_cast<double>(group_.primaryOriginX),
            point.y + static_cast<double>(group_.primaryOriginY)};
}

std::optional<CoordinatePoint> CoordinateTransform::globalToOutput(
    const std::string& outputId, CoordinatePoint point) const noexcept {
    const auto* output = findOutput(outputId);
    if (output == nullptr) return std::nullopt;
    return CoordinatePoint{
        point.x - static_cast<double>(output->globalBounds.left),
        point.y - static_cast<double>(output->globalBounds.top)};
}

std::optional<CoordinatePoint> CoordinateTransform::outputToGlobal(
    const std::string& outputId, CoordinatePoint point) const noexcept {
    const auto* output = findOutput(outputId);
    if (output == nullptr) return std::nullopt;
    return CoordinatePoint{
        point.x + static_cast<double>(output->globalBounds.left),
        point.y + static_cast<double>(output->globalBounds.top)};
}

std::optional<CoordinatePoint> CoordinateTransform::seatToOutput(
    const std::string& outputId, CoordinatePoint point) const noexcept {
    return globalToOutput(outputId, seatToGlobal(point));
}

std::optional<CoordinatePoint> CoordinateTransform::outputToSeat(
    const std::string& outputId, CoordinatePoint point) const noexcept {
    const auto global = outputToGlobal(outputId, point);
    if (!global) return std::nullopt;
    return globalToSeat(*global);
}

std::optional<CoordinatePoint> CoordinateTransform::physicalPixelsToDip(
    const std::string& outputId, CoordinatePoint point) const noexcept {
    const auto* output = findOutput(outputId);
    if (output == nullptr) return std::nullopt;
    const double dpiX = static_cast<double>(output->dpiX == 0 ? 96u : output->dpiX);
    const double dpiY = static_cast<double>(output->dpiY == 0 ? 96u : output->dpiY);
    return CoordinatePoint{point.x * 96.0 / dpiX, point.y * 96.0 / dpiY};
}

std::optional<CoordinatePoint> CoordinateTransform::dipToPhysicalPixels(
    const std::string& outputId, CoordinatePoint point) const noexcept {
    const auto* output = findOutput(outputId);
    if (output == nullptr) return std::nullopt;
    const double dpiX = static_cast<double>(output->dpiX == 0 ? 96u : output->dpiX);
    const double dpiY = static_cast<double>(output->dpiY == 0 ? 96u : output->dpiY);
    return CoordinatePoint{point.x * dpiX / 96.0, point.y * dpiY / 96.0};
}

CoordinatePoint CoordinateTransform::globalToClient(
    CoordinatePoint globalPoint, CoordinatePoint clientGlobalOrigin) const noexcept {
    return {globalPoint.x - clientGlobalOrigin.x, globalPoint.y - clientGlobalOrigin.y};
}

CoordinatePoint CoordinateTransform::clientToGlobal(
    CoordinatePoint clientPoint, CoordinatePoint clientGlobalOrigin) const noexcept {
    return {clientPoint.x + clientGlobalOrigin.x, clientPoint.y + clientGlobalOrigin.y};
}

std::vector<CoordinateRect> CoordinateTransform::clipGlobalRectToOutputs(
    CoordinateRect rect) const {
    std::vector<CoordinateRect> result;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return result;
    for (const auto& output : group_.outputs) {
        CoordinateRect clipped;
        clipped.left = std::max(rect.left, static_cast<double>(output.globalBounds.left));
        clipped.top = std::max(rect.top, static_cast<double>(output.globalBounds.top));
        clipped.right = std::min(rect.right, static_cast<double>(output.globalBounds.right));
        clipped.bottom = std::min(rect.bottom, static_cast<double>(output.globalBounds.bottom));
        if (clipped.right > clipped.left && clipped.bottom > clipped.top) {
            result.push_back(clipped);
        }
    }
    return result;
}

DisplayLayoutValidation buildSeatDisplayLayouts(
    const DisplayTopologySnapshot& topology,
    const std::vector<SeatDisplayRequest>& requests) {
    DisplayLayoutValidation result;
    if (!topology.querySucceeded) {
        result.errors.push_back("display topology is unavailable; Seat display layout cannot be resolved");
        return result;
    }
    if (requests.empty()) {
        result.errors.push_back("no Seat display requests were provided");
        return result;
    }

    std::map<std::string, const DisplayOutput*> activeOutputs;
    for (const auto& output : topology.outputs) {
        if (!output.active) continue;
        const auto key = output.identity.stableKey();
        if (key.empty()) {
            result.errors.push_back("active display target has an empty stable output identifier");
            continue;
        }
        if (!activeOutputs.emplace(key, &output).second) {
            result.errors.push_back("duplicate active stable output identifier: " + key);
        }
    }
    if (!result.errors.empty()) return result;

    std::set<SeatId> seenSeats;
    std::map<std::string, std::vector<std::pair<SeatId, bool>>> outputOwners;

    for (const auto& request : requests) {
        SeatDisplayGroup group;
        group.seatId = request.seatId;
        if (request.seatId == 0) {
            result.errors.push_back("Seat display request uses reserved Seat 0");
            continue;
        }
        if (!seenSeats.insert(request.seatId).second) {
            result.errors.push_back(seatPrefix(request.seatId) + "duplicate display request");
            continue;
        }
        if (request.outputs.empty()) {
            result.errors.push_back(seatPrefix(request.seatId) + "no display outputs requested");
            continue;
        }

        std::set<std::string> seenSelections;
        for (const auto& selection : request.outputs) {
            if (selection.outputId.empty()) {
                result.errors.push_back(seatPrefix(request.seatId) +
                                        "display selection has an empty stable output identifier");
                continue;
            }
            if (!seenSelections.insert(selection.outputId).second) {
                result.errors.push_back(seatPrefix(request.seatId) +
                                        "display selected more than once: " + selection.outputId);
                continue;
            }

            const auto found = activeOutputs.find(selection.outputId);
            if (found == activeOutputs.end()) {
                if (selection.required) {
                    const std::string message = "required output is missing or inactive: " +
                                                selection.outputId;
                    if (request.missingOutputPolicy == MissingOutputPolicy::Block) {
                        result.errors.push_back(seatPrefix(request.seatId) + message);
                    } else {
                        group.degraded = true;
                        addWarning(result, group, message);
                    }
                } else {
                    addWarning(result, group,
                               "optional output is missing or inactive: " + selection.outputId);
                }
                continue;
            }

            const auto& output = *found->second;
            if (!rectHasArea(output.desktopBounds)) {
                const std::string message = "active output has empty desktop bounds: " +
                                            selection.outputId;
                if (request.missingOutputPolicy == MissingOutputPolicy::Block) {
                    result.errors.push_back(seatPrefix(request.seatId) + message);
                } else {
                    group.degraded = true;
                    addWarning(result, group, message);
                }
                continue;
            }

            SeatDisplayOutput resolved;
            resolved.outputId = selection.outputId;
            resolved.identity = output.identity;
            resolved.globalBounds = output.desktopBounds;
            resolved.orientation = output.mode.orientation;
            resolved.dpiX = output.dpiX == 0 ? 96u : output.dpiX;
            resolved.dpiY = output.dpiY == 0 ? 96u : output.dpiY;
            resolved.windowsPrimary = output.primary;
            resolved.shareable = selection.shareable;
            group.outputs.push_back(std::move(resolved));
            outputOwners[selection.outputId].push_back({request.seatId, selection.shareable});
        }

        if (group.outputs.empty()) {
            result.errors.push_back(seatPrefix(request.seatId) +
                                    "has no active resolved display output");
            continue;
        }
        std::sort(group.outputs.begin(), group.outputs.end(),
                  [](const SeatDisplayOutput& left, const SeatDisplayOutput& right) {
                      return left.outputId < right.outputId;
                  });

        auto primary = group.outputs.begin();
        if (!request.primaryOutputId.empty()) {
            primary = std::find_if(group.outputs.begin(), group.outputs.end(),
                                   [&](const SeatDisplayOutput& output) {
                                       return output.outputId == request.primaryOutputId;
                                   });
            if (primary == group.outputs.end()) {
                const std::string message = "requested Seat primary output is missing/inactive: " +
                                            request.primaryOutputId;
                if (request.missingOutputPolicy == MissingOutputPolicy::Block) {
                    result.errors.push_back(seatPrefix(request.seatId) + message);
                    continue;
                }
                group.degraded = true;
                addWarning(result, group, message + "; using first resolved output");
                primary = group.outputs.begin();
            }
        }

        group.primaryOutputId = primary->outputId;
        group.primaryOriginX = primary->globalBounds.left;
        group.primaryOriginY = primary->globalBounds.top;
        group.globalBounds = unionBounds(group.outputs);
        result.degraded = result.degraded || group.degraded;
        result.groups.push_back(std::move(group));
    }

    for (const auto& [outputId, owners] : outputOwners) {
        if (owners.size() < 2) continue;
        const bool allShareable = std::all_of(owners.begin(), owners.end(),
                                              [](const auto& owner) { return owner.second; });
        if (!allShareable) {
            std::string seats;
            for (const auto& [seatId, ignoredShareable] : owners) {
                (void)ignoredShareable;
                if (!seats.empty()) seats += ",";
                seats += std::to_string(seatId);
            }
            result.errors.push_back("output " + outputId +
                                    " overlaps non-shareable Seat assignments: " + seats);
        }
    }

    for (std::size_t leftIndex = 0; leftIndex < result.groups.size(); ++leftIndex) {
        for (std::size_t rightIndex = leftIndex + 1; rightIndex < result.groups.size(); ++rightIndex) {
            const auto& leftGroup = result.groups[leftIndex];
            const auto& rightGroup = result.groups[rightIndex];
            for (const auto& leftOutput : leftGroup.outputs) {
                for (const auto& rightOutput : rightGroup.outputs) {
                    if (leftOutput.outputId == rightOutput.outputId ||
                        !rectsOverlap(leftOutput.globalBounds, rightOutput.globalBounds)) {
                        continue;
                    }
                    if (leftOutput.shareable && rightOutput.shareable) continue;
                    result.errors.push_back(
                        "Seat " + std::to_string(leftGroup.seatId) + " output " +
                        leftOutput.outputId + " spatially overlaps Seat " +
                        std::to_string(rightGroup.seatId) + " output " +
                        rightOutput.outputId + " without explicit sharing");
                }
            }
        }
    }

    std::sort(result.groups.begin(), result.groups.end(),
              [](const SeatDisplayGroup& left, const SeatDisplayGroup& right) {
                  return left.seatId < right.seatId;
              });
    result.valid = result.errors.empty() && result.groups.size() == requests.size();
    return result;
}

} // namespace hydra::display
