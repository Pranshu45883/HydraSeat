#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/seat_hardware_store.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::filesystem::path makeTestRoot() {
    auto root = std::filesystem::temp_directory_path();
#if defined(_WIN32)
    root /= L"HydraSeat-seat-store-" + std::to_wstring(GetCurrentProcessId());
#else
    root /= "HydraSeat-seat-store-test";
#endif
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    return root;
}

} // namespace

int main() {
    using hydra::runtime::SeatHardwareConfiguration;
    using hydra::runtime::SeatHardwareConfigurations;
    using hydra::runtime::SeatHardwareStore;

    const auto root = makeTestRoot();
    const auto path = root / L"seat-config.json";

    SeatHardwareConfigurations initial{{
        SeatHardwareConfiguration{1},
        SeatHardwareConfiguration{2},
    }};

    {
        SeatHardwareStore store(path);
        SeatHardwareConfigurations loaded{{
            SeatHardwareConfiguration{1, L"sentinel"},
            SeatHardwareConfiguration{2, L"sentinel"},
        }};
        std::string error;
        assert(store.load(loaded, &error));
        assert(loaded == initial);

        SeatHardwareConfigurations configured{{
            SeatHardwareConfiguration{
                1,
                L"display:one",
                L"keyboard:one",
                L"mouse:one",
                L"container:{11111111-1111-1111-1111-111111111111}"},
            SeatHardwareConfiguration{
                2,
                L"display:two",
                L"keyboard:two",
                L"mouse:two",
                L"container:{22222222-2222-2222-2222-222222222222}"},
        }};
        assert(store.save(configured, &error));

        SeatHardwareStore reloaded(path);
        SeatHardwareConfigurations roundTrip = initial;
        assert(reloaded.load(roundTrip, &error));
        assert(roundTrip == configured);

        auto conflicting = configured;
        conflicting[1].keyboardId = conflicting[0].keyboardId;
        assert(!reloaded.save(conflicting, &error));

        conflicting = configured;
        conflicting[1].controllerId = conflicting[0].controllerId;
        assert(!reloaded.save(conflicting, &error));

        SeatHardwareStore preserved(path);
        SeatHardwareConfigurations afterRejectedWrite = initial;
        assert(preserved.load(afterRejectedWrite, &error));
        assert(afterRejectedWrite == configured);
    }

    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "{ definitely-not-valid-json";
        corrupt.close();

        SeatHardwareStore store(path);
        SeatHardwareConfigurations sentinel{{
            SeatHardwareConfiguration{
                1, L"display:sentinel", L"keyboard:sentinel", L"mouse:sentinel"},
            SeatHardwareConfiguration{2},
        }};
        const auto before = sentinel;
        std::string error;
        assert(!store.load(sentinel, &error));
        assert(sentinel == before);
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return 0;
}
