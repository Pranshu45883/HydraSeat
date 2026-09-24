#include "hydra/virtual_xinput_pipe.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)

using hydra::controller::ApiSurface;
using hydra::controller::GamepadState;
using hydra::controller::IVirtualControllerBackend;
using hydra::controller::IdentityQuality;
using hydra::controller::InventorySnapshot;
using hydra::controller::IoStatus;
using hydra::controller::NamedPipeVirtualXInputServer;
using hydra::controller::PollResult;
using hydra::controller::SeatBinding;
using hydra::controller::SourceDescriptor;
using hydra::controller::VirtualXInputMapping;
using hydra::controller::VirtualXInputService;

constexpr std::uint32_t kPipeTimeoutMs = 3000;
constexpr std::uint32_t kProcessTimeoutMs = 5000;
constexpr std::uint32_t kSeat1 = 1;
constexpr std::uint32_t kSeat2 = 2;
constexpr std::uint64_t kSeat1Activation = 101;
constexpr std::uint64_t kSeat2Activation = 201;
constexpr std::uint64_t kSeat1RestartedActivation = 102;
constexpr std::uint64_t kSeat1SourceGeneration = 11;
constexpr std::uint64_t kSeat2SourceGeneration = 21;
constexpr char kSeat1Key[] = "synthetic-seat-a";
constexpr char kSeat2Key[] = "synthetic-seat-b";

class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (valid()) CloseHandle(handle_);
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            if (valid()) CloseHandle(handle_);
            handle_ = other.release();
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept {
        const HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }
    void reset(HANDLE handle = nullptr) noexcept {
        if (valid()) CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_{nullptr};
};

struct ProbeRunResult {
    bool launched{false};
    bool timedOut{false};
    DWORD processId{0};
    DWORD exitCode{static_cast<DWORD>(-1)};
    std::string output;
};

struct VibrationReceipt {
    std::size_t calls{0};
    std::uint16_t low{0};
    std::uint16_t high{0};

    bool operator==(const VibrationReceipt&) const = default;
};

struct SyntheticBackend final : IVirtualControllerBackend {
    std::mutex mutex;
    std::map<std::string, GamepadState> states;
    std::map<std::string, VibrationReceipt> vibration;
    std::string blockedPollKey;
    HANDLE pollEnteredEvent{nullptr};
    HANDLE pollReleaseEvent{nullptr};

    PollResult poll(const SeatBinding& binding,
                    const InventorySnapshot&) noexcept override {
        if (!blockedPollKey.empty() && binding.runtimeKey == blockedPollKey) {
            if (pollEnteredEvent != nullptr) SetEvent(pollEnteredEvent);
            if (pollReleaseEvent == nullptr ||
                WaitForSingleObject(pollReleaseEvent, kProcessTimeoutMs) != WAIT_OBJECT_0) {
                return {IoStatus::NativeFailure, std::nullopt};
            }
        }

        std::lock_guard lock(mutex);
        const auto it = states.find(binding.runtimeKey);
        if (it == states.end()) return {IoStatus::InvalidBinding, std::nullopt};
        return {IoStatus::Ok, it->second};
    }

    IoStatus vibrate(const SeatBinding& binding,
                     const InventorySnapshot&,
                     std::uint16_t lowFrequencyMotor,
                     std::uint16_t highFrequencyMotor) noexcept override {
        std::lock_guard lock(mutex);
        auto& receipt = vibration[binding.runtimeKey];
        ++receipt.calls;
        receipt.low = lowFrequencyMotor;
        receipt.high = highFrequencyMotor;
        return IoStatus::Ok;
    }

    VibrationReceipt vibrationFor(const std::string& key) {
        std::lock_guard lock(mutex);
        const auto it = vibration.find(key);
        return it == vibration.end() ? VibrationReceipt{} : it->second;
    }
};

InventorySnapshot makeInventory(std::uint32_t seatId,
                                std::uint64_t sourceGeneration,
                                const std::string& runtimeKey) {
    InventorySnapshot inventory;
    inventory.authoritative = true;

    SourceDescriptor source;
    source.runtimeKey = runtimeKey;
    source.api = ApiSurface::XInput;
    source.identityQuality = IdentityQuality::RuntimeOnly;
    source.runtimeXInputSlot = static_cast<std::uint8_t>(seatId - 1);
    source.connected = true;
    source.sourceGeneration = sourceGeneration;
    inventory.sources.push_back(source);
    return inventory;
}

