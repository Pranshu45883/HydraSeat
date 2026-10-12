#include "hydra/game_launcher.hpp"

#include "hydra/gate_c_external_session.hpp"
#include "hydra/hardware_identity.hpp"
#include "hydra/input_router.hpp"
#include "hydra/process_group.hpp"
#include "hydra/runtime_host.hpp"
#include "hydra/seat_display_layout.hpp"
#include "hydra/virtual_xinput_pipe.hpp"
#include "hydra/virtual_xinput_service.hpp"
#include "hydra/window_placement.hpp"
#include "hydra/window_tracker.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <limits>
#endif

namespace hydra {
namespace {

#ifdef _WIN32
HANDLE toNativeHandle(std::uintptr_t value) noexcept {
    return reinterpret_cast<HANDLE>(value);
}

std::uintptr_t fromNativeHandle(HANDLE handle) noexcept {
    return reinterpret_cast<std::uintptr_t>(handle);
}

runtime::ProcessIdentity readProcessIdentity(HANDLE process, DWORD pid) noexcept {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return {};

    const std::uint64_t creationIdentity =
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) |
        static_cast<std::uint64_t>(creation.dwLowDateTime);
    return {static_cast<std::uint32_t>(pid), creationIdentity};
}

std::wstring commandLineFor(const GameProfile& game) {
    std::wstring commandLine = L"\"" + game.executablePath + L"\"";
    if (!game.launchArguments.empty()) {
        commandLine += L" ";
        commandLine += game.launchArguments;
    }
    return commandLine;
}

HANDLE createStrictSeatJob() noexcept {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

bool queryActiveProcessCount(HANDLE job, DWORD& activeProcesses) noexcept {
    if (!job || job == INVALID_HANDLE_VALUE) return false;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(
            job,
            JobObjectBasicAccountingInformation,
            &accounting,
            sizeof(accounting),
            nullptr)) {
        return false;
    }
    activeProcesses = accounting.ActiveProcesses;
    return true;
}

bool waitForJobEmpty(HANDLE job, DWORD timeoutMs) noexcept {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD activeProcesses = 0;
        if (!queryActiveProcessCount(job, activeProcesses)) return false;
        if (activeProcesses == 0) return true;

        if (GetTickCount64() - start >= timeoutMs) return false;
        Sleep(5);
    }
}

void terminateCreatedProcess(HANDLE process) noexcept {
    if (!process || process == INVALID_HANDLE_VALUE) return;
    if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process, ERROR_CANCELLED);
        (void)WaitForSingleObject(process, 5000);
    }
}

bool containsNul(const std::wstring& value) noexcept {
    return value.find(L'\0') != std::wstring::npos;
}

std::optional<std::filesystem::path> currentExecutableDirectory(
    std::string* error) {
    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        modulePath.data(),
        static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) {
        if (error) {
            *error =
                "failed to resolve the hydra_host.exe installation directory";
        }
        return std::nullopt;
    }

    std::filesystem::path path(
        std::wstring(modulePath.data(), length));
    const auto parent = path.parent_path();
    if (parent.empty()) {
        if (error) {
            *error =
                "hydra_host.exe has no resolvable installation directory";
        }
        return std::nullopt;
    }
    return parent;
}

struct SeatInputDevices {
    std::vector<std::uintptr_t> keyboardHandles;
    std::vector<std::uintptr_t> mouseHandles;

    bool configured() const noexcept {
        return !keyboardHandles.empty() || !mouseHandles.empty();
    }
};

std::optional<SeatInputDevices> resolveSeatInputDevices(
    runtime::RuntimeHost& host,
    std::uint32_t seatId,
    std::string* error) {
    const auto configuration = host.seatHardwareConfiguration(seatId);
    if (!configuration) {
        if (error) *error = "Seat hardware configuration is unavailable";
        return std::nullopt;
    }

    SeatInputDevices result{};
    if (configuration->keyboardId.empty() && configuration->mouseId.empty()) {
        return result;
    }

    const auto inventory = host.hardwareInventory();
    const auto resolve = [&](DeviceType type,
                             const std::wstring& persistentId,
                             const char* label)
        -> std::optional<std::vector<std::uintptr_t>> {
        if (persistentId.empty()) {
            return std::vector<std::uintptr_t>{};
        }
        const auto found = std::find_if(
            inventory.begin(), inventory.end(), [&](const DeviceInfo& device) {
                return device.type == type && device.id == persistentId &&
                       (device.nativeHandle != 0 || !device.nativeHandles.empty());
            });
        if (found == inventory.end()) {
            if (error) {
                *error = std::string("assigned ") + label +
                         " is disconnected or no longer resolves to a live Raw Input device";
            }
            return std::nullopt;
        }

        auto handles = found->nativeHandles;
        if (handles.empty() && found->nativeHandle != 0) {
            handles.push_back(found->nativeHandle);
        }
        handles.erase(
            std::remove(handles.begin(), handles.end(), std::uintptr_t{0}),
            handles.end());
        std::sort(handles.begin(), handles.end());
        handles.erase(
            std::unique(handles.begin(), handles.end()),
            handles.end());
        if (handles.empty()) {
            if (error) {
                *error = std::string("assigned ") + label +
                         " has no live Raw Input collections";
            }
            return std::nullopt;
        }
        return handles;
    };

    const auto keyboard = resolve(
        DeviceType::Keyboard, configuration->keyboardId, "keyboard");
    if (!keyboard) return std::nullopt;
    const auto mouse = resolve(
        DeviceType::Mouse, configuration->mouseId, "mouse");
    if (!mouse) return std::nullopt;

    result.keyboardHandles = *keyboard;
    result.mouseHandles = *mouse;
    return result;
}

