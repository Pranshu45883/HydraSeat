#include "hydra/gate_c_cursor_focus_policy.hpp"
#include "hydra/win32_iat_patch.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct FakeWriter {
    std::size_t calls{0};
    std::size_t failCall{0};
    std::size_t secondFailCall{0};
};

hydra::gatec::IatWriteResult fakeWrite(
    std::uintptr_t* address, std::uintptr_t expected,
    std::uintptr_t replacement, void* opaque) noexcept {
    auto& writer = *static_cast<FakeWriter*>(opaque);
    ++writer.calls;
    if (writer.calls == writer.failCall ||
        writer.calls == writer.secondFailCall) {
        hydra::gatec::IatWriteResult result;
        result.systemError = 5;
        return result;
    }
    hydra::gatec::IatWriteResult result;
    if (address == nullptr || *address != expected) {
        result.systemError = 13;
        return result;
    }
    *address = replacement;
    result.success = true;
    return result;
}

using ValueArray = std::array<std::uintptr_t,
                              hydra::gatec::kCursorFocusImportCount>;

std::vector<hydra::gatec::CursorFocusIatSlot> slotsFor(
    ValueArray& values) {
    std::vector<hydra::gatec::CursorFocusIatSlot> slots;
    slots.reserve(hydra::gatec::kCursorFocusImportCount);
    for (std::size_t index = 0;
         index < hydra::gatec::kCursorFocusImportCount; ++index) {
        slots.push_back(hydra::gatec::CursorFocusIatSlot{
            static_cast<hydra::gatec::CursorFocusImport>(index),
            &values[index], values[index], values[index] + 100u});
    }
    return slots;
}

ValueArray originalValues() {
    ValueArray values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index] = 100u + index;
    }
    return values;
}

void testBoundedTransaction() {
    auto values = originalValues();
    const auto originals = values;
    auto slots = slotsFor(values);
    FakeWriter writer;
    hydra::gatec::CursorFocusIatPatchSet patches;
    auto report = patches.install(slots, fakeWrite, &writer);
    check(report && report.patchedMask ==
                        hydra::gatec::kCursorFocusImportMask &&
              patches.installed(),
          "all ten compile-time cursor/focus imports install transactionally");
    check(patches.install(slots, fakeWrite, &writer).status ==
              hydra::gatec::IatPatchStatus::AlreadyInstalled,
          "repeated cursor/focus install is idempotent");
    report = patches.uninstall(fakeWrite, &writer);
    check(report && report.restoredMask ==
                        hydra::gatec::kCursorFocusImportMask &&
              values == originals,
          "reverse uninstall restores every exact original pointer");
    check(static_cast<bool>(patches.uninstall(fakeWrite, &writer)),
          "repeated cursor/focus uninstall is idempotent");
}

void testMalformedPlansAndRollback() {
    auto values = originalValues();
    const auto originals = values;
    auto slots = slotsFor(values);
    FakeWriter writer;

    hydra::gatec::CursorFocusIatPatchSet missing;
    check(missing.install(
              std::span(slots).first(slots.size() - 1),
              fakeWrite, &writer).status ==
              hydra::gatec::IatPatchStatus::MissingImport,
          "missing cursor/focus allowlist entry is rejected");

    auto duplicateSlots = slots;
    duplicateSlots.push_back(slots.front());
    hydra::gatec::CursorFocusIatPatchSet duplicate;
    check(duplicate.install(duplicateSlots, fakeWrite, &writer).status ==
              hydra::gatec::IatPatchStatus::DuplicateImport,
          "duplicate cursor/focus import is rejected");

    auto invalidSlots = slots;
    invalidSlots[2].address = nullptr;
    hydra::gatec::CursorFocusIatPatchSet invalid;
    check(invalid.install(invalidSlots, fakeWrite, &writer).status ==
              hydra::gatec::IatPatchStatus::InvalidImage,
          "invalid cursor/focus patch metadata is rejected");

    writer = {};
    writer.failCall = 6;
    hydra::gatec::CursorFocusIatPatchSet rollback;
    auto report = rollback.install(slots, fakeWrite, &writer);
    check(report.status == hydra::gatec::IatPatchStatus::PatchFailure &&
              report.rollbackComplete && !rollback.installed() &&
              values == originals,
          "partial cursor/focus install failure rolls every prior slot back");

    writer = {};
    check(static_cast<bool>(rollback.install(slots, fakeWrite, &writer)),
          "cursor/focus patch set installs for retry coverage");
    writer.failCall = writer.calls + 1;
    report = rollback.uninstall(fakeWrite, &writer);
    check(report.status == hydra::gatec::IatPatchStatus::RollbackFailure &&
              !report.rollbackComplete && rollback.installed(),
          "uninstall failure retains a retryable exact restoration record");
    writer.failCall = 0;
    check(static_cast<bool>(rollback.uninstall(fakeWrite, &writer)) &&
              values == originals,
          "retry completes the remaining cursor/focus restoration");
}

