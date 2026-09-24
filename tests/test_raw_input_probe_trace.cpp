#include "hydra/raw_input_probe_trace.hpp"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace hydra::rawprobe;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

RawApiResult successApi(std::uint64_t value = 1) {
    RawApiResult result;
    result.kind = RawProbeResultKind::Success;
    result.success = true;
    result.resultValue = value;
    return result;
}

RawInputProbeTrace sampleTrace(std::uint16_t architectureBits = 64) {
    RawInputProbeTrace trace;
    trace.sourceKind = RawProbeSourceKind::SyntheticParserFixture;
    trace.architectureBits = architectureBits;
    trace.processId = 42;
    trace.threadId = 7;
    trace.rawInputHeaderBytes = architectureBits == 64 ? 24 : 16;
    trace.rawInputBytes = architectureBits == 64 ? 48 : 40;
    trace.rawInputBufferAlignmentBytes = architectureBits == 64 ? 8 : 4;

    RawRegistrationEvent registration;
    registration.context = {1, 100, 7};
    registration.operation = RawProbeOperation::ReplaceKeyboardTarget;
    registration.request = {1, 6, 0, 0x1234567887654321ull};
    registration.call = successApi();
    registration.before.api = successApi(1);
    registration.before.reportedDeviceCount = 1;
    registration.before.registrations.push_back({1, 6, 0, 0x1111});
    registration.after.api = successApi(2);
    registration.after.reportedDeviceCount = 2;
    registration.after.registrations.push_back({1, 2, 0x2000, 0x2222});
    registration.after.registrations.push_back({1, 6, 0, 0x1234567887654321ull});
    trace.registrationEvents.push_back(registration);

    RawDataQueryEvent data;
    data.context = {2, 101, 7};
    data.uiCommand = 0x10000003u;
    data.query = successApi(0);
    data.query.pcbSizeAfter = 40;
    data.query.reportedSize = 40;
    data.read = successApi(40);
    data.read.pcbSizeBefore = 40;
    data.read.pcbSizeAfter = 40;
    data.read.returnedSize = 40;
    data.header = {true, 1, 40, 0xfeedfacecafebeefull, 0, "device-path", "Keyboard:stable"};
    data.totalPayloadBytes = 40;
    trace.dataEvents.push_back(data);

    RawMessageEvent message;
    message.context = {3, 102, 7};
    message.messageKind = RawMessageKind::Input;
    message.messageId = 0x00ffu;
    message.messageTimeMilliseconds = 1234;
    message.windowRuntimeValue = 0x1111;
    message.wParamRuntimeValue = 0;
    message.lParamRuntimeValue = 0x9999;
    message.inputCode = RawInputCodeKind::Foreground;
    message.headerQuery = data;
    message.inputQueryAndRead = data;
    trace.messageEvents.push_back(message);

    RawBufferQueryEvent buffer;
    buffer.context = {4, 103, 7};
    buffer.requestedBufferBytes = 64;
    buffer.call = successApi(1);
    buffer.call.pcbSizeBefore = 64;
    buffer.call.pcbSizeAfter = 64;
    buffer.returnedRawInputCount = 1;
    buffer.blocks.push_back({0, 40, data.header});
    trace.bufferEvents.push_back(buffer);

    trace.observations.push_back({
        "dangerous_flags", RawProbeResultKind::NotObserved,
        "RIDEV_NOLEGACY/RIDEV_CAPTUREMOUSE/RIDEV_APPKEYS: NotTestedInP3Raw01"});
    return trace;
}

void writeU32(std::vector<std::byte>& bytes, std::size_t offset,
              std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
    }
}

void writeRuntime(std::vector<std::byte>& bytes, std::size_t offset,
                  std::size_t width, std::uint64_t value) {
    for (std::size_t index = 0; index < width; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
    }
}

std::vector<std::byte> rawBlock(std::uint16_t architectureBits,
                                std::uint32_t type,
                                std::uint32_t size,
                                std::uint64_t device = 0x1122334455667788ull) {
    const std::size_t header = architectureBits == 64 ? 24 : 16;
    const std::size_t alignment = architectureBits == 64 ? 8 : 4;
    const std::size_t storage = (static_cast<std::size_t>(size) + alignment - 1) & ~(alignment - 1);
    std::vector<std::byte> bytes(storage);
    writeU32(bytes, 0, type);
    writeU32(bytes, 4, size);
    writeRuntime(bytes, 8, alignment, device);
    writeRuntime(bytes, 8 + alignment, alignment, 1);
    check(header <= size, "synthetic raw block contains its header");
    return bytes;
}

