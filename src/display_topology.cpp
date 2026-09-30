#include "hydra/display_topology.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_2.h>
#endif

namespace hydra::display {
namespace {

std::string hex32(std::uint32_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(8) << value;
    return stream.str();
}

std::uint64_t fnv1aLowerUtf16(std::wstring_view value) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    for (const wchar_t raw : value) {
        const auto ch = static_cast<std::uint32_t>(
            std::towlower(static_cast<std::wint_t>(raw)));
        hash ^= static_cast<std::uint8_t>(ch & 0xffu);
        hash *= prime;
        hash ^= static_cast<std::uint8_t>((ch >> 8u) & 0xffu);
        hash *= prime;
        hash ^= static_cast<std::uint8_t>((ch >> 16u) & 0xffu);
        hash *= prime;
        hash ^= static_cast<std::uint8_t>((ch >> 24u) & 0xffu);
        hash *= prime;
    }
    return hash;
}

std::string hex64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::wstring lowercase(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(ch)));
    });
    return result;
}

bool equalsInsensitive(std::wstring_view left, std::wstring_view right) {
#ifdef _WIN32
    if (left.size() != right.size()) return false;
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
#else
    return lowercase(left) == lowercase(right);
#endif
}

bool containsVirtualKeyword(std::wstring_view value) {
    const auto lowered = lowercase(value);
    constexpr std::array<std::wstring_view, 8> keywords{
        L"virtual", L"indirect", L"remote", L"rdp", L"idd",
        L"parsec", L"spacedesk", L"dummy"
    };
    return std::any_of(keywords.begin(), keywords.end(), [&](std::wstring_view keyword) {
        return lowered.find(keyword) != std::wstring::npos;
    });
}

void appendUnique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) return;
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

void classifyOutput(DisplayOutput& output, bool technologySuggestsVirtual) {
    const bool keyword = containsVirtualKeyword(output.friendlyName) ||
                         containsVirtualKeyword(output.identity.monitorDevicePath) ||
                         containsVirtualKeyword(output.gdiDeviceName) ||
                         containsVirtualKeyword(output.dxgiAdapterDescription) ||
                         containsVirtualKeyword(output.dxgiOutputDeviceName);
    if (keyword) {
        output.virtualLikelihood = VirtualDisplayLikelihood::VirtualLikely;
        output.classificationConfidence = ClassificationConfidence::High;
        output.classificationReasons.push_back("virtual/remote keyword in display metadata");
        return;
    }
    if (technologySuggestsVirtual) {
        output.virtualLikelihood = VirtualDisplayLikelihood::VirtualLikely;
        output.classificationConfidence = ClassificationConfidence::Medium;
        output.classificationReasons.push_back("Windows output technology reports indirect display");
        return;
    }
    if (!output.identity.monitorDevicePath.empty() || output.edidManufacturerId != 0 ||
        output.edidProductCodeId != 0) {
        output.virtualLikelihood = VirtualDisplayLikelihood::PhysicalLikely;
        output.classificationConfidence = ClassificationConfidence::Medium;
        output.classificationReasons.push_back("monitor device/EDID metadata is present");
        return;
    }
    output.virtualLikelihood = VirtualDisplayLikelihood::Unknown;
    output.classificationConfidence = ClassificationConfidence::Low;
    output.classificationReasons.push_back("insufficient evidence for physical/virtual classification");
}

