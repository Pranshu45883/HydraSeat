#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/virtual_xinput_pipe.hpp"

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

hydra::controller::InventorySnapshot makePipeInventory() {
    hydra::controller::InventorySnapshot inventory;
    inventory.authoritative = true;
    hydra::controller::SourceDescriptor source;
    source.runtimeKey = "pipe-source";
    source.api = hydra::controller::ApiSurface::XInput;
    source.identityQuality = hydra::controller::IdentityQuality::RuntimeOnly;
    source.runtimeXInputSlot = std::uint8_t{0};
    source.connected = true;
    source.sourceGeneration = 1;
    inventory.sources.push_back(source);
    return inventory;
}

hydra::controller::VirtualXInputMapping makePipeMapping() {
    hydra::controller::SeatBinding binding;
    binding.seatId = 1;
    binding.api = hydra::controller::ApiSurface::XInput;
    binding.runtimeKey = "pipe-source";
    binding.runtimeXInputSlot = std::uint8_t{0};
    binding.sourceGeneration = 1;
    return {1, 1, binding};
}

struct PipeBackend final : hydra::controller::IVirtualControllerBackend {
    std::size_t mutations{0};

    hydra::controller::PollResult poll(
        const hydra::controller::SeatBinding&,
        const hydra::controller::InventorySnapshot&) noexcept override {
        hydra::controller::GamepadState state;
        state.buttons = 0x0040;
        return {hydra::controller::IoStatus::Ok, state};
    }

    hydra::controller::IoStatus vibrate(
        const hydra::controller::SeatBinding&,
        const hydra::controller::InventorySnapshot&,
        std::uint16_t,
        std::uint16_t) noexcept override {
        ++mutations;
        return hydra::controller::IoStatus::Ok;
    }
};

#if defined(_WIN32)
std::wstring uniquePipeName(const wchar_t* suffix) {
    return L"\\\\.\\pipe\\hydraseat-xinput-test-" +
           std::to_wstring(GetCurrentProcessId()) + L"-" + suffix;
}

std::optional<hydra::controller::ipc::VirtualXInputResponse> sendMalformedVersion(
    const std::wstring& endpoint) {
    using namespace hydra::controller::ipc;

    const ULONGLONG start = GetTickCount64();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;) {
        pipe = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;

        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
            return std::nullopt;
        }
        if (GetTickCount64() - start >= 2000) {
            return std::nullopt;
        }
        Sleep(1);
    }

    std::vector<std::uint8_t> bytes(kEncodedRequestSize, 0);
    DWORD written = 0;
    const BOOL writeOk = WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()),
                                   &written, nullptr);
    std::vector<std::uint8_t> responseBytes(kEncodedResponseSize, 0);
    DWORD read = 0;
    const BOOL readOk = writeOk && written == bytes.size() &&
        ReadFile(pipe, responseBytes.data(), static_cast<DWORD>(responseBytes.size()),
                 &read, nullptr);
    CloseHandle(pipe);
    if (!readOk || read != responseBytes.size()) return std::nullopt;
    return decodeResponse(responseBytes);
}
#endif

} // namespace

void testVirtualXInputPipe() {
    using namespace hydra::controller;
    using namespace hydra::controller::ipc;

    PipeBackend backend;
    VirtualXInputService service(makePipeMapping(), makePipeInventory(), backend);

#if defined(_WIN32)
    const auto pingPipe = uniquePipeName(L"ping");
    NamedPipeVirtualXInputServer pingServer(pingPipe, service);
    PipeServerResult pingServerResult;
    std::thread pingThread([&] { pingServerResult = pingServer.serveOne(2000); });

    VirtualXInputRequest ping;
    ping.opcode = ProtocolOpcode::Ping;
    const auto pingResponse = sendVirtualXInputRequest(pingPipe, ping, 2000);
    pingThread.join();
    assert(pingResponse.has_value());
    assert(pingResponse->status == ProtocolStatus::Ok);
    assert(pingServerResult.served);
    assert(pingServerResult.status == ProtocolStatus::Ok);

    const auto malformedPipe = uniquePipeName(L"malformed");
    NamedPipeVirtualXInputServer malformedServer(malformedPipe, service);
    PipeServerResult malformedServerResult;
    std::thread malformedThread(
        [&] { malformedServerResult = malformedServer.serveOne(2000); });
    const auto malformedResponse = sendMalformedVersion(malformedPipe);
    malformedThread.join();
    assert(malformedResponse.has_value());
    assert(malformedResponse->status == ProtocolStatus::InvalidRequest);
    assert(malformedServerResult.served);
    assert(malformedServerResult.status == ProtocolStatus::InvalidRequest);
    assert(backend.mutations == 0);

    const auto missingPipe = uniquePipeName(L"missing");
    assert(!sendVirtualXInputRequest(missingPipe, ping, 20).has_value());
#else
    VirtualXInputRequest ping;
    ping.opcode = ProtocolOpcode::Ping;
    assert(!sendVirtualXInputRequest(L"unsupported", ping, 20).has_value());
#endif
}
