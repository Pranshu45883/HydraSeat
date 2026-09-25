#pragma once

#include "hydra/display_topology.hpp"
#include "hydra/workspace_manager.hpp"

#include <optional>
#include <string>
#include <vector>

namespace hydra::display {

enum class MissingOutputPolicy : std::uint8_t {
    Block = 0,
    Degrade = 1,
};

struct SeatDisplaySelection {
    std::string outputId;
    bool required{true};
    bool shareable{false};
};

struct SeatDisplayRequest {
    SeatId seatId{0};
    std::vector<SeatDisplaySelection> outputs;
    std::string primaryOutputId;
    MissingOutputPolicy missingOutputPolicy{MissingOutputPolicy::Block};
};

struct SeatDisplayOutput {
    std::string outputId;
    DisplayOutputIdentity identity;
    DisplayRect globalBounds;
    DisplayOrientation orientation{DisplayOrientation::Unknown};
    std::uint32_t dpiX{96};
    std::uint32_t dpiY{96};
    bool windowsPrimary{false};
    bool shareable{false};
};

struct SeatDisplayGroup {
    SeatId seatId{0};
    std::vector<SeatDisplayOutput> outputs;
    std::string primaryOutputId;
    DisplayRect globalBounds;
    std::int32_t primaryOriginX{0};
    std::int32_t primaryOriginY{0};
    bool degraded{false};
    std::vector<std::string> diagnostics;
};

struct DisplayLayoutValidation {
    bool valid{false};
    bool degraded{false};
    std::vector<SeatDisplayGroup> groups;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

struct CoordinatePoint {
    double x{0.0};
    double y{0.0};

    friend bool operator==(const CoordinatePoint&, const CoordinatePoint&) = default;
};

struct CoordinateRect {
    double left{0.0};
    double top{0.0};
    double right{0.0};
    double bottom{0.0};

    double width() const noexcept { return right - left; }
    double height() const noexcept { return bottom - top; }
};

class CoordinateTransform {
public:
    explicit CoordinateTransform(SeatDisplayGroup group);

    CoordinatePoint globalToSeat(CoordinatePoint point) const noexcept;
    CoordinatePoint seatToGlobal(CoordinatePoint point) const noexcept;

    std::optional<CoordinatePoint> globalToOutput(const std::string& outputId,
                                                   CoordinatePoint point) const noexcept;
    std::optional<CoordinatePoint> outputToGlobal(const std::string& outputId,
                                                   CoordinatePoint point) const noexcept;
    std::optional<CoordinatePoint> seatToOutput(const std::string& outputId,
                                                 CoordinatePoint point) const noexcept;
    std::optional<CoordinatePoint> outputToSeat(const std::string& outputId,
                                                 CoordinatePoint point) const noexcept;

    std::optional<CoordinatePoint> physicalPixelsToDip(const std::string& outputId,
                                                        CoordinatePoint point) const noexcept;
    std::optional<CoordinatePoint> dipToPhysicalPixels(const std::string& outputId,
                                                        CoordinatePoint point) const noexcept;

    CoordinatePoint globalToClient(CoordinatePoint globalPoint,
                                   CoordinatePoint clientGlobalOrigin) const noexcept;
    CoordinatePoint clientToGlobal(CoordinatePoint clientPoint,
                                   CoordinatePoint clientGlobalOrigin) const noexcept;

    // Returns one clipped rectangle per intersected output. Keeping pieces
    // separate preserves gaps in vertical/L-shaped Seat layouts.
    std::vector<CoordinateRect> clipGlobalRectToOutputs(CoordinateRect rect) const;

    const SeatDisplayGroup& group() const noexcept { return group_; }

private:
    const SeatDisplayOutput* findOutput(const std::string& outputId) const noexcept;
    SeatDisplayGroup group_;
};

DisplayLayoutValidation buildSeatDisplayLayouts(
    const DisplayTopologySnapshot& topology,
    const std::vector<SeatDisplayRequest>& requests);

} // namespace hydra::display