void testTraceContract() {
    const auto trace = sampleTrace();
    const std::string first = serializeRawInputProbeTrace(trace);
    const std::string second = serializeRawInputProbeTrace(trace);
    check(!first.empty(), "trace serialization succeeds");
    check(first == second, "trace serialization is deterministic");
    const auto parsed = parseRawInputProbeTrace(first);
    check(static_cast<bool>(parsed), "trace round trip parses");
    check(parsed && parsed.trace->registrationEvents == trace.registrationEvents,
          "trace registration event round trips");
    check(parsed && parsed.trace->dataEvents == trace.dataEvents,
          "trace data event round trips");
    check(parsed && parsed.trace->messageEvents == trace.messageEvents,
          "trace message event round trips");

    std::string future = first;
    const auto version = future.find("\"schema_version\":1");
    check(version != std::string::npos, "schema version field is present");
    future.replace(version, std::string("\"schema_version\":1").size(),
                   "\"schema_version\":2");
    check(!parseRawInputProbeTrace(future), "future schema version is rejected");

    std::string zero = first;
    zero.replace(version, std::string("\"schema_version\":1").size(),
                 "\"schema_version\":0");
    check(!parseRawInputProbeTrace(zero), "unknown old schema version is rejected");
    check(!parseRawInputProbeTrace(first.substr(0, first.size() - 1)),
          "truncated trace is rejected");
    check(!parseRawInputProbeTrace(std::string(kMaxFixtureBytes + 1, 'x')),
          "oversized trace is rejected");

    auto tooMany = sampleTrace();
    tooMany.observations.resize(kMaxTraceEvents + 1);
    check(serializeRawInputProbeTrace(tooMany).empty(),
          "maximum total event count is enforced");

    auto tooManyRegistrations = sampleTrace();
    tooManyRegistrations.registrationEvents[0].after.registrations.resize(
        kMaxRawRegistrations + 1);
    check(serializeRawInputProbeTrace(tooManyRegistrations).empty(),
          "maximum registration count is enforced");

    auto invalidUtf8 = sampleTrace();
    invalidUtf8.observations[0].detail = std::string(1, static_cast<char>(0xff));
    check(serializeRawInputProbeTrace(invalidUtf8).empty(),
          "invalid UTF-8 is rejected");
}

void testDeterministicRegistrationOrdering() {
    auto trace = sampleTrace();
    auto& values = trace.registrationEvents[0].after.registrations;
    const std::string unsorted = serializeRawInputProbeTrace(trace);
    std::reverse(values.begin(), values.end());
    const std::string reversed = serializeRawInputProbeTrace(trace);
    check(unsorted == reversed, "registration snapshots serialize in canonical order");
}

void testRuntimeHandleWidthNormalization() {
    const auto x86 = sampleTrace(32);
    const auto x64 = sampleTrace(64);
    const auto parsedX86 = parseRawInputProbeTrace(serializeRawInputProbeTrace(x86));
    const auto parsedX64 = parseRawInputProbeTrace(serializeRawInputProbeTrace(x64));
    check(parsedX86 && parsedX64, "x86 and x64 traces parse");
    check(parsedX86 && parsedX64 &&
              parsedX86.trace->registrationEvents[0].request.targetWindowRuntimeValue ==
                  parsedX64.trace->registrationEvents[0].request.targetWindowRuntimeValue,
          "runtime diagnostic values remain fixed-width across architectures");
}

RawDataContractInput validDataContract(std::uint32_t type) {
    RawDataContractInput input;
    input.headerBytes = 24;
    input.queryReturnValue = 0;
    input.querySizeAfter = 48;
    input.readReturnValue = 48;
    input.readSizeAfter = 48;
    input.suppliedBufferBytes = 48;
    input.rawDwType = type;
    input.rawDwSize = 48;
    return input;
}

void testRawDataContract() {
    check(validateRawDataContract(validDataContract(1)) == RawProbeResultKind::Success,
          "valid keyboard RAWINPUT contract passes");
    check(validateRawDataContract(validDataContract(0)) == RawProbeResultKind::Success,
          "valid mouse RAWINPUT contract passes");

    auto value = validDataContract(1);
    value.querySizeAfter = 0;
    check(validateRawDataContract(value) == RawProbeResultKind::InvalidContract,
          "zero size is rejected");
    value = validDataContract(1);
    value.rawDwSize = 8;
    check(validateRawDataContract(value) == RawProbeResultKind::Truncated,
          "header shorter than RAWINPUTHEADER is rejected");
    value = validDataContract(1);
    value.querySizeAfter = static_cast<std::uint32_t>(kMaxRawPacketBytes + 1);
    check(validateRawDataContract(value) == RawProbeResultKind::Oversized,
          "reported packet larger than maximum is rejected");
    value = validDataContract(1);
    value.suppliedBufferBytes = 32;
    check(validateRawDataContract(value) == RawProbeResultKind::Truncated,
          "truncated payload is rejected");
    value = validDataContract(1);
    value.rawDwSize = 44;
    check(validateRawDataContract(value) == RawProbeResultKind::SizeMismatch,
          "inconsistent dwSize is rejected");
    value = validDataContract(99);
    check(validateRawDataContract(value) == RawProbeResultKind::UnsupportedType,
          "unknown raw input type is visible");
    value = validDataContract(1);
    value.readReturnValue = std::numeric_limits<std::uint32_t>::max();
    check(validateRawDataContract(value) == RawProbeResultKind::ApiFailure,
          "API error sentinel is visible");
    value = validDataContract(1);
    value.readReturnValue = 40;
    value.readSizeAfter = 40;
    value.rawDwSize = 40;
    check(validateRawDataContract(value) == RawProbeResultKind::SizeMismatch,
          "size query/read disagreement is rejected");
}