void mergeObservation(DisplayOutput& output, const DisplayPathObservation& path) {
    const bool wasActive = output.active;
    if (path.active || !wasActive) output.sourceId = path.sourceId;
    if (output.identity.monitorDevicePath.empty() && !path.identity.monitorDevicePath.empty()) {
        output.identity.monitorDevicePath = path.identity.monitorDevicePath;
    }
    if (output.gdiDeviceName.empty() && !path.gdiDeviceName.empty()) output.gdiDeviceName = path.gdiDeviceName;
    if (output.friendlyName.empty() && !path.friendlyName.empty()) output.friendlyName = path.friendlyName;
    if (output.edidManufacturerId == 0) output.edidManufacturerId = path.edidManufacturerId;
    if (output.edidProductCodeId == 0) output.edidProductCodeId = path.edidProductCodeId;
    if (output.connectorInstance == 0) output.connectorInstance = path.connectorInstance;
    if (output.outputTechnology == 0) output.outputTechnology = path.outputTechnology;
    if (output.desktopBounds.width() == 0 || output.desktopBounds.height() == 0 || path.active) {
        output.desktopBounds = path.desktopBounds;
    }
    if (output.mode.width == 0 || output.mode.height == 0 || path.active) output.mode = path.mode;
    if (path.dpiX != 0) output.dpiX = path.dpiX;
    if (path.dpiY != 0) output.dpiY = path.dpiY;
    output.primary = output.primary || path.primary;
    output.active = output.active || path.active;
    output.attached = output.attached || path.attached;
    for (const auto& diagnostic : path.diagnostics) appendUnique(output.diagnostics, diagnostic);
}

DisplayOutput outputFromPath(const DisplayPathObservation& path) {
    DisplayOutput output;
    output.identity = path.identity;
    output.sourceId = path.sourceId;
    output.gdiDeviceName = path.gdiDeviceName;
    output.friendlyName = path.friendlyName;
    output.edidManufacturerId = path.edidManufacturerId;
    output.edidProductCodeId = path.edidProductCodeId;
    output.connectorInstance = path.connectorInstance;
    output.outputTechnology = path.outputTechnology;
    output.desktopBounds = path.desktopBounds;
    output.mode = path.mode;
    output.dpiX = path.dpiX == 0 ? 96u : path.dpiX;
    output.dpiY = path.dpiY == 0 ? 96u : path.dpiY;
    output.primary = path.primary;
    output.active = path.active;
    output.attached = path.attached;
    output.diagnostics = path.diagnostics;
    return output;
}

#ifdef _WIN32

AdapterLuid convertLuid(const LUID& luid) noexcept {
    return AdapterLuid{luid.LowPart, luid.HighPart};
}

DisplayRect convertRect(const RECT& rectangle) noexcept {
    return DisplayRect{rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
}

DisplayOrientation convertOrientation(DISPLAYCONFIG_ROTATION rotation) noexcept {
    switch (rotation) {
        case DISPLAYCONFIG_ROTATION_IDENTITY: return DisplayOrientation::Identity;
        case DISPLAYCONFIG_ROTATION_ROTATE90: return DisplayOrientation::Rotate90;
        case DISPLAYCONFIG_ROTATION_ROTATE180: return DisplayOrientation::Rotate180;
        case DISPLAYCONFIG_ROTATION_ROTATE270: return DisplayOrientation::Rotate270;
        default: return DisplayOrientation::Unknown;
    }
}

bool checkedDisplayEndpoint(std::int32_t origin, std::uint32_t extent,
                            std::int32_t& endpoint) noexcept {
    const auto value = static_cast<std::int64_t>(origin) +
                       static_cast<std::int64_t>(extent);
    if (value < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) ||
        value > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
        return false;
    }
    endpoint = static_cast<std::int32_t>(value);
    return true;
}

std::string win32Diagnostic(const char* operation, LONG code) {
    return std::string(operation) + " failed (Win32=" + std::to_string(code) + ")";
}

struct MonitorObservation {
    HMONITOR monitor{nullptr};
    std::wstring gdiDeviceName;
    DisplayRect bounds;
    bool primary{false};
    std::uint32_t dpiX{96};
    std::uint32_t dpiY{96};
};

using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);

struct MonitorEnumContext {
    std::vector<MonitorObservation>* monitors{nullptr};
    std::vector<std::string>* diagnostics{nullptr};
    GetDpiForMonitorFn getDpiForMonitor{nullptr};
};

