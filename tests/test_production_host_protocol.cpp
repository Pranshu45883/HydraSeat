#include "hydra/host_protocol.hpp"
#include "hydra/production_launch_runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::hostipc;
using namespace hydra::production;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

SeatConfig makeSeat(SeatId seatId) {
    SeatConfig seat;
    seat.seatId = seatId;
    seat.name = L"Protocol Seat " + std::to_wstring(seatId);
    seat.displayIds = {L"DISPLAY-" + std::to_wstring(seatId)};
    seat.primaryDisplayId = seat.displayIds.front();
    seat.keyboardIds = {L"KEYBOARD-" + std::to_wstring(seatId)};
    seat.mouseIds = {L"MOUSE-" + std::to_wstring(seatId)};
    seat.active = true;
    return seat;
}

plan::SeatProviderLaunchPlan makeSeatPlan(const SeatConfig& seat,
                                          std::string player,
                                          std::string game) {
    plan::SeatProviderLaunchPlan result;
    result.seatId = seat.seatId;
    result.playerId = std::move(player);
    result.gameId = std::move(game);
    result.setupId = "protocol-setup";
    result.instanceIndex = seat.seatId - 1u;
    result.requirementRevision = 10u + seat.seatId;
    result.compatibility = profile::CompatibilityReference{
        "compat-" + std::to_string(seat.seatId), "local-test", 3u};
    profile::InstanceRecipe recipe;
    recipe.arguments = {L"--seat", std::to_wstring(seat.seatId)};
    recipe.workingDirectory = L"C:\\Games";
    recipe.dataRoot = L"C:\\Games\\Data";
    result.instanceRecipe = std::move(recipe);
    result.hardwareFingerprint = seatHardwareFingerprint(seat);
    result.requirements.display = false;
    result.requirements.keyboard = true;
    result.requirements.mouse = true;
    result.requirements.controller = false;
    result.requirements.audioOutput = false;
    result.requirements.windowOwnership = false;
    result.requirements.recovery = false;
    result.requirements.highRisk = false;
    result.capabilities.process = true;
    result.capabilities.input = true;
    result.launchRequest.providerId = "manual-executable";
    result.launchRequest.gameId = result.gameId;
    result.launchRequest.providerAppId = "app-" + std::to_string(seat.seatId);
    result.launchRequest.accountRef = "account-" + std::to_string(seat.seatId);
    result.launchRequest.metadataRevision = 20u + seat.seatId;
    result.launchRequest.targetKind = provider::LaunchTargetKind::Executable;
    result.launchRequest.target = L"C:\\Games\\game.exe";
    result.launchRequest.arguments = {L"--safe", L"--windowed"};
    result.launchRequest.workingDirectory = L"C:\\Games";
    result.launchRequest.launchCorrelationId =
        "protocol-correlation-" + std::to_string(seat.seatId);
    return result;
}

plan::ProviderAwareLaunchPlan makePlan() {
    const auto first = makeSeat(1);
    const auto second = makeSeat(2);
    plan::ProviderAwareLaunchPlan result;
    result.seats = {makeSeatPlan(first, "mario", "game-a"),
                    makeSeatPlan(second, "luigi", "game-b")};
    result.fingerprint = providerPlanFingerprint(result);
    return result;
}

runtime::RuntimeSessionId sessionId() {
    runtime::RuntimeSessionId value;
    for (std::size_t index = 0; index < value.bytes.size(); ++index) {
        value.bytes[index] = static_cast<std::uint8_t>(index + 1u);
    }
    return value;
}

ProviderPlanInstallRequest requestFor(const plan::ProviderAwareLaunchPlan& plan,
                                      SeatId seatId) {
    const auto found = std::find_if(plan.seats.begin(), plan.seats.end(),
                                    [seatId](const auto& seat) {
                                        return seat.seatId == seatId;
                                    });
    ProviderPlanInstallRequest request;
    request.seatId = seatId;
    request.expectedRegistryRevision = 7u;
    request.planFingerprint = plan.fingerprint;
    request.planRevision = providerPlanRevision(*found);
    request.profileFingerprint = 0x1122334455667788ull;
    request.sessionId = sessionId();
    request.sessionGeneration = 9u;
    request.seatGameGeneration = 4u;
    request.plan = plan;
    return request;
}