void testRegistrationContract() {
    RawRegistrationContractInput input;
    input.request = {1, 6, 0, 0x1234};
    input.cbSize = 16;
    input.nativeCbSize = 16;
    check(validateRawRegistrationContract(input) == RawProbeResultKind::Success,
          "keyboard registration contract is accepted");
    input.request.usage = 2;
    check(validateRawRegistrationContract(input) == RawProbeResultKind::Success,
          "mouse registration contract is accepted");
    input.cbSize = 8;
    check(validateRawRegistrationContract(input) == RawProbeResultKind::InvalidContract,
          "invalid registration cbSize is rejected");
    input.cbSize = 16;
    input.request.targetWindowRuntimeValue = 0;
    check(validateRawRegistrationContract(input) == RawProbeResultKind::InvalidContract,
          "controlled non-remove registration requires a target");
    input.request.flags = 1;
    check(validateRawRegistrationContract(input) == RawProbeResultKind::Success,
          "RIDEV_REMOVE requires a null target");
    input.request.targetWindowRuntimeValue = 1;
    check(validateRawRegistrationContract(input) == RawProbeResultKind::InvalidContract,
          "RIDEV_REMOVE rejects a non-null target");
}

void testRawBufferContract() {
    const auto empty = parseRawInputBufferLayout({}, 0, 64, 24);
    check(empty && empty.blocks.empty(), "zero-packet buffer succeeds");

    auto first = rawBlock(64, 1, 40);
    auto second = rawBlock(64, 0, 48, 0x1234);
    first.insert(first.end(), second.begin(), second.end());
    const auto multi = parseRawInputBufferLayout(first, 2, 64, 24);
    check(multi && multi.blocks.size() == 2, "bounded multi-packet buffer parses");
    check(multi && multi.blocks[0].alignedNextOffset == 40,
          "next block observes native pointer alignment");

    auto malformed = rawBlock(64, 1, 40);
    writeU32(malformed, 4, 8);
    check(parseRawInputBufferLayout(malformed, 1, 64, 24).kind ==
              RawProbeResultKind::InvalidContract,
          "non-progressing/malformed next block is rejected");

    check(parseRawInputBufferLayout(first,
              static_cast<std::uint32_t>(kMaxRawBufferPackets + 1), 64, 24).kind ==
              RawProbeResultKind::BoundsExceeded,
          "raw buffer packet-count bound is enforced");
    check(parseRawInputBufferLayout(
              std::vector<std::byte>(kMaxRawPacketBytes + 1), 1, 64, 24).kind ==
              RawProbeResultKind::Oversized,
          "raw buffer byte bound is enforced");
    check(parseRawInputBufferLayout(first, 2, 64, 16).kind ==
              RawProbeResultKind::InvalidContract,
          "architecture/header mismatch is rejected");

    const auto x86Block = rawBlock(32, 1, 32, 0xfedcba98u);
    const auto x86 = parseRawInputBufferLayout(x86Block, 1, 32, 16);
    check(x86 && x86.blocks[0].header.deviceRuntimeValue == 0xfedcba98u,
          "x86 runtime handle is normalized to uint64");
    const auto wow64Bytes = rawBlock(32, 1, 36, 0x1234);
    const auto wow64 = parseRawInputBufferLayout(wow64Bytes, 1, 32, 16, 8);
    check(wow64 && wow64.blocks[0].alignedNextOffset == 40,
          "WOW64 eight-byte raw buffer alignment is supported explicitly");
}

void testSyntheticFixture() {
#ifdef HYDRA_SOURCE_DIR
    const std::string fixturePath =
        std::string(HYDRA_SOURCE_DIR) + "/tests/fixtures/gate_c_raw_input_parser_v1.json";
    std::ifstream input(fixturePath, std::ios::binary);
    const std::string fixture((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    const auto parsed = parseRawInputProbeTrace(fixture);
    check(static_cast<bool>(parsed), "synthetic parser fixture parses");
    check(parsed && parsed.trace->sourceKind ==
              RawProbeSourceKind::SyntheticParserFixture,
          "fixture cannot be confused with observed Windows evidence");
    check(parsed && !parsed.trace->physicalInputObserved,
          "synthetic fixture makes no physical-input claim");
#endif
}

} // namespace

int main() {
    testTraceContract();
    testDeterministicRegistrationOrdering();
    testRuntimeHandleWidthNormalization();
    testRawDataContract();
    testRegistrationContract();
    testRawBufferContract();
    testSyntheticFixture();
    if (failures != 0) {
        std::cerr << failures << " raw input probe trace test(s) failed\n";
        return 1;
    }
    std::cout << "Raw Input probe trace tests passed\n";
    return 0;
}