BOOL CALLBACK monitorEnumCallback(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* context = reinterpret_cast<MonitorEnumContext*>(parameter);
    if (context == nullptr || context->monitors == nullptr) return TRUE;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) return TRUE;

    MonitorObservation observation;
    observation.monitor = monitor;
    observation.gdiDeviceName = info.szDevice;
    observation.bounds = convertRect(info.rcMonitor);
    observation.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (context->getDpiForMonitor != nullptr) {
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (SUCCEEDED(context->getDpiForMonitor(monitor, 0, &dpiX, &dpiY))) {
            observation.dpiX = dpiX;
            observation.dpiY = dpiY;
        } else if (context->diagnostics != nullptr) {
            appendUnique(*context->diagnostics,
                         "GetDpiForMonitor failed for an active monitor; defaulting to 96 DPI");
        }
    }
    context->monitors->push_back(std::move(observation));
    return TRUE;
}

std::vector<MonitorObservation> queryMonitors(std::vector<std::string>& diagnostics) {
    std::vector<MonitorObservation> monitors;
    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    GetDpiForMonitorFn dpiFunction = nullptr;
    if (shcore != nullptr) {
        dpiFunction = reinterpret_cast<GetDpiForMonitorFn>(
            GetProcAddress(shcore, "GetDpiForMonitor"));
    }
    if (dpiFunction == nullptr) {
        appendUnique(diagnostics,
                     "GetDpiForMonitor unavailable; active monitor DPI defaults to 96");
    }
    MonitorEnumContext context{&monitors, &diagnostics, dpiFunction};
    if (EnumDisplayMonitors(nullptr, nullptr, monitorEnumCallback,
                            reinterpret_cast<LPARAM>(&context)) == FALSE) {
        appendUnique(diagnostics,
                     "EnumDisplayMonitors failed; DisplayConfig target data preserved");
    }
    if (shcore != nullptr) FreeLibrary(shcore);
    return monitors;
}

const MonitorObservation* findMonitor(const std::vector<MonitorObservation>& monitors,
                                      std::wstring_view gdiName) {
    const auto found = std::find_if(monitors.begin(), monitors.end(),
                                    [gdiName](const MonitorObservation& monitor) {
                                        return equalsInsensitive(monitor.gdiDeviceName, gdiName);
                                    });
    return found == monitors.end() ? nullptr : &*found;
}

void queryDxgi(DisplayTopologyObservation& observation) {
    IDXGIFactory1* factory = nullptr;
    const HRESULT factoryResult = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                                      reinterpret_cast<void**>(&factory));
    if (FAILED(factoryResult) || factory == nullptr) {
        observation.diagnostics.push_back("DXGI factory unavailable; DisplayConfig data preserved");
        return;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, &adapter);
        if (adapterResult == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(adapterResult) || adapter == nullptr) {
            observation.diagnostics.push_back("DXGI adapter enumeration returned an error");
            continue;
        }

        DXGI_ADAPTER_DESC1 adapterDescription{};
        if (FAILED(adapter->GetDesc1(&adapterDescription))) {
            observation.diagnostics.push_back(
                "DXGI adapter description unavailable; its outputs cannot be safely correlated");
            adapter->Release();
            continue;
        }
        DxgiAdapterObservation adapterObservation;
        adapterObservation.identity.luid = convertLuid(adapterDescription.AdapterLuid);
        adapterObservation.description = adapterDescription.Description;
        adapterObservation.vendorId = adapterDescription.VendorId;
        adapterObservation.deviceId = adapterDescription.DeviceId;
        adapterObservation.dedicatedVideoMemory =
            static_cast<std::uint64_t>(adapterDescription.DedicatedVideoMemory);
        observation.adapters.push_back(std::move(adapterObservation));

        for (UINT outputIndex = 0;; ++outputIndex) {
            IDXGIOutput* output = nullptr;
            const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &output);
            if (outputResult == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(outputResult) || output == nullptr) continue;
            DXGI_OUTPUT_DESC outputDescription{};
            if (SUCCEEDED(output->GetDesc(&outputDescription))) {
                DxgiOutputObservation outputObservation;
                outputObservation.adapterLuid = convertLuid(adapterDescription.AdapterLuid);
                outputObservation.gdiDeviceName = outputDescription.DeviceName;
                outputObservation.desktopBounds = convertRect(outputDescription.DesktopCoordinates);
                outputObservation.attachedToDesktop = outputDescription.AttachedToDesktop != FALSE;
                outputObservation.adapterDescription = adapterDescription.Description;
                observation.dxgiOutputs.push_back(std::move(outputObservation));
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
}