bool keyMatches(std::wstring_view entry, std::wstring_view key) noexcept {
    const auto equals = entry.find(L'=');
    if (equals == std::wstring_view::npos || equals != key.size()) return false;
    return _wcsnicmp(entry.data(), key.data(), key.size()) == 0;
}

std::optional<std::vector<wchar_t>> xinputEnvironmentBlock(
    const controller::VirtualXInputMapping& mapping,
    const std::wstring& pipeEndpoint) {
    if (!mapping.valid() || mapping.source.sourceGeneration == 0 ||
        pipeEndpoint.empty() || containsNul(pipeEndpoint)) {
        return std::nullopt;
    }

    const std::array<std::pair<std::wstring, std::wstring>, 4> overrides{{
        {L"HYDRA_XINPUT_PIPE", pipeEndpoint},
        {L"HYDRA_XINPUT_SEAT_ID", std::to_wstring(mapping.seatId)},
        {L"HYDRA_XINPUT_ACTIVATION_GENERATION", std::to_wstring(mapping.activationGeneration)},
        {L"HYDRA_XINPUT_SOURCE_GENERATION", std::to_wstring(mapping.source.sourceGeneration)},
    }};

    LPWCH rawEnvironment = GetEnvironmentStringsW();
    if (!rawEnvironment) return std::nullopt;

    std::vector<std::wstring> entries;
    for (const wchar_t* cursor = rawEnvironment; *cursor != L'\0';) {
        std::wstring entry(cursor);
        entries.push_back(entry);
        cursor += entry.size() + 1u;
    }
    FreeEnvironmentStringsW(rawEnvironment);

    for (const auto& [key, value] : overrides) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const std::wstring& entry) {
                                         return keyMatches(entry, key);
                                     }),
                      entries.end());
        entries.push_back(key + L"=" + value);
    }

    std::sort(entries.begin(), entries.end(), [](const std::wstring& left,
                                                  const std::wstring& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    std::vector<wchar_t> block;
    std::size_t characterCount = 1u;
    for (const auto& entry : entries) characterCount += entry.size() + 1u;
    block.reserve(characterCount);
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::wstring hostXInputPipeEndpoint(
    std::uint32_t seatId,
    std::uint64_t generation) {
    return L"\\\\.\\pipe\\HydraSeat.XInput.Host." +
           std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(seatId) + L"." + std::to_wstring(generation);
}

std::optional<std::string> wideToUtf8(std::wstring_view value) {
    if (value.empty()) return std::string{};
    if (value.size() >
        static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }

    const int sourceLength = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        sourceLength,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) return std::nullopt;

    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        sourceLength,
        result.data(),
        required,
        nullptr,
        nullptr);
    if (written != required) return std::nullopt;
    return result;
}

std::optional<display::SeatDisplayGroup> resolveSeatDisplayGroup(
    runtime::RuntimeHost& host,
    std::uint32_t seatId,
    std::string* error) {
    const auto configuration = host.seatHardwareConfiguration(seatId);
    if (!configuration) {
        if (error) *error = "Seat hardware configuration is unavailable";
        return std::nullopt;
    }
    if (configuration->displayId.empty()) {
        if (error) error->clear();
        return std::nullopt;
    }

    const auto inventory = host.hardwareInventory();
    const auto found = std::find_if(
        inventory.begin(),
        inventory.end(),
        [&](const DeviceInfo& device) {
            return device.type == DeviceType::Display &&
                   device.id == configuration->displayId;
        });
    if (found == inventory.end()) {
        if (error) {
            *error = "assigned display is no longer connected";
        }
        return std::nullopt;
    }
    if (found->devicePath.empty()) {
        if (error) {
            *error = "assigned display has no live GDI device name";
        }
        return std::nullopt;
    }

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(
            found->devicePath.c_str(),
            ENUM_CURRENT_SETTINGS,
            &mode,
            0)) {
        if (error) {
            *error =
                "failed to resolve current desktop bounds for assigned display";
        }
        return std::nullopt;
    }

    const auto left = static_cast<std::int64_t>(mode.dmPosition.x);
    const auto top = static_cast<std::int64_t>(mode.dmPosition.y);
    const auto right =
        left + static_cast<std::int64_t>(mode.dmPelsWidth);
    const auto bottom =
        top + static_cast<std::int64_t>(mode.dmPelsHeight);
    constexpr auto minimum =
        static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::min)());
    constexpr auto maximum =
        static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)());
    if (mode.dmPelsWidth == 0 || mode.dmPelsHeight == 0 ||
        left < minimum || left > maximum ||
        top < minimum || top > maximum ||
        right < minimum || right > maximum ||
        bottom < minimum || bottom > maximum) {
        if (error) {
            *error = "assigned display reports invalid desktop bounds";
        }
        return std::nullopt;
    }

    const auto outputId = wideToUtf8(configuration->displayId);
    if (!outputId || outputId->empty()) {
        if (error) {
            *error = "assigned display stable ID is not valid Unicode";
        }
        return std::nullopt;
    }

    display::SeatDisplayOutput output;
    output.outputId = *outputId;
    output.globalBounds = {
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        static_cast<std::int32_t>(right),
        static_cast<std::int32_t>(bottom),
    };
    output.windowsPrimary = true;

    display::SeatDisplayGroup group;
    group.seatId = seatId;
    group.outputs.push_back(output);
    group.primaryOutputId = output.outputId;
    group.globalBounds = output.globalBounds;
    group.primaryOriginX = output.globalBounds.left;
    group.primaryOriginY = output.globalBounds.top;
    return group;
}

