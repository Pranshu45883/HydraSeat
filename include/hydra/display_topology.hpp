#pragma once

#include "hydra/display_identity.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hydra::display {

struct DisplayRect {
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};

    std::int32_t width() const noexcept { return right - left; }
    std::int32_t height() const noexcept { return bottom - top; }
    friend bool operator==(const DisplayRect&, const DisplayRect&) = default;
};

enum class DisplayOrientation : std::uint8_t {
    Identity = 0,
    Rotate90 = 1,
    Rotate180 = 2,
    Rotate270 = 3,
    Unknown = 255,
};

struct DisplayMode {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t refreshNumerator{0};
    std::uint32_t refreshDenominator{0};
    std::uint32_t pixelFormat{0};
    DisplayOrientation orientation{DisplayOrientation::Unknown};

    friend bool operator==(const DisplayMode&, const DisplayMode&) = default;
};

enum class VirtualDisplayLikelihood : std::uint8_t {
    PhysicalLikely = 0,
    VirtualLikely = 1,
    Unknown = 2,
};

enum class ClassificationConfidence : std::uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
};

struct DisplayAdapter {
    DisplayAdapterIdentity identity;
    std::wstring description;
    std::uint32_t vendorId{0};
    std::uint32_t deviceId{0};
    std::uint64_t dedicatedVideoMemory{0};
};

struct DisplayOutput {
    DisplayOutputIdentity identity;
    std::uint32_t sourceId{0};
    std::wstring gdiDeviceName;
    std::wstring friendlyName;
    std::uint16_t edidManufacturerId{0};
    std::uint16_t edidProductCodeId{0};
    std::uint32_t connectorInstance{0};
    std::uint32_t outputTechnology{0};

    DisplayRect desktopBounds;
    DisplayMode mode;
    std::uint32_t dpiX{96};
    std::uint32_t dpiY{96};
    std::uint32_t effectiveScalePercent{100};

    bool primary{false};
    bool active{false};
    bool attached{false};
    bool dxgiMatched{false};
    std::wstring dxgiAdapterDescription;
    std::wstring dxgiOutputDeviceName;

    VirtualDisplayLikelihood virtualLikelihood{VirtualDisplayLikelihood::Unknown};
    ClassificationConfidence classificationConfidence{ClassificationConfidence::Low};
    std::vector<std::string> classificationReasons;
    std::vector<std::string> diagnostics;
};

struct DisplayTopologySnapshot {
    std::uint64_t generation{0};
    std::uint32_t queryAttempts{0};
    bool querySucceeded{false};
    std::vector<DisplayAdapter> adapters;
    std::vector<DisplayOutput> outputs;
    std::vector<std::string> diagnostics;
};

// Provider-neutral observations keep the assembler deterministic and make
// topology races/correlation testable without changing a real display mode.
struct DisplayPathObservation {
    DisplayOutputIdentity identity;
    std::uint32_t sourceId{0};
    std::wstring gdiDeviceName;
    std::wstring friendlyName;
    std::uint16_t edidManufacturerId{0};
    std::uint16_t edidProductCodeId{0};
    std::uint32_t connectorInstance{0};
    std::uint32_t outputTechnology{0};
    DisplayRect desktopBounds;
    DisplayMode mode;
    std::uint32_t dpiX{96};
    std::uint32_t dpiY{96};
    bool primary{false};
    bool active{false};
    bool attached{false};
    bool technologySuggestsVirtual{false};
    std::vector<std::string> diagnostics;
};

struct DxgiAdapterObservation {
    DisplayAdapterIdentity identity;
    std::wstring description;
    std::uint32_t vendorId{0};
    std::uint32_t deviceId{0};
    std::uint64_t dedicatedVideoMemory{0};
};

struct DxgiOutputObservation {
    AdapterLuid adapterLuid;
    std::wstring gdiDeviceName;
    DisplayRect desktopBounds;
    bool attachedToDesktop{false};
    std::wstring adapterDescription;
};

struct DisplayTopologyObservation {
    std::vector<DisplayPathObservation> paths;
    std::vector<DxgiAdapterObservation> adapters;
    std::vector<DxgiOutputObservation> dxgiOutputs;
    std::vector<std::string> diagnostics;
};

enum class DisplayQueryStatus : std::uint8_t {
    Success = 0,
    TopologyChanged = 1,
    Failed = 2,
};

struct DisplayQueryResult {
    DisplayQueryStatus status{DisplayQueryStatus::Failed};
    DisplayTopologyObservation observation;
    std::string error;
};

using DisplayTopologyQuery = std::function<DisplayQueryResult()>;

DisplayTopologySnapshot assembleDisplayTopology(const DisplayTopologyObservation& observation,
                                                 std::uint64_t generation,
                                                 std::uint32_t queryAttempts = 1);

DisplayTopologySnapshot collectDisplayTopology(const DisplayTopologyQuery& query,
                                                std::uint64_t generation,
                                                std::uint32_t maxAttempts = 3);

class DisplayTopologyInventory {
public:
    DisplayTopologySnapshot refresh();
    std::uint64_t generation() const noexcept { return generation_; }

private:
    std::uint64_t generation_{0};
};

} // namespace hydra::display