DisplayQueryResult queryWindowsDisplayTopology() {
    DisplayQueryResult result;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    constexpr UINT32 queryFlags = QDC_ALL_PATHS;
    const LONG sizeResult = GetDisplayConfigBufferSizes(queryFlags, &pathCount, &modeCount);
    if (sizeResult != ERROR_SUCCESS) {
        result.status = DisplayQueryStatus::Failed;
        result.error = win32Diagnostic("GetDisplayConfigBufferSizes", sizeResult);
        return result;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    LONG queryResult = QueryDisplayConfig(queryFlags, &pathCount, paths.data(),
                                          &modeCount, modes.data(), nullptr);
    if (queryResult == ERROR_INSUFFICIENT_BUFFER) {
        result.status = DisplayQueryStatus::TopologyChanged;
        result.error = "display topology changed while QueryDisplayConfig buffers were being filled";
        return result;
    }
    if (queryResult != ERROR_SUCCESS) {
        result.status = DisplayQueryStatus::Failed;
        result.error = win32Diagnostic("QueryDisplayConfig", queryResult);
        return result;
    }
    paths.resize(pathCount);
    modes.resize(modeCount);

    const auto monitors = queryMonitors(result.observation.diagnostics);
    result.observation.paths.reserve(paths.size());
    for (const auto& path : paths) {
        DisplayPathObservation observed;
        observed.identity.adapterLuid = convertLuid(path.targetInfo.adapterId);
        observed.identity.targetId = path.targetInfo.id;
        observed.sourceId = path.sourceInfo.id;
        observed.active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        observed.attached = path.targetInfo.targetAvailable != FALSE;
        observed.outputTechnology = static_cast<std::uint32_t>(path.targetInfo.outputTechnology);
        observed.technologySuggestsVirtual = observed.outputTechnology == 16u;
        observed.mode.refreshNumerator = path.targetInfo.refreshRate.Numerator;
        observed.mode.refreshDenominator = path.targetInfo.refreshRate.Denominator;
        observed.mode.orientation = convertOrientation(path.targetInfo.rotation);

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        const LONG targetResult = DisplayConfigGetDeviceInfo(&targetName.header);
        if (targetResult == ERROR_SUCCESS) {
            observed.friendlyName = targetName.monitorFriendlyDeviceName;
            observed.identity.monitorDevicePath = targetName.monitorDevicePath;
            observed.edidManufacturerId = targetName.edidManufactureId;
            observed.edidProductCodeId = targetName.edidProductCodeId;
            observed.connectorInstance = targetName.connectorInstance;
        } else {
            observed.diagnostics.push_back(win32Diagnostic("GET_TARGET_NAME", targetResult));
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        const LONG sourceResult = DisplayConfigGetDeviceInfo(&sourceName.header);
        if (sourceResult == ERROR_SUCCESS) {
            observed.gdiDeviceName = sourceName.viewGdiDeviceName;
        } else {
            observed.diagnostics.push_back(win32Diagnostic("GET_SOURCE_NAME", sourceResult));
        }

        if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            path.sourceInfo.modeInfoIdx < modes.size()) {
            const auto& modeInfo = modes[path.sourceInfo.modeInfoIdx];
            if (modeInfo.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                const auto& sourceMode = modeInfo.sourceMode;
                observed.mode.width = sourceMode.width;
                observed.mode.height = sourceMode.height;
                observed.mode.pixelFormat = static_cast<std::uint32_t>(sourceMode.pixelFormat);
                observed.desktopBounds.left = sourceMode.position.x;
                observed.desktopBounds.top = sourceMode.position.y;
                std::int32_t right = 0;
                std::int32_t bottom = 0;
                if (checkedDisplayEndpoint(sourceMode.position.x, sourceMode.width, right) &&
                    checkedDisplayEndpoint(sourceMode.position.y, sourceMode.height, bottom)) {
                    observed.desktopBounds.right = right;
                    observed.desktopBounds.bottom = bottom;
                } else {
                    observed.desktopBounds = {};
                    observed.diagnostics.push_back(
                        "source mode desktop bounds exceed signed 32-bit coordinate range");
                }
            }
        }
        if ((observed.mode.width == 0 || observed.mode.height == 0) &&
            path.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            path.targetInfo.modeInfoIdx < modes.size()) {
            const auto& modeInfo = modes[path.targetInfo.modeInfoIdx];
            if (modeInfo.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                observed.mode.width = modeInfo.targetMode.targetVideoSignalInfo.activeSize.cx;
                observed.mode.height = modeInfo.targetMode.targetVideoSignalInfo.activeSize.cy;
            }
        }

        if (observed.active && !observed.gdiDeviceName.empty()) {
            if (const auto* monitor = findMonitor(monitors, observed.gdiDeviceName)) {
                observed.primary = monitor->primary;
                observed.dpiX = monitor->dpiX;
                observed.dpiY = monitor->dpiY;
                observed.desktopBounds = monitor->bounds;
            }
        }
        result.observation.paths.push_back(std::move(observed));
    }

    queryDxgi(result.observation);
    result.status = DisplayQueryStatus::Success;
    return result;
}

#endif

} // namespace