process::ProcessTreeSnapshot exactRootProcessTree(
    std::uint32_t seatId,
    const runtime::ProcessIdentity& identity,
    const std::wstring& executablePath) {
    process::ProcessIdentity root;
    root.processId = identity.pid;
    root.creationTime100ns = identity.creationIdentity;
    root.executablePath = executablePath;

    process::ProcessRecord rootRecord;
    rootRecord.identity = root;
    rootRecord.root = true;

    process::ProcessTreeSnapshot tree;
    tree.seatId = seatId;
    tree.capability = process::ChildTrackingCapability::RootOnly;
    tree.root = root;
    tree.processes.push_back(std::move(rootRecord));
    tree.sequence = 1;
    tree.trackingComplete = true;
    return tree;
}
#endif

} // namespace

struct GameLauncher::ControllerPipeRuntime {
#ifdef _WIN32
    runtime::RuntimeHost* host{nullptr};
    controller::NativeVirtualControllerBackend backend;
    controller::VirtualXInputService service;
    controller::NamedPipeVirtualXInputServer server;
    std::jthread worker;

    ControllerPipeRuntime(
        controller::VirtualXInputMapping mapping,
        controller::InventorySnapshot inventory,
        std::wstring endpoint,
        runtime::RuntimeHost* runtimeHost)
        : host(runtimeHost),
          service(std::move(mapping), std::move(inventory), backend),
          server(std::move(endpoint), service),
          worker([this](std::stop_token stop) {
              auto nextRefresh = std::chrono::steady_clock::now();
              while (!stop.stop_requested()) {
                  (void)server.serveOne(100);
                  if (host && std::chrono::steady_clock::now() >= nextRefresh) {
                      service.updateInventory(host->controllerInventorySnapshot());
                      nextRefresh =
                          std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(250);
                  }
              }
          }) {}
#else
    ControllerPipeRuntime(
        controller::VirtualXInputMapping,
        controller::InventorySnapshot,
        std::wstring,
        runtime::RuntimeHost*) {}
#endif
};

