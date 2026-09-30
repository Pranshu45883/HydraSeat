#include "hydra/gate_c_raw_input_shim.hpp"
#include "hydra/win32_iat_patch.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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
};

hydra::gatec::IatWriteResult fakeWrite(
    std::uintptr_t* address, std::uintptr_t expected,
    std::uintptr_t replacement, void* opaque) noexcept {
    auto& writer = *static_cast<FakeWriter*>(opaque);
    ++writer.calls;
    hydra::gatec::IatWriteResult result;
    if (writer.failCall != 0 && writer.calls == writer.failCall) {
        result.systemError = 5;
        return result;
    }
    if (address == nullptr || *address != expected) {
        result.systemError = 13;
        return result;
    }
    *address = replacement;
    result.success = true;
    return result;
}

using Values = std::array<std::uintptr_t,
                          hydra::gatec::kRawInputImportCount>;

Values originals() {
    return {101, 102, 103, 104};
}

std::vector<hydra::gatec::RawInputIatSlot> slotsFor(Values& values) {
    std::vector<hydra::gatec::RawInputIatSlot> slots;
    for (std::size_t index = 0; index < values.size(); ++index) {
        slots.push_back({
            static_cast<hydra::gatec::RawInputImport>(index),
            &values[index], values[index], values[index] + 100u});
    }
    return slots;
}

void testRawPatchTransaction() {
    auto values = originals();
    const auto saved = values;
    auto slots = slotsFor(values);
    FakeWriter writer;
    hydra::gatec::RawInputIatPatchSet patches;
    auto report = patches.install(slots, fakeWrite, &writer);
    check(report && report.patchedMask == hydra::gatec::kRawInputImportMask &&
              patches.installed(),
          "four-function Raw Input patch set installs transactionally");
    check(patches.install(slots, fakeWrite, &writer).status ==
              hydra::gatec::IatPatchStatus::AlreadyInstalled,
          "repeated Raw Input install is deterministic");
    report = patches.uninstall(fakeWrite, &writer);
    check(report && report.restoredMask == hydra::gatec::kRawInputImportMask &&
              values == saved && !patches.installed(),
          "Raw Input uninstall restores exact originals in reverse order");
    check(static_cast<bool>(patches.uninstall(fakeWrite, &writer)),
          "repeated Raw Input uninstall is idempotent");
}

void testMalformedAndRollback() {
    auto values = originals();
    const auto saved = values;
    auto slots = slotsFor(values);
    FakeWriter writer;
    hydra::gatec::RawInputIatPatchSet missing;
    check(missing.install(std::span(slots).first(3), fakeWrite, &writer).status ==
              hydra::gatec::IatPatchStatus::MissingImport,
          "missing Raw Input import fails closed");
    auto duplicateSlots = slots;
    duplicateSlots.push_back(slots.front());
    hydra::gatec::RawInputIatPatchSet duplicate;
    check(duplicate.install(duplicateSlots, fakeWrite, &writer).status ==
              hydra::gatec::IatPatchStatus::DuplicateImport,
          "duplicate Raw Input import fails closed");
    auto invalidSlots = slots;
    invalidSlots[2].address = nullptr;
    hydra::gatec::RawInputIatPatchSet invalid;
    check(invalid.install(invalidSlots, fakeWrite, &writer).status ==
              hydra::gatec::IatPatchStatus::InvalidImage,
          "malformed Raw Input patch metadata is rejected");

    writer = {};
    writer.failCall = 3;
    hydra::gatec::RawInputIatPatchSet rollback;
    auto report = rollback.install(slots, fakeWrite, &writer);
    check(report.status == hydra::gatec::IatPatchStatus::PatchFailure &&
              report.rollbackComplete && values == saved &&
              !rollback.installed(),
          "partial Raw Input install restores earlier entries");

    writer = {};
    check(static_cast<bool>(rollback.install(slots, fakeWrite, &writer)),
          "Raw Input set installs for retryable uninstall coverage");
    writer.failCall = writer.calls + 1;
    report = rollback.uninstall(fakeWrite, &writer);
    check(report.status == hydra::gatec::IatPatchStatus::RollbackFailure &&
              !report.rollbackComplete && rollback.installed(),
          "failed Raw Input uninstall retains retry records");
    writer.failCall = 0;
    check(static_cast<bool>(rollback.uninstall(fakeWrite, &writer)) &&
              values == saved,
          "Raw Input uninstall retry completes exact restoration");
}

void testErrorContract() {
    check(hydra::gatec::rawInputSystemError(
              HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT) == 87 &&
              hydra::gatec::rawInputSystemError(
                  HYDRA_GATE_C_ADAPTER_BUFFER_TOO_SMALL) == 122 &&
              hydra::gatec::rawInputSystemError(
                  HYDRA_GATE_C_ADAPTER_RAW_INPUT_FAILURE) == 6 &&
              hydra::gatec::rawInputSystemError(
                  HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR) == 1167,
          "adapter failures have deterministic Win32 error mappings");
    check(!hydra::gatec::rawInputResultIsBackendFatal(
                  HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT) &&
              !hydra::gatec::rawInputResultIsBackendFatal(
                  HYDRA_GATE_C_ADAPTER_BUFFER_TOO_SMALL) &&
              !hydra::gatec::rawInputResultIsBackendFatal(
                  HYDRA_GATE_C_ADAPTER_RAW_INPUT_FAILURE) &&
              hydra::gatec::rawInputResultIsBackendFatal(
                  HYDRA_GATE_C_ADAPTER_INVALID_STATE) &&
              hydra::gatec::rawInputResultIsBackendFatal(
                  HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR),
          "caller and token errors stay local while backend loss is lifecycle-fatal");
}

void testCombinedInstallRollback() {
    std::array<std::uintptr_t, hydra::gatec::kPollingImportCount> pollingValues{
        11u, 12u, 13u};
    const auto savedPolling = pollingValues;
    std::vector<hydra::gatec::PollingIatSlot> pollingSlots;
    for (std::size_t index = 0; index < pollingValues.size(); ++index) {
        pollingSlots.push_back({
            static_cast<hydra::gatec::PollingImport>(index),
            &pollingValues[index], pollingValues[index],
            pollingValues[index] + 100u});
    }
    auto rawValues = originals();
    const auto savedRaw = rawValues;
    auto rawSlots = slotsFor(rawValues);
    FakeWriter writer;
    hydra::gatec::PollingIatPatchSet polling;
    hydra::gatec::RawInputIatPatchSet raw;
    check(static_cast<bool>(polling.install(pollingSlots, fakeWrite, &writer)),
          "combined transaction installs the polling prerequisite first");
    writer.failCall = writer.calls + 2u;
    const auto rawReport = raw.install(rawSlots, fakeWrite, &writer);
    writer.failCall = 0;
    const auto pollingRollback = polling.uninstall(fakeWrite, &writer);
    check(rawReport.status == hydra::gatec::IatPatchStatus::PatchFailure &&
              rawReport.rollbackComplete && pollingRollback &&
              pollingValues == savedPolling && rawValues == savedRaw &&
              !polling.installed() && !raw.installed(),
          "a Raw Input install failure rolls back both Raw Input and the polling prerequisite");
}

} // namespace

int main() {
    testRawPatchTransaction();
    testMalformedAndRollback();
    testCombinedInstallRollback();
    testErrorContract();
    std::cout << "Gate C Raw Input shim transaction tests passed.\n";
    return EXIT_SUCCESS;
}