std::string AdapterLuid::stableKey() const {
    return hex32(static_cast<std::uint32_t>(highPart)) + "-" + hex32(lowPart);
}

std::string DisplayAdapterIdentity::stableKey() const {
    return "adapter-" + luid.stableKey();
}

std::string DisplayOutputIdentity::stableKey() const {
    if (!monitorDevicePath.empty()) {
        return "monitor-" + hex64(fnv1aLowerUtf16(monitorDevicePath)) +
               "-target-" + std::to_string(targetId);
    }
    return "output-" + adapterLuid.stableKey() + "-target-" +
           std::to_string(targetId);
}

DisplayTopologySnapshot assembleDisplayTopology(const DisplayTopologyObservation& observation,
                                                 std::uint64_t generation,
                                                 std::uint32_t queryAttempts) {
    DisplayTopologySnapshot snapshot;
    snapshot.generation = generation;
    snapshot.queryAttempts = queryAttempts;
    snapshot.querySucceeded = true;
    snapshot.diagnostics = observation.diagnostics;

    for (const auto& observedAdapter : observation.adapters) {
        const auto found = std::find_if(snapshot.adapters.begin(), snapshot.adapters.end(),
                                        [&](const DisplayAdapter& adapter) {
                                            return adapter.identity == observedAdapter.identity;
                                        });
        if (found != snapshot.adapters.end()) continue;
        DisplayAdapter adapter;
        adapter.identity = observedAdapter.identity;
        adapter.description = observedAdapter.description;
        adapter.vendorId = observedAdapter.vendorId;
        adapter.deviceId = observedAdapter.deviceId;
        adapter.dedicatedVideoMemory = observedAdapter.dedicatedVideoMemory;
        snapshot.adapters.push_back(std::move(adapter));
    }
    for (const auto& path : observation.paths) {
        const DisplayAdapterIdentity identity{path.identity.adapterLuid};
        const auto found = std::find_if(snapshot.adapters.begin(), snapshot.adapters.end(),
                                        [&](const DisplayAdapter& adapter) {
                                            return adapter.identity == identity;
                                        });
        if (found != snapshot.adapters.end()) continue;
        DisplayAdapter adapter;
        adapter.identity = identity;
        snapshot.adapters.push_back(std::move(adapter));
        appendUnique(snapshot.diagnostics,
                     "DisplayConfig adapter was not enumerated by DXGI; preserving identity-only adapter record");
    }

    std::vector<bool> virtualTechnology;
    for (const auto& path : observation.paths) {
        auto found = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(),
                                  [&](const DisplayOutput& output) {
                                      return output.identity.sameTarget(path.identity);
                                  });
        if (found == snapshot.outputs.end()) {
            snapshot.outputs.push_back(outputFromPath(path));
            virtualTechnology.push_back(path.technologySuggestsVirtual);
        } else {
            const auto index = static_cast<std::size_t>(
                std::distance(snapshot.outputs.begin(), found));
            mergeObservation(*found, path);
            virtualTechnology[index] = virtualTechnology[index] || path.technologySuggestsVirtual;
        }
    }

    for (auto& output : snapshot.outputs) {
        const DxgiOutputObservation* matched = nullptr;
        if (output.active) {
            for (const auto& dxgi : observation.dxgiOutputs) {
                if (!(dxgi.adapterLuid == output.identity.adapterLuid)) continue;
                if (!output.gdiDeviceName.empty() &&
                    equalsInsensitive(dxgi.gdiDeviceName, output.gdiDeviceName)) {
                    matched = &dxgi;
                    break;
                }
                if (matched == nullptr && output.gdiDeviceName.empty() &&
                    dxgi.desktopBounds == output.desktopBounds) {
                    matched = &dxgi;
                }
            }
        }
        if (matched != nullptr) {
            output.dxgiMatched = true;
            output.dxgiAdapterDescription = matched->adapterDescription;
            output.dxgiOutputDeviceName = matched->gdiDeviceName;
            output.attached = output.attached || matched->attachedToDesktop;
        } else if (output.active) {
            output.diagnostics.push_back("no DXGI output correlation for active DisplayConfig target");
        }
        if (output.dpiX == 0) {
            output.effectiveScalePercent = 100u;
        } else {
            const std::uint64_t scaled =
                (static_cast<std::uint64_t>(output.dpiX) * 100ull + 48ull) / 96ull;
            output.effectiveScalePercent = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(scaled,
                    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
        }
    }

    for (std::size_t index = 0; index < snapshot.outputs.size(); ++index) {
        classifyOutput(snapshot.outputs[index], virtualTechnology[index]);
    }

    std::sort(snapshot.adapters.begin(), snapshot.adapters.end(),
              [](const DisplayAdapter& left, const DisplayAdapter& right) {
                  return left.identity.stableKey() < right.identity.stableKey();
              });
    std::sort(snapshot.outputs.begin(), snapshot.outputs.end(),
              [](const DisplayOutput& left, const DisplayOutput& right) {
                  const auto leftKey = left.identity.stableKey();
                  const auto rightKey = right.identity.stableKey();
                  if (leftKey != rightKey) return leftKey < rightKey;
                  return left.sourceId < right.sourceId;
              });
    return snapshot;
}

