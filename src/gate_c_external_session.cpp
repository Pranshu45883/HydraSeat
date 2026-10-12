#include "hydra/gate_c_external_session.hpp"

#include "hydra/gate_c_architecture.hpp"
#include "hydra/gate_c_external_profile.hpp"
#include "hydra/gate_c_transport.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace hydra::gatec {
namespace {

constexpr std::uint32_t kHandshakeTimeoutMs = 10000u;
constexpr std::uint32_t kIoTimeoutMs = 1000u;

void setError(std::string* output, std::string message) {
    if (output) *output = std::move(message);
}

#if defined(_WIN32)

bool fileExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

bool writeBridgeMapping(
    HANDLE& mapping,
    std::uint32_t seatId,
    std::uint32_t processId,
    const SessionToken& runtimeSessionId,
    const std::wstring& pipeName,
    std::uint32_t requiredApiMask,
    std::string* error) {
    if (pipeName.empty() ||
        pipeName.size() >= kExternalBridgePipeNameChars ||
        !validProfiledShimMask(requiredApiMask)) {
        setError(error, "Gate C bridge configuration is invalid");
        return false;
    }

    const auto mappingName = externalBridgeMappingName(processId);
    mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(ExternalBridgeConfigV1)),
        mappingName.c_str());
    if (mapping == nullptr) {
        setError(
            error,
            "failed to create Gate C bridge mapping (win32=" +
                std::to_string(GetLastError()) + ")");
        return false;
    }

    void* view = MapViewOfFile(
        mapping,
        FILE_MAP_WRITE,
        0,
        0,
        sizeof(ExternalBridgeConfigV1));
    if (view == nullptr) {
        setError(
            error,
            "failed to map Gate C bridge configuration (win32=" +
                std::to_string(GetLastError()) + ")");
        return false;
    }

    ExternalBridgeConfigV1 config{};
    config.seatId = seatId;
    config.requiredApiMask = requiredApiMask;
    config.token = runtimeSessionId;
    std::copy(pipeName.begin(), pipeName.end(), config.pipeName);
    config.pipeName[pipeName.size()] = L'\0';

    std::memcpy(view, &config, sizeof(config));
    const BOOL flushed = FlushViewOfFile(view, sizeof(config));
    UnmapViewOfFile(view);
    if (flushed == FALSE) {
        setError(
            error,
            "failed to flush Gate C bridge configuration (win32=" +
                std::to_string(GetLastError()) + ")");
        return false;
    }
    return true;
}

bool injectLibrary(
    HANDLE process,
    const std::filesystem::path& libraryPath,
    std::string* error) {
    if (!fileExists(libraryPath)) {
        setError(
            error,
            "required Gate C artifact is missing: " +
                libraryPath.string());
        return false;
    }

    const std::wstring library = libraryPath.wstring();
    const SIZE_T bytes = (library.size() + 1u) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(
        process,
        nullptr,
        bytes,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
    if (remote == nullptr) {
        setError(
            error,
            "Gate C remote allocation failed (win32=" +
                std::to_string(GetLastError()) + ")");
        return false;
    }

    SIZE_T written = 0;
    const bool wrote =
        WriteProcessMemory(
            process,
            remote,
            library.c_str(),
            bytes,
            &written) != FALSE &&
        written == bytes;
    if (!wrote) {
        const auto code = GetLastError();
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        setError(
            error,
            "Gate C library path write failed (win32=" +
                std::to_string(code) + ")");
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        kernel32 != nullptr
            ? GetProcAddress(kernel32, "LoadLibraryW")
            : nullptr);
    if (loadLibrary == nullptr) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        setError(error, "LoadLibraryW could not be resolved");
        return false;
    }

    HANDLE thread = CreateRemoteThread(
        process,
        nullptr,
        0,
        loadLibrary,
        remote,
        0,
        nullptr);
    if (thread == nullptr) {
        const auto code = GetLastError();
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        setError(
            error,
            "Gate C bridge loader thread failed (win32=" +
                std::to_string(code) + ")");
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, kHandshakeTimeoutMs);
    if (wait == WAIT_TIMEOUT) {
        // The remote LoadLibraryW thread may still be reading `remote`. Freeing
        // that buffer here races the live target thread and can crash the game.
        // The caller owns a kill-on-close Seat Job and will terminate the still-
        // suspended target after this fail-closed attach error, which also
        // releases the remote allocation safely with the process address space.
        CloseHandle(thread);
        setError(error, "Gate C artifact load timed out; target launch aborted fail closed");
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        const auto code = GetLastError();
        // As above, do not free a buffer that an unobserved remote thread may
        // still reference. Launch rollback terminates the target process tree.
        CloseHandle(thread);
        setError(
            error,
            "Gate C artifact loader wait failed (win32=" +
                std::to_string(code) + ")");
        return false;
    }

    DWORD moduleResult = 0;
    const BOOL readExitCode = GetExitCodeThread(thread, &moduleResult);
    const auto code = readExitCode != FALSE
        ? (moduleResult != 0 ? ERROR_SUCCESS : ERROR_DLL_INIT_FAILED)
        : GetLastError();
    const bool loaded = readExitCode != FALSE && moduleResult != 0;

    CloseHandle(thread);
    // The loader thread is signaled, so the remote path buffer is no longer in
    // use and can now be released without racing LoadLibraryW.
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);

    if (!loaded) {
        setError(
            error,
            "Gate C artifact load failed (win32=" +
                std::to_string(code) + ")");
        return false;
    }
    return true;
}

