#include "hydra/controller_identity.hpp"
#include "hydra/controller_inventory.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <vector>

namespace {

bool hasIssue(const hydra::controller::BindingPlan& plan,
              hydra::controller::BindingIssueCode code) {
    return std::any_of(plan.issues.begin(), plan.issues.end(),
                       [code](const auto& issue) { return issue.code == code; });
}

} // namespace

void testControllerIdentity() {
    using namespace hydra::controller;

    const SourceDescriptor stablePad{
        "gameinput:pad-a",
        std::wstring{L"container-a"},
        L"Stable Pad A",
        ApiSurface::GameInput,
        IdentityQuality::Stable,
        std::nullopt,
        true,
    };
    const SourceDescriptor runtimePad{
        "xinput-slot:0",
        std::nullopt,
        L"Runtime XInput Slot 0",
        ApiSurface::XInput,
        IdentityQuality::RuntimeOnly,
        std::uint8_t{0},
        true,
    };
    const std::array sources{stablePad, runtimePad};

    const std::array requests{
        SeatBindingRequest{1, ApiSurface::GameInput,
                           std::wstring{L"CONTAINER-A"}, std::nullopt},
        SeatBindingRequest{2, ApiSurface::XInput,
                           std::nullopt, std::uint8_t{0}},
    };
    const auto validPlan = planSeatBindings(requests, sources);
    assert(validPlan.valid);
    assert(validPlan.bindings.size() == 2);

    const std::array missingIdentity{
        SeatBindingRequest{1, ApiSurface::XInput, std::nullopt, std::nullopt},
    };
    assert(hasIssue(planSeatBindings(missingIdentity, sources),
                    BindingIssueCode::MissingPersistentIdentity));

    const std::array duplicateSource{
        SeatBindingRequest{1, ApiSurface::GameInput,
                           std::wstring{L"container-a"}, std::nullopt},
        SeatBindingRequest{2, ApiSurface::GameInput,
                           std::wstring{L"container-a"}, std::nullopt},
    };
    assert(hasIssue(planSeatBindings(duplicateSource, sources),
                    BindingIssueCode::SourceAlreadyAssigned));

    const std::array invalidSlot{
        SeatBindingRequest{1, ApiSurface::XInput,
                           std::nullopt, std::uint8_t{4}},
    };
    assert(hasIssue(planSeatBindings(invalidSlot, sources),
                    BindingIssueCode::RuntimeSlotOutOfRange));

    std::vector<SourceDescriptor> ambiguous{stablePad, runtimePad, stablePad};
    ambiguous.back().runtimeKey = "gameinput:pad-a-duplicate";
    const std::array ambiguousRequest{
        SeatBindingRequest{1, ApiSurface::GameInput,
                           std::wstring{L"container-a"}, std::nullopt},
    };
    assert(hasIssue(planSeatBindings(ambiguousRequest, ambiguous),
                    BindingIssueCode::AmbiguousSource));

    const auto inventory = scanControllerSources();
#if defined(_WIN32)
    assert(inventory.authoritative);
    assert(inventory.sources.size() == kXInputSlotCount);
    for (std::uint8_t slot = 0; slot < kXInputSlotCount; ++slot) {
        const auto& source = inventory.sources[slot];
        assert(source.api == ApiSurface::XInput);
        assert(source.identityQuality == IdentityQuality::RuntimeOnly);
        assert(source.runtimeXInputSlot == std::optional<std::uint8_t>{slot});
        assert(!source.persistentId.has_value());
    }
#else
    assert(!inventory.authoritative);
    assert(inventory.sources.empty());
    assert(!inventory.error.empty());
#endif
}