DisplayTopologySnapshot collectDisplayTopology(const DisplayTopologyQuery& query,
                                                std::uint64_t generation,
                                                std::uint32_t maxAttempts) {
    DisplayTopologySnapshot failure;
    failure.generation = generation;
    if (!query) {
        failure.diagnostics.push_back("display topology query callback is empty");
        return failure;
    }
    maxAttempts = std::clamp<std::uint32_t>(maxAttempts, 1u, 5u);
    for (std::uint32_t attempt = 1; attempt <= maxAttempts; ++attempt) {
        auto result = query();
        if (result.status == DisplayQueryStatus::Success) {
            return assembleDisplayTopology(result.observation, generation, attempt);
        }
        if (result.status == DisplayQueryStatus::Failed) {
            failure.queryAttempts = attempt;
            failure.diagnostics.push_back(result.error.empty()
                ? "display topology provider failed without diagnostic" : result.error);
            return failure;
        }
        if (!result.error.empty()) appendUnique(failure.diagnostics, result.error);
        failure.queryAttempts = attempt;
    }
    failure.diagnostics.push_back("display topology changed during every bounded retry attempt");
    return failure;
}

DisplayTopologySnapshot DisplayTopologyInventory::refresh() {
    ++generation_;
#ifdef _WIN32
    return collectDisplayTopology([] { return queryWindowsDisplayTopology(); }, generation_, 3u);
#else
    DisplayTopologySnapshot snapshot;
    snapshot.generation = generation_;
    snapshot.queryAttempts = 1;
    snapshot.querySucceeded = false;
    snapshot.diagnostics.push_back("display topology inventory is Windows-only");
    return snapshot;
#endif
}

} // namespace hydra::display