bool validateHandshake(
    PipeChannel& server,
    const ExternalInputSessionOptions& options,
    const SessionToken& runtimeSessionId,
    std::string* error) {
    if (!waitForGateCClient(
            server,
            kHandshakeTimeoutMs,
            error)) {
        return false;
    }

    const auto read = server.readFrame(kHandshakeTimeoutMs);
    HelloMessage hello{};
    std::string decodeError;
    if (!read || !read.frame ||
        read.frame->sequence != 1u ||
        !decodeHello(*read.frame, hello, &decodeError)) {
        setError(
            error,
            decodeError.empty()
                ? "Gate C bridge sent an invalid handshake"
                : std::move(decodeError));
        return false;
    }

    if (hello.token != runtimeSessionId ||
        hello.seatId != options.seatId ||
        hello.processId != options.processId ||
        hello.architectureBits != 64u ||
        hello.targetWindow == 0u) {
        setError(error, "Gate C bridge handshake identity mismatch");
        return false;
    }

    DWORD windowOwner = 0;
    if (GetWindowThreadProcessId(
            reinterpret_cast<HWND>(
                static_cast<std::uintptr_t>(hello.targetWindow)),
            &windowOwner) == 0 ||
        windowOwner != options.processId) {
        setError(error, "Gate C bridge window is not owned by the target process");
        return false;
    }

    HelloAckMessage ack{};
    ack.accepted = true;
    ack.serverProcessId = GetCurrentProcessId();
    ack.grantedCapabilities = testCapabilityBits(
        kControlledTargetCapabilities |
        TestCapability::PollingApiShim |
        TestCapability::CursorFocusApiShim |
        TestCapability::RawInputApiShim);

    if (!server.writeFrame(
            encodeHelloAck(1u, ack),
            kIoTimeoutMs,
            error)) {
        return false;
    }
    return true;
}

#endif

} // namespace

class ExternalInputSession::Impl final {
public:
#if defined(_WIN32)
    PipeChannel channel;
    HANDLE mapping{nullptr};
#endif
    mutable std::mutex mutex;
    std::uint64_t sequence{1u};
    bool isActive{false};

    ~Impl() {
#if defined(_WIN32)
        if (mapping != nullptr) {
            CloseHandle(mapping);
            mapping = nullptr;
        }
#endif
    }
};