struct GameLauncher::SeatWindowRuntime final
    : windowing::WindowTargetObserver,
      std::enable_shared_from_this<GameLauncher::SeatWindowRuntime> {
#ifdef _WIN32
    runtime::RuntimeHost* host{nullptr};
    runtime::ActivationToken token{};
    runtime::ProcessIdentity processIdentity{};
    display::SeatDisplayGroup displayGroup;
    windowing::WindowTracker tracker;
    std::optional<windowing::WindowRestoreState> restoreState;
    std::uint64_t observerId{0};
    std::atomic<std::uintptr_t> boundHwnd{0};
    std::atomic<bool> shuttingDown{false};

    SeatWindowRuntime(
        runtime::RuntimeHost& runtimeHost,
        runtime::ActivationToken activation,
        runtime::ProcessIdentity processIdentity,
        display::SeatDisplayGroup group)
        : host(&runtimeHost),
          token(activation),
          processIdentity(processIdentity),
          displayGroup(std::move(group)) {}

    static std::shared_ptr<SeatWindowRuntime> create(
        runtime::RuntimeHost& host,
        runtime::ActivationToken token,
        runtime::ProcessIdentity processIdentity,
        process::ProcessTreeSnapshot tree,
        display::SeatDisplayGroup group,
        std::string* error) {
        auto runtime = std::shared_ptr<SeatWindowRuntime>(
            new SeatWindowRuntime(
                host,
                token,
                processIdentity,
                std::move(group)));

        windowing::WindowProfileRules rules;
        rules.defaultRole = windowing::WindowRole::PrimaryGame;
        rules.visualTargetRole = windowing::WindowRole::PrimaryGame;
        if (!runtime->tracker.setProfileRules(std::move(rules), error)) {
            return {};
        }

        runtime->tracker.setProcessTrees({std::move(tree)});
        if (!runtime->tracker.start(error)) {
            return {};
        }

        runtime->observerId = runtime->tracker.addTargetObserver(
            token.seatId,
            windowing::WindowTargetKind::Visual,
            runtime);
        if (runtime->observerId == 0) {
            if (error) {
                *error = "failed to register Seat visual-target observer";
            }
            runtime->tracker.stop();
            return {};
        }

        // The target can have resolved between tracker start and observer
        // registration. Sample once so host HWND authority never depends on a
        // future WinEvent that might not arrive.
        if (const auto current = runtime->tracker.target(
                token.seatId,
                windowing::WindowTargetKind::Visual)) {
            runtime->onWindowTargetChanged(*current);
        }
        return runtime;
    }

    bool exactProcess(
        const windowing::WindowTargetSnapshot& target) const noexcept {
        return target.window &&
               target.window->identity.process.processId ==
                   processIdentity.pid &&
               target.window->identity.process.creationTime100ns ==
                   processIdentity.creationIdentity;
    }

    void onWindowTargetChanged(
        const windowing::WindowTargetSnapshot& target) noexcept override {
        if (shuttingDown.load(std::memory_order_acquire) ||
            target.seatId != token.seatId ||
            target.kind != windowing::WindowTargetKind::Visual) {
            return;
        }

        if (target.status == windowing::WindowTargetStatus::Bound &&
            exactProcess(target)) {
            const auto hwnd = target.window->identity.nativeHandle;
            const auto previous = boundHwnd.load(std::memory_order_acquire);
            if (previous == hwnd) return;

            if (previous != 0) {
                (void)host->clearTargetWindow(
                    token, processIdentity, previous);
                boundHwnd.store(0, std::memory_order_release);
            }

            if (host->bindTargetWindow(
                    token, processIdentity, hwnd)) {
                boundHwnd.store(hwnd, std::memory_order_release);
            }
            return;
        }

        const auto previous =
            boundHwnd.exchange(0, std::memory_order_acq_rel);
        if (previous != 0) {
            (void)host->clearTargetWindow(
                token, processIdentity, previous);
        }
    }

    bool placeInitial(std::string* error) {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(10);

        std::optional<windowing::WindowTargetSnapshot> target;
        while (std::chrono::steady_clock::now() < deadline) {
            target = tracker.target(
                token.seatId,
                windowing::WindowTargetKind::Visual);
            if (target &&
                target->status == windowing::WindowTargetStatus::FailedClosed) {
                if (error) {
                    *error =
                        "window tracker failed closed because the owned visual target is ambiguous";
                }
                return false;
            }
            if (target &&
                target->status == windowing::WindowTargetStatus::Bound &&
                exactProcess(*target) &&
                tracker.validateIdentity(target->window->identity)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        if (!target ||
            target->status != windowing::WindowTargetStatus::Bound ||
            !exactProcess(*target) ||
            !tracker.validateIdentity(target->window->identity)) {
            if (error) {
                *error =
                    "timed out waiting for an authoritative window from the launched executable";
            }
            return false;
        }

        onWindowTargetChanged(*target);
        if (boundHwnd.load(std::memory_order_acquire) !=
            target->window->identity.nativeHandle) {
            if (error) {
                *error =
                    "failed to publish the exact owned target window to host authority";
            }
            return false;
        }

        windowing::WindowPlacementPolicy policy;
        policy.mode =
            windowing::WindowPlacementMode::PlaceOnPrimaryOutput;
        policy.retryCount = 2;
        policy.retryDelayMs = 75;
        policy.placementTolerancePixels = 4;
        policy.followRecreatedWindow = true;

        windowing::WindowPlacementEngine engine(tracker);
        auto result =
            engine.apply(*target->window, displayGroup, policy);
        if (result.status != windowing::WindowPlacementStatus::Applied &&
            result.status != windowing::WindowPlacementStatus::NoChange) {
            if (error) {
                if (!result.diagnostics.empty()) {
                    *error =
                        "assigned-display placement failed: " +
                        result.diagnostics.front();
                } else {
                    *error =
                        "assigned-display placement failed without diagnostics";
                }
            }
            return false;
        }

        if (result.restoreState.valid) {
            restoreState = std::move(result.restoreState);
        }
        return true;
    }

    void shutdown() noexcept {
        bool expected = false;
        if (!shuttingDown.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return;
        }

        if (observerId != 0) {
            tracker.removeTargetObserver(observerId);
            observerId = 0;
        }

        if (restoreState && restoreState->valid) {
            windowing::WindowPlacementEngine engine(tracker);
            std::string rollbackError;
            if (!engine.rollback(*restoreState, &rollbackError) &&
                !rollbackError.empty()) {
                std::cerr
                    << "[GameLauncher] window rollback warning: "
                    << rollbackError << '\n';
            }
        }
        restoreState.reset();

        const auto previous =
            boundHwnd.exchange(0, std::memory_order_acq_rel);
        if (previous != 0) {
            (void)host->clearTargetWindow(
                token, processIdentity, previous);
        }
        tracker.stop();
    }

    ~SeatWindowRuntime() override {
        shutdown();
    }
#else
    static std::shared_ptr<SeatWindowRuntime> create(
        runtime::RuntimeHost&,
        runtime::ActivationToken,
        runtime::ProcessIdentity,
        process::ProcessTreeSnapshot,
        display::SeatDisplayGroup,
        std::string*) {
        return {};
    }

    void onWindowTargetChanged(
        const windowing::WindowTargetSnapshot&) noexcept override {}
    bool placeInitial(std::string*) { return false; }
    void shutdown() noexcept {}
#endif
};

GameLauncher::~GameLauncher() {
#ifdef _WIN32
    for (std::uint32_t seatId = 1; seatId <= 2; ++seatId) {
        const auto index = seatIndex(seatId);
        if (!index || !seatProcesses_[*index]) continue;
        if (stopWorkspaceGame(seatId)) continue;

        // Destruction cannot report cleanup failure. Closing a strict Job Object
        // still kills its assigned tree, but we deliberately do not mark the
        // runtime Idle because safe-state verification did not complete.
        auto& state = *seatProcesses_[*index];
        state.inputSession.reset();
        state.windowRuntime.reset();
        state.controllerPipe.reset();
        HANDLE job = toNativeHandle(state.jobHandle);
        HANDLE process = toNativeHandle(state.processHandle);
        if (job && job != INVALID_HANDLE_VALUE) CloseHandle(job);
        if (process && process != INVALID_HANDLE_VALUE) CloseHandle(process);
        seatProcesses_[*index].reset();
    }
#endif
}

std::optional<std::size_t> GameLauncher::seatIndex(
    std::uint32_t workspaceId) noexcept {
    if (workspaceId == 1) return std::size_t{0};
    if (workspaceId == 2) return std::size_t{1};
    return std::nullopt;
}

runtime::ActivationToken GameLauncher::beginSeatActivation(
    std::uint32_t seatId) noexcept {
    if (host_) return host_->beginSeatActivation(seatId);
    if (controller_) return controller_->beginSeatActivation(seatId);
    return {};
}

bool GameLauncher::publishProcess(
    const runtime::ActivationToken& token,
    const runtime::ProcessIdentity& process) noexcept {
    if (host_) return host_->publishProcess(token, process);
    return controller_ && controller_->publishProcess(token, process);
}

bool GameLauncher::bindTargetWindow(
    const runtime::ActivationToken& token,
    const runtime::ProcessIdentity& process,
    std::uintptr_t hwnd) noexcept {
    if (host_) return host_->bindTargetWindow(token, process, hwnd);
    return controller_ &&
           controller_->bindTargetWindow(token, process, hwnd);
}

bool GameLauncher::clearTargetWindow(
    const runtime::ActivationToken& token,
    const runtime::ProcessIdentity& process,
    std::uintptr_t expectedHwnd) noexcept {
    if (host_) {
        return host_->clearTargetWindow(
            token, process, expectedHwnd);
    }
    return controller_ &&
           controller_->clearTargetWindow(
               token, process, expectedHwnd);
}

bool GameLauncher::bindController(
    const runtime::ActivationToken& token,
    const controller::SeatBinding& binding,
    const controller::InventorySnapshot& inventory) noexcept {
    if (host_) return host_->bindController(token, binding, inventory);
    return controller_ && controller_->bindController(token, binding, inventory);
}

std::optional<controller::VirtualXInputMapping>
GameLauncher::virtualXInputMapping(
    const runtime::ActivationToken& token) const noexcept {
    if (host_) return host_->virtualXInputMapping(token);
    if (controller_) return controller_->virtualXInputMapping(token);
    return std::nullopt;
}

bool GameLauncher::endSeatActivation(
    const runtime::ActivationToken& token) noexcept {
    if (host_) return host_->endSeatActivation(token);
    return controller_ && controller_->endSeatActivation(token);
}

std::string GameLauncher::lastError() const {
    std::lock_guard lock(processMutex_);
    return lastError_;
}

void GameLauncher::setLastError(std::string message) {
    std::lock_guard lock(processMutex_);
    lastError_ = std::move(message);
}

void GameLauncher::reapExitedGames() noexcept {
    // Reconciliation runs on every host request, including read-only polling.
    // Never let a long launch/stop operation make GetSnapshot wait behind the
    // GameLauncher mutex and falsely report the canonical host as unavailable.
    std::unique_lock lock(processMutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
#ifdef _WIN32
    for (std::uint32_t seatId = 1; seatId <= 2; ++seatId) {
        const auto index = seatIndex(seatId);
        if (!index || !seatProcesses_[*index]) continue;

        auto& session = *seatProcesses_[*index];
        HANDLE job = toNativeHandle(session.jobHandle);
        if (!job || job == INVALID_HANDLE_VALUE) continue;

        DWORD activeProcesses = 0;
        if (!queryActiveProcessCount(job, activeProcesses) ||
            activeProcesses != 0) {
            continue;
        }

        // The owned Job Object is already empty: this is reconciliation, not
        // termination. Tear down per-Seat helpers first while the activation
        // token is still valid, then return canonical authority to Idle.
        session.inputSession.reset();
        session.windowRuntime.reset();
        session.controllerPipe.reset();

        if (!endSeatActivation(session.token)) {
            lastError_ =
                "a naturally exited Seat process tree could not release runtime authority";
            continue;
        }

        HANDLE process = toNativeHandle(session.processHandle);
        if (process && process != INVALID_HANDLE_VALUE) {
            CloseHandle(process);
        }
        CloseHandle(job);
        seatProcesses_[*index].reset();
    }
#endif
}

bool GameLauncher::hasWorkspaceGame(std::uint32_t workspaceId) const {
    std::lock_guard lock(processMutex_);
    const auto index = seatIndex(workspaceId);
    return index && seatProcesses_[*index].has_value();
}

bool GameLauncher::launchGameForWorkspace(
    const GameProfile& game,
    const WorkspaceConfig& workspace) {
    std::lock_guard lock(processMutex_);
    lastError_.clear();
#ifdef _WIN32
    // Stable physical controller identity is persisted with the Seat; the
    // XInput slot is runtime-only and must be paired for the current session.
    if (host_) {
        const auto hardware =
            host_->seatHardwareConfiguration(workspace.workspaceId);
        const auto snapshot = host_->seatSnapshot(workspace.workspaceId);

        if (hardware && !hardware->controllerId.empty()) {
            if (!snapshot || !snapshot->controllerBinding ||
                !snapshot->controllerBinding->persistentControllerId ||
                *snapshot->controllerBinding->persistentControllerId !=
                    hardware->controllerId) {
                lastError_ =
                    "the assigned controller is not paired to a current XInput source; pair it in Seats and try again";
                return false;
            }

            auto inventory = host_->controllerInventorySnapshot();
            if (!inventory.authoritative) {
                lastError_ =
                    "controller inventory is unavailable; reconnect the controller and try again";
                return false;
            }
            if (!controller::bindingMatchesInventory(
                    *snapshot->controllerBinding, inventory)) {
                lastError_ =
                    "the paired controller disconnected or its XInput slot changed; pair it again";
                return false;
            }
            const auto endpoint = hostXInputPipeEndpoint(
                workspace.workspaceId, snapshot->generation);
            return launchGameForWorkspaceImpl(
                game,
                workspace,
                &*snapshot->controllerBinding,
                &inventory,
                &endpoint);
        }
    }
#endif
    return launchGameForWorkspaceImpl(
        game, workspace, nullptr, nullptr, nullptr);
}

bool GameLauncher::launchGameForWorkspace(
    const GameProfile& game,
    const WorkspaceConfig& workspace,
    const controller::SeatBinding& controllerBinding,
    const controller::InventorySnapshot& inventory,
    std::wstring xinputPipeEndpoint) {
    std::lock_guard lock(processMutex_);
    lastError_.clear();
    return launchGameForWorkspaceImpl(
        game, workspace, &controllerBinding, &inventory, &xinputPipeEndpoint);
}

bool GameLauncher::launchGameForWorkspaceImpl(
    const GameProfile& game,
    const WorkspaceConfig& workspace,
    const controller::SeatBinding* controllerBinding,
    const controller::InventorySnapshot* inventory,
    const std::wstring* xinputPipeEndpoint) {
#ifdef _WIN32
    const auto index = seatIndex(workspace.workspaceId);
    if (!controller_ && !host_) {
        lastError_ = "game launcher has no runtime authority owner";
        return false;
    }
    if (!index) {
        lastError_ = "only Seat 1 and Seat 2 are supported";
        return false;
    }
    if (game.executablePath.empty() || containsNul(game.executablePath) ||
        containsNul(game.launchArguments) || containsNul(game.workingDirectory)) {
        lastError_ = "launch target contains an invalid executable path or command line";
        return false;
    }
    if (seatProcesses_[*index]) {
        lastError_ = "this Seat already owns a running game process tree";
        return false;
    }

    SeatInputDevices inputDevices{};
    std::optional<display::SeatDisplayGroup> displayGroup;
    if (host_) {
        std::string inputError;
        const auto resolvedInput = resolveSeatInputDevices(
            *host_, workspace.workspaceId, &inputError);
        if (!resolvedInput) {
            lastError_ = inputError.empty()
                ? "assigned keyboard/mouse could not be resolved"
                : std::move(inputError);
            return false;
        }
        inputDevices = *resolvedInput;

        std::string displayError;
        displayGroup = resolveSeatDisplayGroup(
            *host_, workspace.workspaceId, &displayError);
        if (!displayGroup && !displayError.empty()) {
            lastError_ = displayError;
            std::cerr
                << "[GameLauncher] assigned display rejected launch: "
                << displayError << '\n';
            return false;
        }
    }

    const bool wantsXInput =
        controllerBinding != nullptr || inventory != nullptr ||
        xinputPipeEndpoint != nullptr;
    if (wantsXInput &&
        (!controllerBinding || !inventory || !xinputPipeEndpoint ||
         controllerBinding->seatId != workspace.workspaceId ||
         controllerBinding->api != controller::ApiSurface::XInput ||
         controllerBinding->sourceGeneration == 0 ||
         xinputPipeEndpoint->empty() || containsNul(*xinputPipeEndpoint))) {
        lastError_ =
            "controller launch configuration is incomplete or stale";
        return false;
    }

    const auto token = beginSeatActivation(workspace.workspaceId);
    if (!token.valid()) {
        lastError_ =
            "Seat runtime authority could not begin a new game activation";
        return false;
    }

    bool activationOwned = true;
    const auto endActivation = [&]() noexcept {
        if (activationOwned) {
            (void)endSeatActivation(token);
            activationOwned = false;
        }
    };

    std::optional<std::vector<wchar_t>> environment;
    std::shared_ptr<ControllerPipeRuntime> controllerPipe;
    DWORD creationFlags = CREATE_SUSPENDED;
    if (wantsXInput) {
        if (!bindController(token, *controllerBinding, *inventory)) {
            lastError_ =
                "the selected controller binding is no longer valid";
            endActivation();
            return false;
        }
        const auto mapping = virtualXInputMapping(token);
        if (!mapping) {
            lastError_ =
                "virtual XInput mapping could not be created for this Seat";
            endActivation();
            return false;
        }
        environment = xinputEnvironmentBlock(*mapping, *xinputPipeEndpoint);
        if (!environment) {
            lastError_ =
                "virtual XInput environment could not be constructed safely";
            endActivation();
            return false;
        }
        controllerPipe = std::make_shared<ControllerPipeRuntime>(
            *mapping, *inventory, *xinputPipeEndpoint, host_);
        creationFlags |= CREATE_UNICODE_ENVIRONMENT;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = commandLineFor(game);
    const wchar_t* workingDirectory =
        game.workingDirectory.empty() ? nullptr : game.workingDirectory.c_str();

    const BOOL created = CreateProcessW(
        game.executablePath.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        creationFlags,
        environment ? environment->data() : nullptr,
        workingDirectory,
        &startup,
        &processInfo);

    if (!created) {
        const DWORD createError = GetLastError();
        lastError_ =
            "CreateProcessW failed for the selected executable (win32=" +
            std::to_string(createError) + ")";
        controllerPipe.reset();
        endActivation();
        return false;
    }

    HANDLE job = createStrictSeatJob();
    if (!job) {
        const DWORD jobError = GetLastError();
        lastError_ =
            "strict Seat Job Object creation failed (win32=" +
            std::to_string(jobError) + ")";
        terminateCreatedProcess(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        controllerPipe.reset();
        endActivation();
        return false;
    }

    if (!AssignProcessToJobObject(job, processInfo.hProcess)) {
        const DWORD jobAssignError = GetLastError();
        lastError_ =
            "game process could not be assigned to the strict Seat Job Object "
            "(win32=" + std::to_string(jobAssignError) + ")";
        terminateCreatedProcess(processInfo.hProcess);
        CloseHandle(job);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        controllerPipe.reset();
        endActivation();
        return false;
    }

    seatProcesses_[*index] = SeatProcess{
        fromNativeHandle(processInfo.hProcess),
        fromNativeHandle(job),
        token,
        {},
        std::move(controllerPipe),
        {},
        inputDevices.keyboardHandles,
        inputDevices.mouseHandles,
        {}};
    activationOwned = false;

    const auto rollback = [&]() {
        if (processInfo.hThread) {
            CloseHandle(processInfo.hThread);
            processInfo.hThread = nullptr;
        }
        (void)stopWorkspaceGame(workspace.workspaceId);
    };

    const auto identity =
        readProcessIdentity(processInfo.hProcess, processInfo.dwProcessId);
    if (!identity.valid()) {
        lastError_ =
            "the launched process identity could not be verified";
        rollback();
        return false;
    }

    seatProcesses_[*index]->identity = identity;
    if (!publishProcess(token, identity)) {
        lastError_ =
            "canonical Seat authority rejected the launched process identity";
        rollback();
        return false;
    }

    if (displayGroup) {
        std::string windowError;
        auto windowRuntime = SeatWindowRuntime::create(
            *host_,
            token,
            identity,
            exactRootProcessTree(
                workspace.workspaceId,
                identity,
                game.executablePath),
            std::move(*displayGroup),
            &windowError);
        if (!windowRuntime) {
            lastError_ = windowError.empty()
                ? "display/window ownership tracker could not be established"
                : windowError;
            std::cerr
                << "[GameLauncher] display/window tracker setup failed: "
                << lastError_ << '\n';
            rollback();
            return false;
        }
        seatProcesses_[*index]->windowRuntime =
            std::move(windowRuntime);
    }

    // Establish every process-local input boundary while the target's
    // primary thread is still suspended. Resuming first creates a bleed window:
    // keyboard/mouse can observe system Raw Input and an XInput title can bind
    // directly to the machine-wide controller slots before HydraSeat redirects
    // its reviewed static imports.
    if (inputDevices.configured() || wantsXInput) {
        std::string inputError;
        const auto artifactDirectory = currentExecutableDirectory(&inputError);
        if (!artifactDirectory) {
            lastError_ = inputError.empty()
                ? "Gate C runtime directory could not be resolved"
                : std::move(inputError);
            rollback();
            return false;
        }

        gatec::ExternalInputSessionOptions options{};
        options.seatId = workspace.workspaceId;
        options.processHandle = fromNativeHandle(processInfo.hProcess);
        options.processId = processInfo.dwProcessId;
        options.artifactDirectory = *artifactDirectory;
        options.enableXInputRedirect = wantsXInput;

        auto inputSession =
            gatec::ExternalInputSession::attach(options, &inputError);
        if (!inputSession) {
            lastError_ = inputError.empty()
                ? (wantsXInput
                       ? "process-local input/XInput isolation could not be established"
                       : "process-local keyboard/mouse isolation could not be established")
                : std::move(inputError);
            rollback();
            return false;
        }
        seatProcesses_[*index]->inputSession = std::move(inputSession);
    }

    if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        const DWORD resumeError = GetLastError();
        lastError_ =
            "the launched process could not be resumed safely (win32=" +
            std::to_string(resumeError) + ")";
        rollback();
        return false;
    }

    CloseHandle(processInfo.hThread);
    processInfo.hThread = nullptr;

    if (seatProcesses_[*index]->windowRuntime) {
        std::string placementError;
        if (!seatProcesses_[*index]->windowRuntime->placeInitial(
                &placementError)) {
            lastError_ = placementError.empty()
                ? "assigned display placement failed"
                : placementError;
            std::cerr
                << "[GameLauncher] assigned display placement failed: "
                << placementError << '\n';
            rollback();
            return false;
        }
    }

    std::wcout << L"[GameLauncher] Started " << game.title
               << L" for Seat #" << workspace.workspaceId
               << L" (PID: " << processInfo.dwProcessId << L")\n";
    return true;
#else
    (void)game;
    (void)workspace;
    (void)controllerBinding;
    (void)inventory;
    (void)xinputPipeEndpoint;
    return false;
#endif
}

bool GameLauncher::routePhysicalInput(const RawInputEvent& event) {
#ifdef _WIN32
    std::lock_guard lock(processMutex_);
    if (event.deviceHandle == 0) return false;

    const bool keyboardEvent = event.rawDevType == RIM_TYPEKEYBOARD;
    const bool mouseEvent = event.rawDevType == RIM_TYPEMOUSE;
    if (!keyboardEvent && !mouseEvent) return false;

    SeatProcess* target = nullptr;
    for (auto& candidate : seatProcesses_) {
        if (!candidate || !candidate->inputSession ||
            !candidate->inputSession->active()) {
            continue;
        }
        const auto containsHandle = [&](const auto& handles) {
            return std::find(
                       handles.begin(),
                       handles.end(),
                       event.deviceHandle) != handles.end();
        };
        const bool matches =
            (keyboardEvent && containsHandle(candidate->keyboardHandles)) ||
            (mouseEvent && containsHandle(candidate->mouseHandles));
        if (!matches) continue;
        if (target != nullptr) {
            lastError_ = "one physical input device resolved to more than one active Seat";
            return false;
        }
        target = &*candidate;
    }
    if (target == nullptr) return false;

    gatec::InputEventMessage message{};
    message.timestampMicros = event.timestampMicros;
    message.isTouchpad = event.isTouchpad;
    if (keyboardEvent) {
        message.kind = gatec::InputKind::Keyboard;
        message.vkey = event.vkey;
        message.scanCode = event.scanCode;
        message.keyboardFlags = event.keyboardFlags;
        switch (event.messageType) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            message.keyTransition = gatec::KeyTransition::Down;
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            message.keyTransition = gatec::KeyTransition::Up;
            break;
        default:
            return false;
        }
    } else {
        message.kind = gatec::InputKind::Mouse;
        message.deltaX = event.deltaX;
        message.deltaY = event.deltaY;
        message.mouseButtonFlags = event.mouseButtonFlags;
        message.wheelDelta = event.wheelDelta;
    }

    std::string error;
    if (!target->inputSession->sendInput(message, &error)) {
        const std::uint32_t failedSeatId = target->token.seatId;
        const std::string deliveryError = error.empty()
            ? "process-local input delivery failed"
            : "process-local input delivery failed: " + error;

        // Once the process-local bridge cannot accept Seat input, continuing the
        // owned process tree would be fail-open: a launcher-handoff child could
        // outlive the injected root with no proven input isolation. The launcher
        // mutex is recursive by design, so stop the exact Seat Job synchronously.
        const bool stopped = stopWorkspaceGame(failedSeatId);
        lastError_ = stopped
            ? deliveryError + "; Seat process tree terminated fail closed"
            : deliveryError + "; Seat process tree termination also failed";
        return false;
    }
    return true;
#else
    (void)event;
    return false;
#endif
}

bool GameLauncher::stopWorkspaceGame(std::uint32_t workspaceId) {
    std::lock_guard lock(processMutex_);
#ifdef _WIN32
    const auto index = seatIndex(workspaceId);
    if ((!controller_ && !host_) || !index || !seatProcesses_[*index]) {
        return false;
    }

    auto& session = *seatProcesses_[*index];
    HANDLE process = toNativeHandle(session.processHandle);
    HANDLE job = toNativeHandle(session.jobHandle);
    if (!process || process == INVALID_HANDLE_VALUE ||
        !job || job == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Keep Seat-local isolation/tracking alive until the exact owned process
    // tree is confirmed stopped. If termination or verification fails, returning
    // with the game still running must not silently drop keyboard/mouse isolation
    // or window ownership.
    DWORD activeProcesses = 0;
    if (!queryActiveProcessCount(job, activeProcesses)) return false;
    if (activeProcesses != 0 && !TerminateJobObject(job, ERROR_CANCELLED)) {
        return false;
    }

    if (!waitForJobEmpty(job, 5000)) return false;
    if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) return false;

    session.inputSession.reset();
    session.windowRuntime.reset();
    session.controllerPipe.reset();
    if (!endSeatActivation(session.token)) return false;

    CloseHandle(process);
    CloseHandle(job);
    seatProcesses_[*index].reset();
    return true;
#else
    (void)workspaceId;
    return false;
#endif
}

} // namespace hydra
