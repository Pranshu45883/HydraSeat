#include "hydra/host_transport.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

void printHostSnapshot(const hydra::hostipc::HostSnapshot& snapshot) {
    std::cout << "authority_revision=" << snapshot.authorityRevision << '\n';
    for (const auto& seat : snapshot.seats) {
        std::cout
            << "seat=" << seat.seatId
            << " generation=" << seat.generation
            << " active=" << (seat.active ? 1 : 0)
            << " ui_lease=" << (seat.uiLeaseActive ? 1 : 0)
            << " game_lease=" << (seat.gameLeaseActive ? 1 : 0)
            << " process_owned=" << (seat.processOwned ? 1 : 0)
            << " window_owned=" << (seat.windowOwned ? 1 : 0)
            << " controller_bound=" << (seat.controllerBound ? 1 : 0)
            << " process_id=" << seat.processId
            << " process_creation_identity=" << seat.processCreationIdentity
            << " target_hwnd=" << seat.targetHwnd
            << '\n';
    }
}

#if defined(_WIN32)
std::optional<std::string> wideToUtf8(std::wstring_view value) {
    if (value.empty()) return std::string{};
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
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

std::optional<std::uint32_t> parseSeat(std::wstring_view value) {
    if (value.size() != 1 || (value[0] != L'1' && value[0] != L'2')) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value[0] - L'0');
}
#endif

int printSnapshot() {
    hydra::hostipc::HostPipeClient client;
    std::string error;
    if (!client.connect(hydra::hostipc::ClientRole::ReadOnly, 5000, &error)) {
        std::cerr << "connect failed: " << error << '\n';
        return 1;
    }

    const auto snapshot = client.getSnapshot(5000, &error);
    if (!snapshot) {
        std::cerr << "snapshot failed: " << error << '\n';
        return 1;
    }
    printHostSnapshot(*snapshot);
    return 0;
}

int runPing() {
    hydra::hostipc::HostPipeClient client;
    std::string error;
    if (!client.connect(hydra::hostipc::ClientRole::ReadOnly, 5000, &error)) {
        std::cerr << "connect failed: " << error << '\n';
        return 1;
    }
    constexpr std::uint64_t nonce = 0x4859445241ull;
    if (!client.ping(nonce, 5000, &error)) {
        std::cerr << "ping failed: " << error << '\n';
        return 1;
    }
    std::cout << "pong\n";
    return 0;
}

#if defined(_WIN32)
int runLaunch(
    std::uint32_t seatId,
    std::wstring_view executable,
    std::wstring_view arguments) {
    const auto executableUtf8 = wideToUtf8(executable);
    const auto argumentsUtf8 = wideToUtf8(arguments);
    if (!executableUtf8 || !argumentsUtf8 || executableUtf8->empty()) {
        std::cerr << "launch arguments are not valid Unicode\n";
        return 2;
    }

    hydra::hostipc::HostPipeClient client;
    std::string error;
    if (!client.connect(hydra::hostipc::ClientRole::Control, 5000, &error)) {
        std::cerr << "connect failed: " << error << '\n';
        return 1;
    }
    if (!client.acquireUiLease(seatId, 5000, &error)) {
        std::cerr << "acquire UI lease failed: " << error << '\n';
        return 1;
    }

    hydra::hostipc::LaunchGameRequest request;
    request.seatId = seatId;
    request.executablePathUtf8 = *executableUtf8;
    request.launchArgumentsUtf8 = *argumentsUtf8;

    const auto snapshot = client.launchGame(request, 5000, &error);
    if (!snapshot) {
        std::cerr << "launch failed: " << error << '\n';
        return 1;
    }
    printHostSnapshot(*snapshot);
    return 0;
}

int runStop(std::uint32_t seatId) {
    hydra::hostipc::HostPipeClient client;
    std::string error;
    if (!client.connect(hydra::hostipc::ClientRole::Control, 5000, &error)) {
        std::cerr << "connect failed: " << error << '\n';
        return 1;
    }
    if (!client.acquireUiLease(seatId, 5000, &error)) {
        std::cerr << "acquire UI lease failed: " << error << '\n';
        return 1;
    }

    const auto snapshot = client.stopGame(seatId, 10000, &error);
    if (!snapshot) {
        std::cerr << "stop failed: " << error << '\n';
        return 1;
    }
    printHostSnapshot(*snapshot);
    return 0;
}
#endif

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    const std::wstring_view command =
        argc >= 2 && argv[1] != nullptr ? argv[1] : L"snapshot";

    if (command == L"snapshot") return printSnapshot();
    if (command == L"ping") return runPing();

    if (command == L"launch") {
        if (argc < 4 || argc > 5 || argv[2] == nullptr || argv[3] == nullptr) {
            std::cerr
                << "usage: hydraseat_hostctl launch <seat:1|2> <executable> [arguments]\n";
            return 2;
        }
        const auto seat = parseSeat(argv[2]);
        if (!seat) {
            std::cerr << "Seat must be 1 or 2\n";
            return 2;
        }
        return runLaunch(
            *seat,
            argv[3],
            argc == 5 && argv[4] != nullptr
                ? std::wstring_view(argv[4])
                : std::wstring_view{});
    }

    if (command == L"stop") {
        if (argc != 3 || argv[2] == nullptr) {
            std::cerr << "usage: hydraseat_hostctl stop <seat:1|2>\n";
            return 2;
        }
        const auto seat = parseSeat(argv[2]);
        if (!seat) {
            std::cerr << "Seat must be 1 or 2\n";
            return 2;
        }
        return runStop(*seat);
    }

    std::cerr
        << "usage: hydraseat_hostctl [snapshot|ping|launch|stop]\n";
    return 2;
}
#else
int main() {
    return 2;
}
#endif