ExternalInputSession::ExternalInputSession(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ExternalInputSession::~ExternalInputSession() {
    shutdown();
}

std::shared_ptr<ExternalInputSession> ExternalInputSession::attach(
    const ExternalInputSessionOptions& options,
    std::string* error) {
#if defined(_WIN32)
    if (options.seatId == 0u ||
        options.processHandle == 0u ||
        options.processId == 0u ||
        options.artifactDirectory.empty() ||
        !validProfiledShimMask(options.requiredApiMask)) {
        setError(error, "invalid Gate C input-session options");
        return {};
    }

    HANDLE process = reinterpret_cast<HANDLE>(options.processHandle);
    if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
        setError(error, "target process exited before Gate C setup");
        return {};
    }

    const auto architecture = detectProcessArchitecture(process);
    if (!architecture ||
        architecture.architecture != ProcessArchitecture::X64) {
        setError(
            error,
            "process-local keyboard/mouse isolation currently requires an x64 target");
        return {};
    }

    const auto adapterPath =
        options.artifactDirectory / L"hydra_gate_c_adapter.dll";
    const auto shimPath =
        options.artifactDirectory / L"hydra_gate_c_shim.dll";
    const auto bridgePath =
        options.artifactDirectory / L"hydra_gate_c_external_bridge.dll";
    const auto xinputAdapterPath =
        options.artifactDirectory / L"hydra_xinput_adapter.dll";
    if (!fileExists(adapterPath) ||
        !fileExists(shimPath) ||
        !fileExists(bridgePath) ||
        (options.enableXInputRedirect &&
         !fileExists(xinputAdapterPath))) {
        setError(
            error,
            "Gate C runtime artifacts are not installed beside hydra_host.exe");
        return {};
    }

    const auto runtimeSessionId = generateSessionToken();
    if (!runtimeSessionId) {
        setError(error, "failed to generate Gate C session identifier");
        return {};
    }

    const auto pipeName = makeGateCPipeName(
        GetCurrentProcessId(),
        options.seatId,
        *runtimeSessionId);
    auto server = createGateCServerPipe(pipeName, error);
    if (!server.valid()) return {};

    auto impl = std::make_unique<Impl>();
    if (!writeBridgeMapping(
            impl->mapping,
            options.seatId,
            options.processId,
            *runtimeSessionId,
            pipeName,
            options.requiredApiMask,
            error) ||
        (options.enableXInputRedirect &&
         !injectLibrary(process, xinputAdapterPath, error)) ||
        !injectLibrary(process, adapterPath, error) ||
        !injectLibrary(process, shimPath, error) ||
        !injectLibrary(process, bridgePath, error) ||
        !validateHandshake(
            server,
            options,
            *runtimeSessionId,
            error)) {
        return {};
    }

    impl->channel = std::move(server);
    impl->isActive = true;

    auto session = std::shared_ptr<ExternalInputSession>(
        new ExternalInputSession(std::move(impl)));

    ControlStateMessage initialControl{};
    initialControl.virtualForeground = true;
    initialControl.virtualCapture = false;
    if (!session->sendControl(initialControl, error)) {
        session->shutdown();
        return {};
    }
    return session;
#else
    (void)options;
    setError(error, "Gate C external input sessions are Windows-only");
    return {};
#endif
}

bool ExternalInputSession::active() const noexcept {
    if (!impl_) return false;
    std::lock_guard lock(impl_->mutex);
    return impl_->isActive;
}

bool ExternalInputSession::sendInput(
    const InputEventMessage& input,
    std::string* error) {
    if (!impl_) {
        setError(error, "Gate C input session is unavailable");
        return false;
    }

    std::lock_guard lock(impl_->mutex);
#if defined(_WIN32)
    if (!impl_->isActive || !impl_->channel.valid()) {
        setError(error, "Gate C input session is not active");
        return false;
    }

    const auto frame = encodeInputEvent(++impl_->sequence, input);
    if (frame.empty() ||
        !impl_->channel.writeFrame(frame, kIoTimeoutMs, error)) {
        impl_->isActive = false;
        impl_->channel.close();
        return false;
    }
    return true;
#else
    (void)input;
    setError(error, "Gate C external input sessions are Windows-only");
    return false;
#endif
}

bool ExternalInputSession::sendControl(
    const ControlStateMessage& control,
    std::string* error) {
    if (!impl_) {
        setError(error, "Gate C input session is unavailable");
        return false;
    }

    std::lock_guard lock(impl_->mutex);
#if defined(_WIN32)
    if (!impl_->isActive || !impl_->channel.valid()) {
        setError(error, "Gate C input session is not active");
        return false;
    }

    const auto frame = encodeControlState(++impl_->sequence, control);
    if (frame.empty() ||
        !impl_->channel.writeFrame(frame, kIoTimeoutMs, error)) {
        impl_->isActive = false;
        impl_->channel.close();
        return false;
    }
    return true;
#else
    (void)control;
    setError(error, "Gate C external input sessions are Windows-only");
    return false;
#endif
}

void ExternalInputSession::shutdown() noexcept {
    if (!impl_) return;

    std::lock_guard lock(impl_->mutex);
#if defined(_WIN32)
    if (impl_->isActive && impl_->channel.valid()) {
        std::string ignored;
        (void)impl_->channel.writeFrame(
            encodeShutdown(++impl_->sequence),
            250u,
            &ignored);
    }
    impl_->channel.close();
    impl_->isActive = false;

    if (impl_->mapping != nullptr) {
        CloseHandle(impl_->mapping);
        impl_->mapping = nullptr;
    }
#else
    impl_->isActive = false;
#endif
}

} // namespace hydra::gatec
