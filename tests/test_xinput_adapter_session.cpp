#include "hydra/xinput_adapter_session.hpp"

#include <cassert>
#include <limits>
#include <string>

void testXInputAdapterSession() {
    using hydra::controller::adapter::parseSessionConfig;

    const auto seat1 = parseSessionConfig(
        L"\\\\.\\pipe\\hydraseat-seat-1", L"1", L"101", L"11");
    assert(seat1.has_value());
    assert(seat1->valid());
    assert(seat1->pipeEndpoint == L"\\\\.\\pipe\\hydraseat-seat-1");
    assert(seat1->seatId == 1);
    assert(seat1->activationGeneration == 101);
    assert(seat1->sourceGeneration == 11);

    const auto seat2 = parseSessionConfig(
        L"pipe-b", L"2", L"202", L"22");
    assert(seat2.has_value());
    assert(seat2->seatId == 2);

    assert(!parseSessionConfig(L"", L"1", L"1", L"1").has_value());
    assert(!parseSessionConfig(L"pipe", L"0", L"1", L"1").has_value());
    assert(!parseSessionConfig(L"pipe", L"3", L"1", L"1").has_value());
    assert(!parseSessionConfig(L"pipe", L"1", L"0", L"1").has_value());
    assert(!parseSessionConfig(L"pipe", L"1", L"1", L"0").has_value());
    assert(!parseSessionConfig(L"pipe", L"x", L"1", L"1").has_value());
    assert(!parseSessionConfig(L"pipe", L"1", L"1x", L"1").has_value());
    assert(!parseSessionConfig(L"pipe", L"1", L"1", L"-1").has_value());

    const std::wstring overflow =
        std::to_wstring(std::numeric_limits<std::uint64_t>::max()) + L"0";
    assert(!parseSessionConfig(L"pipe", L"1", overflow, L"1").has_value());
}