VirtualXInputMapping makeMapping(std::uint32_t seatId,
                                 std::uint64_t activationGeneration,
                                 std::uint64_t sourceGeneration,
                                 const std::string& runtimeKey) {
    SeatBinding binding;
    binding.seatId = seatId;
    binding.api = ApiSurface::XInput;
    binding.runtimeKey = runtimeKey;
    binding.runtimeXInputSlot = static_cast<std::uint8_t>(seatId - 1);
    binding.sourceGeneration = sourceGeneration;
    return {seatId, activationGeneration, binding};
}

std::wstring uniqueEndpoint(const wchar_t* suffix) {
    return L"\\\\.\\pipe\\hydraseat-xinput-process-" +
           std::to_wstring(GetCurrentProcessId()) + L"-" + suffix;
}

std::wstring quoteArgument(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::string readAll(HANDLE handle) {
    std::string output;
    char buffer[512];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            break;
        }
        output.append(buffer, buffer + read);
    }
    return output;
}

ProbeRunResult runProbe(const std::wstring& probePath,
                        const std::wstring& endpoint,
                        std::uint32_t seatId,
                        std::uint64_t activationGeneration,
                        std::uint64_t sourceGeneration,
                        const wchar_t* mode,
                        std::uint16_t low = 0,
                        std::uint16_t high = 0) {
    ProbeRunResult result;

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readRaw = nullptr;
    HANDLE writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &security, 0)) return result;
    ScopedHandle readHandle(readRaw);
    ScopedHandle writeHandle(writeRaw);
    if (!SetHandleInformation(readHandle.get(), HANDLE_FLAG_INHERIT, 0)) return result;

    ScopedHandle nullInput(CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nullInput.valid()) return result;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput.get();
    startup.hStdOutput = writeHandle.get();
    startup.hStdError = writeHandle.get();

    PROCESS_INFORMATION process{};
    std::wstring commandLine =
        quoteArgument(probePath) +
        L" --pipe " + quoteArgument(endpoint) +
        L" --seat " + std::to_wstring(seatId) +
        L" --activation-generation " + std::to_wstring(activationGeneration) +
        L" --source-generation " + std::to_wstring(sourceGeneration) +
        L" --mode " + mode;
    if (std::wstring(mode) == L"vibrate") {
        commandLine += L" --low " + std::to_wstring(low) +
                       L" --high " + std::to_wstring(high);
    }

    if (!CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        return result;
    }

    result.launched = true;
    result.processId = process.dwProcessId;
    ScopedHandle processHandle(process.hProcess);
    ScopedHandle threadHandle(process.hThread);
    writeHandle.reset();

    const DWORD wait = WaitForSingleObject(processHandle.get(), kProcessTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        result.timedOut = true;
        TerminateProcess(processHandle.get(), 120);
        WaitForSingleObject(processHandle.get(), 1000);
    }

    result.output = readAll(readHandle.get());
    DWORD exitCode = static_cast<DWORD>(-1);
    if (GetExitCodeProcess(processHandle.get(), &exitCode)) {
        result.exitCode = exitCode;
    }
    return result;
}

struct ServerRunResult {
    bool ok{true};
    std::size_t served{0};
};

void serveRequests(NamedPipeVirtualXInputServer& server,
                   std::size_t requestCount,
                   ServerRunResult& result) {
    for (std::size_t request = 0; request < requestCount; ++request) {
        const auto served = server.serveOne(kPipeTimeoutMs);
        if (!served.served) {
            result.ok = false;
            return;
        }
        ++result.served;
    }
}

bool snapshotShowsOnly(const ProbeRunResult& probe,
                       std::uint16_t expectedButtons,
                       std::int16_t expectedLX,
                       std::uint16_t forbiddenButtons,
                       std::int16_t forbiddenLX) {
    if (!probe.launched || probe.timedOut || probe.exitCode != 0) return false;
    const std::string expected =
        "slot=0 status=Ok buttons=" + std::to_string(expectedButtons) +
        " lx=" + std::to_string(expectedLX);
    const std::string forbiddenButtonsText =
        "buttons=" + std::to_string(forbiddenButtons);
    const std::string forbiddenLXText = "lx=" + std::to_string(forbiddenLX);
    return probe.output.find(expected) != std::string::npos &&
           probe.output.find("slot=1 status=Disconnected") != std::string::npos &&
           probe.output.find("slot=2 status=Disconnected") != std::string::npos &&
           probe.output.find("slot=3 status=Disconnected") != std::string::npos &&
           probe.output.find(forbiddenButtonsText) == std::string::npos &&
           probe.output.find(forbiddenLXText) == std::string::npos;
}