void testCombinedCapabilityRollback() {
    std::array<std::uintptr_t, hydra::gatec::kPollingImportCount>
        pollingValues{11, 12, 13};
    const auto pollingOriginals = pollingValues;
    std::array<hydra::gatec::PollingIatSlot,
               hydra::gatec::kPollingImportCount> pollingSlots{
        hydra::gatec::PollingIatSlot{
            hydra::gatec::PollingImport::GetAsyncKeyState,
            &pollingValues[0], 11, 21},
        hydra::gatec::PollingIatSlot{
            hydra::gatec::PollingImport::GetKeyState,
            &pollingValues[1], 12, 22},
        hydra::gatec::PollingIatSlot{
            hydra::gatec::PollingImport::GetKeyboardState,
            &pollingValues[2], 13, 23}};
    auto cursorValues = originalValues();
    const auto cursorOriginals = cursorValues;
    auto cursorSlots = slotsFor(cursorValues);
    FakeWriter writer;
    hydra::gatec::PollingIatPatchSet polling;
    hydra::gatec::CursorFocusIatPatchSet cursor;
    check(static_cast<bool>(polling.install(
              pollingSlots, fakeWrite, &writer)),
          "polling capability installs before cursor/focus capability");
    writer.failCall = writer.calls + 4;
    const auto cursorFailure = cursor.install(
        cursorSlots, fakeWrite, &writer);
    const auto pollingRollback = polling.uninstall(fakeWrite, &writer);
    check(cursorFailure.status ==
              hydra::gatec::IatPatchStatus::PatchFailure &&
              cursorFailure.rollbackComplete && pollingRollback &&
              pollingValues == pollingOriginals &&
              cursorValues == cursorOriginals,
          "cursor/focus install failure rolls back the earlier polling capability too");
}

void testCoordinatePolicy() {
    using hydra::gatec::CursorClipRect;
    using hydra::gatec::CursorPoint;
    check(hydra::gatec::validCursorClipRect({-100, -50, 0, 25}),
          "negative logical coordinates form a valid clip rectangle");
    check(!hydra::gatec::validCursorClipRect({0, 0, 0, 10}) &&
              !hydra::gatec::validCursorClipRect({0, 2, 10, 2}),
          "empty and inverted clip rectangles are invalid");
    check(hydra::gatec::clampCursorToClip(
              CursorPoint{500, -500}, CursorClipRect{-100, -50, 0, 25}) ==
              CursorPoint{-1, -50},
          "cursor clamps against right/bottom-exclusive boundaries");
    const CursorClipRect extreme{
        (std::numeric_limits<std::int32_t>::min)(),
        (std::numeric_limits<std::int32_t>::min)(),
        (std::numeric_limits<std::int32_t>::max)(),
        (std::numeric_limits<std::int32_t>::max)()};
    check(hydra::gatec::validCursorClipRect(extreme) &&
              hydra::gatec::clampCursorToClip(
                  CursorPoint{(std::numeric_limits<std::int32_t>::max)(),
                              (std::numeric_limits<std::int32_t>::min)()},
                  extreme) ==
                  CursorPoint{
                      (std::numeric_limits<std::int32_t>::max)() - 1,
                      (std::numeric_limits<std::int32_t>::min)()},
          "extreme 32-bit coordinates clamp without overflow");
}

} // namespace

int main() {
    testBoundedTransaction();
    testMalformedPlansAndRollback();
    testCombinedCapabilityRollback();
    testCoordinatePolicy();
    std::cout << "Gate C cursor/focus policy and transaction tests passed.\n";
    return EXIT_SUCCESS;
}