void testTypedRoundTrips() {
    const auto plan = makePlan();
    const auto request = requestFor(plan, 1);
    const auto encoded = encodeProviderPlanInstallRequest(request);
    check(!encoded.empty() && encoded.size() <= kHostProtocolMaxPayloadBytes,
          "typed provider-plan install request encodes within frame bound");
    const auto decoded = decodeProviderPlanInstallRequest(encoded);
    check(decoded && *decoded == request,
          "typed provider-plan install request round-trips exactly");

    ProviderPlanRegistrySnapshot registry;
    registry.registryRevision = 8u;
    registry.profileFingerprint = request.profileFingerprint;
    registry.sessionId = request.sessionId;
    registry.sessionGeneration = request.sessionGeneration;
    registry.entries.push_back(ProviderPlanRegistryEntry{
        1u,
        8u,
        request.planFingerprint,
        request.planRevision,
        request.profileFingerprint,
        request.sessionId,
        request.sessionGeneration,
        request.seatGameGeneration,
        "mario",
        "game-a",
    });
    const auto registryBytes = encodeProviderPlanRegistrySnapshot(registry);
    const auto decodedRegistry = decodeProviderPlanRegistrySnapshot(registryBytes);
    check(decodedRegistry && *decodedRegistry == registry,
          "provider-plan registry snapshot round-trips exactly");

    ProviderPlanInstallResult result;
    result.code = ProviderPlanInstallCode::Ok;
    result.registry = registry;
    result.diagnostic = "installed";
    const auto resultBytes = encodeProviderPlanInstallResult(result);
    const auto decodedResult = decodeProviderPlanInstallResult(resultBytes);
    check(decodedResult && decodedResult->code == result.code &&
              decodedResult->registry == result.registry &&
              decodedResult->diagnostic == result.diagnostic,
          "typed provider-plan install result round-trips exactly");

    ProviderPlanRemoveRequest remove;
    remove.seatId = 1u;
    remove.expectedRegistryRevision = registry.registryRevision;
    remove.planFingerprint = request.planFingerprint;
    remove.planRevision = request.planRevision;
    remove.profileFingerprint = request.profileFingerprint;
    remove.sessionId = request.sessionId;
    remove.sessionGeneration = request.sessionGeneration;
    remove.seatGameGeneration = request.seatGameGeneration;
    const auto removeBytes = encodeProviderPlanRemoveRequest(remove);
    const auto decodedRemove = decodeProviderPlanRemoveRequest(removeBytes);
    check(decodedRemove && *decodedRemove == remove,
          "typed provider-plan removal request round-trips exactly");
}

void testVersionAndBoundsFailClosed() {
    check(kHostProtocolVersion == 4u,
          "provider-plan IPC ships only on host protocol v4");

    const auto plan = makePlan();
    const auto request = requestFor(plan, 1);
    Frame frame{MessageType::InstallProviderPlan, 0x1234u,
                encodeProviderPlanInstallRequest(request)};
    auto frameBytes = encodeFrame(frame);
    check(!frameBytes.empty(), "protocol-v4 install frame encodes");
    if (frameBytes.size() >= 6u) {
        frameBytes[4] = std::byte{3u};
        frameBytes[5] = std::byte{0u};
        DecodeResult result;
        check(!decodeFrame(frameBytes, &result) &&
                  result.error == ErrorCode::VersionMismatch,
              "protocol-v3 frame header is rejected by v4 host codec");
    }

    auto tooManyArguments = plan;
    tooManyArguments.seats.front().launchRequest.arguments.assign(
        kHostProtocolMaxPlanArguments + 1u, L"x");
    tooManyArguments.fingerprint = providerPlanFingerprint(tooManyArguments);
    auto overBoundRequest = requestFor(tooManyArguments, 1);
    check(encodeProviderPlanInstallRequest(overBoundRequest).empty(),
          "provider launch argument count is independently bounded");

    auto tooManySeats = plan;
    auto thirdSeat = makeSeat(3);
    tooManySeats.seats.push_back(makeSeatPlan(thirdSeat, "peach", "game-c"));
    tooManySeats.fingerprint = providerPlanFingerprint(tooManySeats);
    auto thirdSeatRequest = requestFor(tooManySeats, 1);
    check(encodeProviderPlanInstallRequest(thirdSeatRequest).empty(),
          "provider-plan payload refuses a third v1 Seat");

    auto forged = request;
    ++forged.plan.fingerprint;
    forged.planFingerprint = forged.plan.fingerprint;
    check(encodeProviderPlanInstallRequest(forged).empty(),
          "encoder refuses a plan whose content fingerprint was forged");

    auto overString = plan;
    overString.seats.front().playerId.assign(kHostProtocolMaxStringBytes + 1u, 'x');
    overString.fingerprint = providerPlanFingerprint(overString);
    auto overStringRequest = requestFor(overString, 1);
    check(encodeProviderPlanInstallRequest(overStringRequest).empty(),
          "provider-plan strings are independently bounded");

    auto malformed = encodeProviderPlanInstallRequest(request);
    if (!malformed.empty()) {
        malformed.pop_back();
        check(!decodeProviderPlanInstallRequest(malformed),
              "truncated provider-plan install payload is rejected");
    }
}

} // namespace

int main() {
    testTypedRoundTrips();
    testVersionAndBoundsFailClosed();
    if (failures != 0) {
        std::cerr << failures << " production host protocol test(s) failed.\n";
        return 1;
    }
    std::cout << "Production host protocol tests passed.\n";
    return 0;
}