int fail(const char* message, int code) {
    std::cerr << message << '\n';
    return code;
}

#endif

} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS |
                 SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);

    if (argc < 2) return fail("probe executable argument missing", 2);
    const std::wstring probePath = std::filesystem::path(argv[1]).wstring();

    SyntheticBackend backend;
    GamepadState seat1State;
    seat1State.packetNumber = 7;
    seat1State.buttons = 64;
    seat1State.thumbLX = 1234;
    GamepadState seat2State;
    seat2State.packetNumber = 9;
    seat2State.buttons = 128;
    seat2State.thumbLX = -2345;
    backend.states.emplace(kSeat1Key, seat1State);
    backend.states.emplace(kSeat2Key, seat2State);

    auto seat1Inventory = makeInventory(kSeat1, kSeat1SourceGeneration, kSeat1Key);
    auto seat2Inventory = makeInventory(kSeat2, kSeat2SourceGeneration, kSeat2Key);
    auto seat1Mapping = makeMapping(
        kSeat1, kSeat1Activation, kSeat1SourceGeneration, kSeat1Key);
    auto seat2Mapping = makeMapping(
        kSeat2, kSeat2Activation, kSeat2SourceGeneration, kSeat2Key);
    VirtualXInputService seat1Service(seat1Mapping, seat1Inventory, backend);
    VirtualXInputService seat2Service(seat2Mapping, seat2Inventory, backend);

    const auto seat1Endpoint = uniqueEndpoint(L"seat-a-snapshot");
    const auto seat2Endpoint = uniqueEndpoint(L"seat-b-snapshot");
    NamedPipeVirtualXInputServer seat1Server(seat1Endpoint, seat1Service);
    NamedPipeVirtualXInputServer seat2Server(seat2Endpoint, seat2Service);
    ServerRunResult seat1ServerResult;
    ServerRunResult seat2ServerResult;
    ProbeRunResult seat1Probe;
    ProbeRunResult seat2Probe;

    std::thread seat1ServerThread(
        [&] { serveRequests(seat1Server, 4, seat1ServerResult); });
    std::thread seat2ServerThread(
        [&] { serveRequests(seat2Server, 4, seat2ServerResult); });
    std::thread seat1ProbeThread([&] {
        seat1Probe = runProbe(
            probePath, seat1Endpoint, kSeat1, kSeat1Activation,
            kSeat1SourceGeneration, L"snapshot");
    });
    std::thread seat2ProbeThread([&] {
        seat2Probe = runProbe(
            probePath, seat2Endpoint, kSeat2, kSeat2Activation,
            kSeat2SourceGeneration, L"snapshot");
    });

    seat1ProbeThread.join();
    seat2ProbeThread.join();
    seat1ServerThread.join();
    seat2ServerThread.join();

    if (!seat1ServerResult.ok || seat1ServerResult.served != 4) {
        return fail("Seat 1 snapshot server did not serve four requests", 20);
    }
    if (!seat2ServerResult.ok || seat2ServerResult.served != 4) {
        return fail("Seat 2 snapshot server did not serve four requests", 21);
    }
    if (seat1Probe.processId == 0 || seat2Probe.processId == 0 ||
        seat1Probe.processId == seat2Probe.processId) {
        return fail("probe processes were not distinct", 22);
    }
    if (!snapshotShowsOnly(seat1Probe, 64, 1234, 128, -2345)) {
        return fail("Game A observed the wrong logical controller namespace", 23);
    }
    if (!snapshotShowsOnly(seat2Probe, 128, -2345, 64, 1234)) {
        return fail("Game B observed the wrong logical controller namespace", 24);
    }

    const auto seat1VibrationEndpoint = uniqueEndpoint(L"seat-a-vibration");
    const auto seat2VibrationEndpoint = uniqueEndpoint(L"seat-b-vibration");
    NamedPipeVirtualXInputServer seat1VibrationServer(
        seat1VibrationEndpoint, seat1Service);
    NamedPipeVirtualXInputServer seat2VibrationServer(
        seat2VibrationEndpoint, seat2Service);
    ServerRunResult seat1VibrationServerResult;
    ServerRunResult seat2VibrationServerResult;
    std::thread seat1VibrationServerThread([&] {
        serveRequests(seat1VibrationServer, 1, seat1VibrationServerResult);
    });
    std::thread seat2VibrationServerThread([&] {
        serveRequests(seat2VibrationServer, 1, seat2VibrationServerResult);
    });
    const auto seat1VibrationProbe = runProbe(
        probePath, seat1VibrationEndpoint, kSeat1, kSeat1Activation,
        kSeat1SourceGeneration, L"vibrate", 111, 222);
    const auto seat2VibrationProbe = runProbe(
        probePath, seat2VibrationEndpoint, kSeat2, kSeat2Activation,
        kSeat2SourceGeneration, L"vibrate", 333, 444);
    seat1VibrationServerThread.join();
    seat2VibrationServerThread.join();

    if (!seat1VibrationServerResult.ok || !seat2VibrationServerResult.ok ||
        seat1VibrationProbe.exitCode != 0 || seat2VibrationProbe.exitCode != 0) {
        return fail("vibration probes failed", 30);
    }
    const auto seat1Receipt = backend.vibrationFor(kSeat1Key);
    const auto seat2Receipt = backend.vibrationFor(kSeat2Key);
    if (seat1Receipt.calls != 1 || seat1Receipt.low != 111 || seat1Receipt.high != 222) {
        return fail("Game A vibration did not route only to Seat 1", 31);
    }
    if (seat2Receipt.calls != 1 || seat2Receipt.low != 333 || seat2Receipt.high != 444) {
        return fail("Game B vibration did not route only to Seat 2", 32);
    }

    const auto staleSourceEndpoint = uniqueEndpoint(L"seat-a-stale-source");
    NamedPipeVirtualXInputServer staleSourceServer(staleSourceEndpoint, seat1Service);
    ServerRunResult staleSourceServerResult;
    std::thread staleSourceServerThread([&] {
        serveRequests(staleSourceServer, 1, staleSourceServerResult);
    });
    const auto staleSourceProbe = runProbe(
        probePath, staleSourceEndpoint, kSeat1, kSeat1Activation,
        kSeat1SourceGeneration - 1, L"vibrate", 900, 901);
    staleSourceServerThread.join();
    if (!staleSourceServerResult.ok || staleSourceProbe.exitCode == 0) {
        return fail("stale source generation was not rejected", 33);
    }
    if (backend.vibrationFor(kSeat1Key) != seat1Receipt) {
        return fail("stale source request mutated Seat 1 vibration", 34);
    }

    const auto staleActivationEndpoint = uniqueEndpoint(L"seat-a-stale-activation");
    NamedPipeVirtualXInputServer staleActivationServer(
        staleActivationEndpoint, seat1Service);
    ServerRunResult staleActivationServerResult;
    std::thread staleActivationServerThread([&] {
        serveRequests(staleActivationServer, 1, staleActivationServerResult);
    });
    const auto staleActivationProbe = runProbe(
        probePath, staleActivationEndpoint, kSeat1, kSeat1Activation - 1,
        kSeat1SourceGeneration, L"vibrate", 902, 903);
    staleActivationServerThread.join();
    if (!staleActivationServerResult.ok || staleActivationProbe.exitCode == 0) {
        return fail("stale activation generation was not rejected", 35);
    }
    if (backend.vibrationFor(kSeat1Key) != seat1Receipt) {
        return fail("stale activation request mutated Seat 1 vibration", 36);
    }

    ScopedHandle seat2PollEntered(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ScopedHandle seat2PollRelease(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!seat2PollEntered.valid() || !seat2PollRelease.valid()) {
        return fail("failed to create Seat 2 hold events", 40);
    }
    backend.blockedPollKey = kSeat2Key;
    backend.pollEnteredEvent = seat2PollEntered.get();
    backend.pollReleaseEvent = seat2PollRelease.get();

    const auto heldSeat2Endpoint = uniqueEndpoint(L"seat-b-held");
    NamedPipeVirtualXInputServer heldSeat2Server(heldSeat2Endpoint, seat2Service);
    ServerRunResult heldSeat2ServerResult;
    ProbeRunResult heldSeat2Probe;
    std::thread heldSeat2ServerThread([&] {
        serveRequests(heldSeat2Server, 4, heldSeat2ServerResult);
    });
    std::thread heldSeat2ProbeThread([&] {
        heldSeat2Probe = runProbe(
            probePath, heldSeat2Endpoint, kSeat2, kSeat2Activation,
            kSeat2SourceGeneration, L"snapshot");
    });

    if (WaitForSingleObject(seat2PollEntered.get(), kPipeTimeoutMs) != WAIT_OBJECT_0) {
        SetEvent(seat2PollRelease.get());
        heldSeat2ProbeThread.join();
        heldSeat2ServerThread.join();
        return fail("Seat 2 child never reached its held poll", 41);
    }

    auto restartedSeat1Mapping = makeMapping(
        kSeat1, kSeat1RestartedActivation, kSeat1SourceGeneration, kSeat1Key);
    VirtualXInputService restartedSeat1Service(
        restartedSeat1Mapping, seat1Inventory, backend);

    const auto oldActivationEndpoint = uniqueEndpoint(L"seat-a-old-after-restart");
    NamedPipeVirtualXInputServer oldActivationServer(
        oldActivationEndpoint, restartedSeat1Service);
    ServerRunResult oldActivationServerResult;
    std::thread oldActivationServerThread([&] {
        serveRequests(oldActivationServer, 1, oldActivationServerResult);
    });
    const auto oldActivationProbe = runProbe(
        probePath, oldActivationEndpoint, kSeat1, kSeat1Activation,
        kSeat1SourceGeneration, L"snapshot");
    oldActivationServerThread.join();
    if (!oldActivationServerResult.ok || oldActivationProbe.exitCode == 0) {
        SetEvent(seat2PollRelease.get());
        heldSeat2ProbeThread.join();
        heldSeat2ServerThread.join();
        return fail("old Seat 1 activation survived restart", 42);
    }

    const auto restartedSeat1Endpoint = uniqueEndpoint(L"seat-a-restarted");
    NamedPipeVirtualXInputServer restartedSeat1Server(
        restartedSeat1Endpoint, restartedSeat1Service);
    ServerRunResult restartedSeat1ServerResult;
    std::thread restartedSeat1ServerThread([&] {
        serveRequests(restartedSeat1Server, 4, restartedSeat1ServerResult);
    });
    const auto restartedSeat1Probe = runProbe(
        probePath, restartedSeat1Endpoint, kSeat1, kSeat1RestartedActivation,
        kSeat1SourceGeneration, L"snapshot");
    restartedSeat1ServerThread.join();
    if (!restartedSeat1ServerResult.ok ||
        !snapshotShowsOnly(restartedSeat1Probe, 64, 1234, 128, -2345)) {
        std::cerr << "restarted Seat 1 diagnostics: served="
                  << restartedSeat1ServerResult.served
                  << " serverOk=" << restartedSeat1ServerResult.ok
                  << " launched=" << restartedSeat1Probe.launched
                  << " timedOut=" << restartedSeat1Probe.timedOut
                  << " pid=" << restartedSeat1Probe.processId
                  << " exit=" << restartedSeat1Probe.exitCode
                  << " output=[" << restartedSeat1Probe.output << "]\n";
        SetEvent(seat2PollRelease.get());
        heldSeat2ProbeThread.join();
        heldSeat2ServerThread.join();
        return fail("restarted Game A did not receive the new Seat 1 mapping", 43);
    }

    SetEvent(seat2PollRelease.get());
    heldSeat2ProbeThread.join();
    heldSeat2ServerThread.join();
    backend.blockedPollKey.clear();
    backend.pollEnteredEvent = nullptr;
    backend.pollReleaseEvent = nullptr;

    if (!heldSeat2ServerResult.ok ||
        !snapshotShowsOnly(heldSeat2Probe, 128, -2345, 64, 1234)) {
        std::cerr << "held Seat 2 diagnostics: served=" << heldSeat2ServerResult.served
                  << " serverOk=" << heldSeat2ServerResult.ok
                  << " launched=" << heldSeat2Probe.launched
                  << " timedOut=" << heldSeat2Probe.timedOut
                  << " pid=" << heldSeat2Probe.processId
                  << " exit=" << heldSeat2Probe.exitCode
                  << " output=[" << heldSeat2Probe.output << "]\n";
        return fail("Game B was disturbed by Seat 1 restart", 44);
    }

    std::cout << "XInput process isolation harness passed\n";
    return 0;
#else
    (void)argc;
    (void)argv;
    return 0;
#endif
}
