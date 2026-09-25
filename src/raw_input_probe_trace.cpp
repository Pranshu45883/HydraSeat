#include "hydra/raw_input_probe_trace.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace hydra::rawprobe {
namespace {

struct JsonNumber {
    std::string text;
};

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    std::variant<std::nullptr_t, bool, JsonNumber, std::string, Array, Object> value;
};

bool validUtf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fu) {
            ++index;
            continue;
        }
        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if (first >= 0xc2u && first <= 0xdfu) {
            continuationCount = 1;
            codePoint = first & 0x1fu;
        } else if (first >= 0xe0u && first <= 0xefu) {
            continuationCount = 2;
            codePoint = first & 0x0fu;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            continuationCount = 3;
            codePoint = first & 0x07u;
        } else {
            return false;
        }
        if (continuationCount > text.size() - index - 1u) return false;
        for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xc0u) != 0x80u) return false;
            codePoint = (codePoint << 6u) | (next & 0x3fu);
        }
        if ((continuationCount == 2 && codePoint < 0x800u) ||
            (continuationCount == 3 && codePoint < 0x10000u) ||
            codePoint > 0x10ffffu ||
            (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            return false;
        }
        index += continuationCount + 1u;
    }
    return true;
}

class JsonParser final {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skipSpace();
        JsonValue result = parseValue(0);
        skipSpace();
        if (position_ != text_.size()) {
            fail("trailing JSON data");
        }
        return result;
    }

private:
    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(message);
    }

    void skipSpace() {
        while (position_ < text_.size()) {
            const char value = text_[position_];
            if (value != ' ' && value != '\n' && value != '\r' && value != '\t') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char expected) {
        skipSpace();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void require(char expected) {
        if (!consume(expected)) {
            fail("unexpected JSON token");
        }
    }

    std::string parseString() {
        skipSpace();
        if (position_ >= text_.size() || text_[position_++] != '"') {
            fail("expected JSON string");
        }
        std::string result;
        while (position_ < text_.size()) {
            const unsigned char value = static_cast<unsigned char>(text_[position_++]);
            if (value == '"') {
                if (!validUtf8(result)) {
                    fail("JSON string is not valid UTF-8");
                }
                return result;
            }
            if (value < 0x20u) {
                fail("unescaped JSON control character");
            }
            if (value != '\\') {
                result.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= text_.size()) {
                fail("truncated JSON escape");
            }
            const char escaped = text_[position_++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                if (position_ + 4 > text_.size()) {
                    fail("truncated JSON unicode escape");
                }
                unsigned code = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = text_[position_++];
                    code <<= 4u;
                    if (digit >= '0' && digit <= '9') code += static_cast<unsigned>(digit - '0');
                    else if (digit >= 'a' && digit <= 'f') code += static_cast<unsigned>(digit - 'a' + 10);
                    else if (digit >= 'A' && digit <= 'F') code += static_cast<unsigned>(digit - 'A' + 10);
                    else fail("invalid JSON unicode escape");
                }
                if (code > 0x7fu) {
                    fail("trace unicode escapes must be ASCII controls");
                }
                result.push_back(static_cast<char>(code));
                break;
            }
            default:
                fail("unsupported JSON escape");
            }
        }
        fail("unterminated JSON string");
    }

    JsonNumber parseNumber() {
        skipSpace();
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') {
            ++position_;
        }
        const std::size_t digits = position_;
        while (position_ < text_.size() && text_[position_] >= '0' &&
               text_[position_] <= '9') {
            ++position_;
        }
        if (digits == position_) {
            fail("invalid JSON number");
        }
        if ((position_ - digits) > 1 && text_[digits] == '0') {
            fail("non-canonical JSON number");
        }
        if (position_ < text_.size() &&
            (text_[position_] == '.' || text_[position_] == 'e' ||
             text_[position_] == 'E')) {
            fail("trace numbers must be integers");
        }
        return {std::string(text_.substr(start, position_ - start))};
    }

    JsonValue parseValue(std::uint32_t depth) {
        if (depth > 32) {
            fail("JSON nesting exceeds limit");
        }
        skipSpace();
        if (position_ >= text_.size()) {
            fail("truncated JSON");
        }
        if (text_[position_] == '"') {
            return JsonValue{parseString()};
        }
        if (text_[position_] == '{') {
            ++position_;
            JsonValue::Object object;
            if (consume('}')) {
                return JsonValue{std::move(object)};
            }
            while (true) {
                const std::string key = parseString();
                require(':');
                if (!object.emplace(key, parseValue(depth + 1)).second) {
                    fail("duplicate JSON key");
                }
                if (consume('}')) {
                    break;
                }
                require(',');
            }
            return JsonValue{std::move(object)};
        }
        if (text_[position_] == '[') {
            ++position_;
            JsonValue::Array array;
            if (consume(']')) {
                return JsonValue{std::move(array)};
            }
            while (true) {
                if (array.size() >= kMaxTraceEvents * 8) {
                    fail("JSON array exceeds limit");
                }
                array.push_back(parseValue(depth + 1));
                if (consume(']')) {
                    break;
                }
                require(',');
            }
            return JsonValue{std::move(array)};
        }
        if (text_.substr(position_, 4) == "true") {
            position_ += 4;
            return JsonValue{true};
        }
        if (text_.substr(position_, 5) == "false") {
            position_ += 5;
            return JsonValue{false};
        }
        if (text_.substr(position_, 4) == "null") {
            position_ += 4;
            return JsonValue{nullptr};
        }
        return JsonValue{parseNumber()};
    }

    std::string_view text_;
    std::size_t position_{0};
};

const JsonValue::Object& objectOf(const JsonValue& value) {
    const auto* object = std::get_if<JsonValue::Object>(&value.value);
    if (object == nullptr) {
        throw std::runtime_error("expected JSON object");
    }
    return *object;
}

const JsonValue::Array& arrayOf(const JsonValue& value) {
    const auto* array = std::get_if<JsonValue::Array>(&value.value);
    if (array == nullptr) {
        throw std::runtime_error("expected JSON array");
    }
    return *array;
}

const JsonValue& required(const JsonValue::Object& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        throw std::runtime_error(std::string("missing JSON field: ") + key);
    }
    return found->second;
}

std::uint64_t unsignedOf(const JsonValue& value) {
    const auto* number = std::get_if<JsonNumber>(&value.value);
    if (number == nullptr || number->text.empty() || number->text.front() == '-') {
        throw std::runtime_error("expected unsigned JSON integer");
    }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(number->text.data(),
                                        number->text.data() + number->text.size(),
                                        result);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != number->text.data() + number->text.size()) {
        throw std::runtime_error("unsigned JSON integer out of range");
    }
    return result;
}

template <typename T>
T boundedUnsignedOf(const JsonValue& value) {
    const std::uint64_t number = unsignedOf(value);
    if (number > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        throw std::runtime_error("JSON integer exceeds field width");
    }
    return static_cast<T>(number);
}

bool boolOf(const JsonValue& value) {
    const auto* boolean = std::get_if<bool>(&value.value);
    if (boolean == nullptr) {
        throw std::runtime_error("expected JSON boolean");
    }
    return *boolean;
}

const std::string& stringOf(const JsonValue& value) {
    const auto* text = std::get_if<std::string>(&value.value);
    if (text == nullptr) {
        throw std::runtime_error("expected JSON string");
    }
    return *text;
}

template <typename Enum>
Enum enumOf(const JsonValue& value, std::uint8_t minimum,
            std::uint8_t maximum) {
    const auto raw = boundedUnsignedOf<std::uint8_t>(value);
    if (raw < minimum || raw > maximum) {
        throw std::runtime_error("JSON enum value is invalid");
    }
    return static_cast<Enum>(raw);
}

void quote(std::ostringstream& output, std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (const char character : text) {
        const auto value = static_cast<unsigned char>(character);
        switch (value) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (value < 0x20u) {
                output << "\\u00" << hex[value >> 4u] << hex[value & 0x0fu];
            } else {
                output << static_cast<char>(value);
            }
        }
    }
    output << '"';
}

bool headerTextValid(const RawHeaderObservation& header) noexcept {
    return validUtf8(header.devicePath) && validUtf8(header.stableDeviceId);
}

bool traceBoundsAndTextValid(const RawInputProbeTrace& trace) noexcept {
    if (!validUtf8(trace.platform)) return false;
    if ((trace.architectureBits != 32 && trace.architectureBits != 64) ||
        (trace.rawInputBufferAlignmentBytes != 4 &&
         trace.rawInputBufferAlignmentBytes != 8) ||
        (trace.architectureBits == 64 &&
         trace.rawInputBufferAlignmentBytes != 8)) return false;
    const std::size_t totalEvents = trace.registrationEvents.size() +
        trace.messageEvents.size() + trace.dataEvents.size() +
        trace.bufferEvents.size() + trace.observations.size();
    if (totalEvents > kMaxTraceEvents) return false;
    for (const auto& event : trace.registrationEvents) {
        if (event.before.registrations.size() > kMaxRawRegistrations ||
            event.after.registrations.size() > kMaxRawRegistrations) return false;
    }
    for (const auto& event : trace.messageEvents) {
        if (!headerTextValid(event.headerQuery.header) ||
            !headerTextValid(event.inputQueryAndRead.header) ||
            event.headerQuery.totalPayloadBytes > kMaxRawPacketBytes ||
            event.inputQueryAndRead.totalPayloadBytes > kMaxRawPacketBytes) return false;
    }
    for (const auto& event : trace.dataEvents) {
        if (!headerTextValid(event.header) ||
            event.totalPayloadBytes > kMaxRawPacketBytes) return false;
    }
    for (const auto& event : trace.bufferEvents) {
        if (event.requestedBufferBytes > kMaxRawPacketBytes ||
            event.blocks.size() > kMaxRawBufferPackets) return false;
        for (const auto& block : event.blocks) {
            if (!headerTextValid(block.header)) return false;
        }
    }
    for (const auto& observation : trace.observations) {
        if (!validUtf8(observation.name) || !validUtf8(observation.detail)) return false;
    }
    return true;
}

void appendContext(std::ostringstream& output, const RawEventContext& value) {
    output << "{\"sequence\":" << value.sequence
           << ",\"monotonic_timestamp_micros\":"
           << value.monotonicTimestampMicros << ",\"thread_id\":"
           << value.threadId << '}';
}

void appendApi(std::ostringstream& output, const RawApiResult& value) {
    output << "{\"context\":";
    appendContext(output, value.context);
    output << ",\"kind\":" << static_cast<unsigned>(value.kind)
           << ",\"success\":" << (value.success ? "true" : "false")
           << ",\"result_value\":" << value.resultValue
           << ",\"system_error\":" << value.systemError
           << ",\"pcb_size_before\":" << value.pcbSizeBefore
           << ",\"pcb_size_after\":" << value.pcbSizeAfter
           << ",\"cb_size_header\":" << value.cbSizeHeader
           << ",\"reported_size\":" << value.reportedSize
           << ",\"returned_size\":" << value.returnedSize << '}';
}

void appendRegistration(std::ostringstream& output,
                        const RawRegistrationDescriptor& value) {
    output << "{\"usage_page\":" << value.usagePage
           << ",\"usage\":" << value.usage << ",\"flags\":" << value.flags
           << ",\"target_window_runtime_value\":"
           << value.targetWindowRuntimeValue << '}';
}

void appendSnapshot(std::ostringstream& output,
                    const RawRegistrationSnapshot& value) {
    output << "{\"api\":";
    appendApi(output, value.api);
    output << ",\"size_query\":";
    appendApi(output, value.sizeQuery);
    output << ",\"read\":";
    appendApi(output, value.read);
    output << ",\"reported_device_count\":" << value.reportedDeviceCount
           << ",\"registrations\":[";
    auto registrations = value.registrations;
    std::sort(registrations.begin(), registrations.end(),
              [](const RawRegistrationDescriptor& left,
                 const RawRegistrationDescriptor& right) {
                  if (left.usagePage != right.usagePage) {
                      return left.usagePage < right.usagePage;
                  }
                  if (left.usage != right.usage) {
                      return left.usage < right.usage;
                  }
                  if (left.flags != right.flags) {
                      return left.flags < right.flags;
                  }
                  return left.targetWindowRuntimeValue <
                         right.targetWindowRuntimeValue;
              });
    for (std::size_t index = 0; index < registrations.size(); ++index) {
        if (index != 0) output << ',';
        appendRegistration(output, registrations[index]);
    }
    output << "]}";
}

void appendHeader(std::ostringstream& output, const RawHeaderObservation& value) {
    output << "{\"available\":" << (value.available ? "true" : "false")
           << ",\"dw_type\":" << value.dwType << ",\"dw_size\":"
           << value.dwSize << ",\"device_runtime_value\":"
           << value.deviceRuntimeValue << ",\"wparam_runtime_value\":"
           << value.wParamRuntimeValue << ",\"device_path\":";
    quote(output, value.devicePath);
    output << ",\"stable_device_id\":";
    quote(output, value.stableDeviceId);
    output << '}';
}

void appendData(std::ostringstream& output, const RawDataQueryEvent& value) {
    output << "{\"context\":";
    appendContext(output, value.context);
    output << ",\"ui_command\":" << value.uiCommand << ",\"query\":";
    appendApi(output, value.query);
    output << ",\"read\":";
    appendApi(output, value.read);
    output << ",\"header\":";
    appendHeader(output, value.header);
    output << ",\"total_payload_bytes\":" << value.totalPayloadBytes << '}';
}

RawEventContext parseContext(const JsonValue& value) {
    const auto& object = objectOf(value);
    RawEventContext result;
    result.sequence = unsignedOf(required(object, "sequence"));
    result.monotonicTimestampMicros =
        unsignedOf(required(object, "monotonic_timestamp_micros"));
    result.threadId = boundedUnsignedOf<std::uint32_t>(required(object, "thread_id"));
    return result;
}

RawApiResult parseApi(const JsonValue& value) {
    const auto& object = objectOf(value);
    RawApiResult result;
    result.context = parseContext(required(object, "context"));
    result.kind = enumOf<RawProbeResultKind>(required(object, "kind"), 1, 9);
    result.success = boolOf(required(object, "success"));
    result.resultValue = unsignedOf(required(object, "result_value"));
    result.systemError = boundedUnsignedOf<std::uint32_t>(required(object, "system_error"));
    result.pcbSizeBefore = boundedUnsignedOf<std::uint32_t>(required(object, "pcb_size_before"));
    result.pcbSizeAfter = boundedUnsignedOf<std::uint32_t>(required(object, "pcb_size_after"));
    result.cbSizeHeader = boundedUnsignedOf<std::uint32_t>(required(object, "cb_size_header"));
    result.reportedSize = boundedUnsignedOf<std::uint32_t>(required(object, "reported_size"));
    result.returnedSize = boundedUnsignedOf<std::uint32_t>(required(object, "returned_size"));
    return result;
}

RawRegistrationDescriptor parseRegistration(const JsonValue& value) {
    const auto& object = objectOf(value);
    RawRegistrationDescriptor result;
    result.usagePage = boundedUnsignedOf<std::uint16_t>(required(object, "usage_page"));
    result.usage = boundedUnsignedOf<std::uint16_t>(required(object, "usage"));
    result.flags = boundedUnsignedOf<std::uint32_t>(required(object, "flags"));
    result.targetWindowRuntimeValue = unsignedOf(required(object, "target_window_runtime_value"));
    return result;
}

RawRegistrationSnapshot parseSnapshot(const JsonValue& value) {
    const auto& object = objectOf(value);
    RawRegistrationSnapshot result;
    result.api = parseApi(required(object, "api"));
    result.sizeQuery = parseApi(required(object, "size_query"));
    result.read = parseApi(required(object, "read"));
    result.reportedDeviceCount = boundedUnsignedOf<std::uint32_t>(
        required(object, "reported_device_count"));
    const auto& registrations = arrayOf(required(object, "registrations"));
    if (registrations.size() > kMaxRawRegistrations) {
        throw std::runtime_error("registration count exceeds limit");
    }
    for (const auto& registration : registrations) {
        result.registrations.push_back(parseRegistration(registration));
    }
    return result;
}

RawHeaderObservation parseHeader(const JsonValue& value) {
    const auto& object = objectOf(value);
    RawHeaderObservation result;
    result.available = boolOf(required(object, "available"));
    result.dwType = boundedUnsignedOf<std::uint32_t>(required(object, "dw_type"));
    result.dwSize = boundedUnsignedOf<std::uint32_t>(required(object, "dw_size"));
    result.deviceRuntimeValue = unsignedOf(required(object, "device_runtime_value"));
    result.wParamRuntimeValue = unsignedOf(required(object, "wparam_runtime_value"));
    result.devicePath = stringOf(required(object, "device_path"));
    result.stableDeviceId = stringOf(required(object, "stable_device_id"));
    if (result.devicePath.size() > kMaxFixtureBytes ||
        result.stableDeviceId.size() > kMaxFixtureBytes) {
        throw std::runtime_error("header diagnostic string exceeds limit");
    }
    return result;
}

RawDataQueryEvent parseData(const JsonValue& value) {
    const auto& object = objectOf(value);
    RawDataQueryEvent result;
    result.context = parseContext(required(object, "context"));
    result.uiCommand = boundedUnsignedOf<std::uint32_t>(required(object, "ui_command"));
    result.query = parseApi(required(object, "query"));
    result.read = parseApi(required(object, "read"));
    result.header = parseHeader(required(object, "header"));
    result.totalPayloadBytes = boundedUnsignedOf<std::uint32_t>(required(object, "total_payload_bytes"));
    if (result.totalPayloadBytes > kMaxRawPacketBytes) {
        throw std::runtime_error("raw packet exceeds limit");
    }
    return result;
}

template <typename T, typename Parser>
std::vector<T> parseBoundedArray(const JsonValue& value, std::size_t maximum,
                                 Parser parser, const char* error) {
    const auto& array = arrayOf(value);
    if (array.size() > maximum) {
        throw std::runtime_error(error);
    }
    std::vector<T> result;
    result.reserve(array.size());
    for (const auto& entry : array) {
        result.push_back(parser(entry));
    }
    return result;
}

std::uint32_t readU32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("truncated raw buffer header");
    }
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1])) << 8u) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2])) << 16u) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3])) << 24u);
}

std::uint64_t readRuntimeValue(std::span<const std::byte> bytes,
                               std::size_t offset, std::size_t width) {
    if (width != 4 && width != 8) {
        throw std::runtime_error("invalid runtime handle width");
    }
    if (offset > bytes.size() || bytes.size() - offset < width) {
        throw std::runtime_error("truncated runtime handle");
    }
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < width; ++index) {
        result |= static_cast<std::uint64_t>(
                      std::to_integer<unsigned char>(bytes[offset + index]))
                  << (index * 8u);
    }
    return result;
}

} // namespace

std::string_view rawProbeSourceKindName(RawProbeSourceKind value) noexcept {
    switch (value) {
    case RawProbeSourceKind::SyntheticParserFixture: return "synthetic_parser_fixture";
    case RawProbeSourceKind::ObservedWindowsApi: return "observed_windows_api";
    case RawProbeSourceKind::SyntheticOsInputExperiment: return "synthetic_os_input_experiment";
    }
    return "unknown";
}

std::string_view rawProbeOperationName(RawProbeOperation value) noexcept {
    switch (value) {
    case RawProbeOperation::Snapshot: return "snapshot";
    case RawProbeOperation::RegisterKeyboard: return "register_keyboard";
    case RawProbeOperation::RegisterMouse: return "register_mouse";
    case RawProbeOperation::ReplaceKeyboardTarget: return "replace_keyboard_target";
    case RawProbeOperation::ReplaceMouseTarget: return "replace_mouse_target";
    case RawProbeOperation::RegisterInputSink: return "register_input_sink";
    case RawProbeOperation::RegisterDeviceNotify: return "register_device_notify";
    case RawProbeOperation::RegisterBackgroundDeviceNotify: return "register_background_device_notify";
    case RawProbeOperation::RemoveKeyboard: return "remove_keyboard";
    case RawProbeOperation::RemoveMouse: return "remove_mouse";
    case RawProbeOperation::DestroyTargetWindow: return "destroy_target_window";
    case RawProbeOperation::ReplaceDestroyedTarget: return "replace_destroyed_target";
    case RawProbeOperation::Cleanup: return "cleanup";
    }
    return "unknown";
}

std::string_view rawProbeResultKindName(RawProbeResultKind value) noexcept {
    switch (value) {
    case RawProbeResultKind::Success: return "success";
    case RawProbeResultKind::ApiFailure: return "api_failure";
    case RawProbeResultKind::InvalidContract: return "invalid_contract";
    case RawProbeResultKind::Truncated: return "truncated";
    case RawProbeResultKind::Oversized: return "oversized";
    case RawProbeResultKind::UnsupportedType: return "unsupported_type";
    case RawProbeResultKind::SizeMismatch: return "size_mismatch";
    case RawProbeResultKind::BoundsExceeded: return "bounds_exceeded";
    case RawProbeResultKind::NotObserved: return "not_observed";
    }
    return "unknown";
}

std::string serializeRawInputProbeTrace(const RawInputProbeTrace& trace) {
    if (trace.schemaVersion != kRawInputProbeSchemaVersion) {
        return {};
    }
    if (!traceBoundsAndTextValid(trace)) {
        return {};
    }
    std::ostringstream output;
    output << "{\"schema_version\":" << trace.schemaVersion
           << ",\"platform\":";
    quote(output, trace.platform);
    output << ",\"source_kind\":" << static_cast<unsigned>(trace.sourceKind)
           << ",\"source_kind_name\":";
    quote(output, rawProbeSourceKindName(trace.sourceKind));
    output
           << ",\"architecture_bits\":" << trace.architectureBits
           << ",\"process_id\":" << trace.processId
           << ",\"thread_id\":" << trace.threadId
           << ",\"native_sizes\":{\"raw_input_header_bytes\":"
           << trace.rawInputHeaderBytes << ",\"raw_input_bytes\":"
           << trace.rawInputBytes << ",\"raw_input_buffer_alignment_bytes\":"
           << trace.rawInputBufferAlignmentBytes
           << "},\"limits\":{\"maximum_registrations\":"
           << trace.limits.maximumRegistrations
           << ",\"maximum_trace_events\":" << trace.limits.maximumTraceEvents
           << ",\"maximum_raw_packet_bytes\":" << trace.limits.maximumRawPacketBytes
           << ",\"maximum_raw_buffer_packets\":" << trace.limits.maximumRawBufferPackets
           << ",\"maximum_fixture_bytes\":" << trace.limits.maximumFixtureBytes
           << "},\"physical_input_observed\":"
           << (trace.physicalInputObserved ? "true" : "false")
           << ",\"device_change_observed\":"
           << (trace.deviceChangeObserved ? "true" : "false")
           << ",\"trace_overflowed\":"
           << (trace.traceOverflowed ? "true" : "false")
           << ",\"registration_events\":[";
    for (std::size_t index = 0; index < trace.registrationEvents.size(); ++index) {
        if (index != 0) output << ',';
        const auto& event = trace.registrationEvents[index];
        output << "{\"context\":";
        appendContext(output, event.context);
        output << ",\"operation\":" << static_cast<unsigned>(event.operation)
               << ",\"operation_name\":";
        quote(output, rawProbeOperationName(event.operation));
        output
               << ",\"request\":";
        appendRegistration(output, event.request);
        output << ",\"call\":";
        appendApi(output, event.call);
        output << ",\"before\":";
        appendSnapshot(output, event.before);
        output << ",\"after\":";
        appendSnapshot(output, event.after);
        output << '}';
    }
    output << "],\"message_events\":[";
    for (std::size_t index = 0; index < trace.messageEvents.size(); ++index) {
        if (index != 0) output << ',';
        const auto& event = trace.messageEvents[index];
        output << "{\"context\":";
        appendContext(output, event.context);
        output << ",\"message_kind\":" << static_cast<unsigned>(event.messageKind)
               << ",\"message_id\":" << event.messageId
               << ",\"message_time_milliseconds\":"
               << event.messageTimeMilliseconds
               << ",\"window_runtime_value\":" << event.windowRuntimeValue
               << ",\"wparam_runtime_value\":" << event.wParamRuntimeValue
               << ",\"lparam_runtime_value\":" << event.lParamRuntimeValue
               << ",\"input_code\":" << static_cast<unsigned>(event.inputCode)
               << ",\"device_change\":" << static_cast<unsigned>(event.deviceChange)
               << ",\"header_query\":";
        appendData(output, event.headerQuery);
        output << ",\"input_query_and_read\":";
        appendData(output, event.inputQueryAndRead);
        output << '}';
    }
    output << "],\"data_events\":[";
    for (std::size_t index = 0; index < trace.dataEvents.size(); ++index) {
        if (index != 0) output << ',';
        appendData(output, trace.dataEvents[index]);
    }
    output << "],\"buffer_events\":[";
    for (std::size_t index = 0; index < trace.bufferEvents.size(); ++index) {
        if (index != 0) output << ',';
        const auto& event = trace.bufferEvents[index];
        output << "{\"context\":";
        appendContext(output, event.context);
        output << ",\"requested_buffer_bytes\":" << event.requestedBufferBytes
               << ",\"call\":";
        appendApi(output, event.call);
        output << ",\"returned_raw_input_count\":" << event.returnedRawInputCount
               << ",\"blocks\":[";
        for (std::size_t blockIndex = 0; blockIndex < event.blocks.size(); ++blockIndex) {
            if (blockIndex != 0) output << ',';
            const auto& block = event.blocks[blockIndex];
            output << "{\"offset\":" << block.offset
                   << ",\"aligned_next_offset\":" << block.alignedNextOffset
                   << ",\"header\":";
            appendHeader(output, block.header);
            output << '}';
        }
        output << "]}";
    }
    output << "],\"observations\":[";
    for (std::size_t index = 0; index < trace.observations.size(); ++index) {
        if (index != 0) output << ',';
        const auto& observation = trace.observations[index];
        output << "{\"name\":";
        quote(output, observation.name);
        output << ",\"result\":" << static_cast<unsigned>(observation.result)
               << ",\"detail\":";
        quote(output, observation.detail);
        output << '}';
    }
    output << "]}";
    std::string result = output.str();
    if (result.size() > kMaxFixtureBytes) {
        return {};
    }
    return result;
}

RawTraceParseResult parseRawInputProbeTrace(std::string_view json) {
    RawTraceParseResult result;
    if (json.empty()) {
        result.error = "trace is empty";
        return result;
    }
    if (json.size() > kMaxFixtureBytes) {
        result.error = "trace exceeds maximum fixture bytes";
        return result;
    }
    try {
        const auto rootValue = JsonParser(json).parse();
        const auto& root = objectOf(rootValue);
        RawInputProbeTrace trace;
        trace.schemaVersion = boundedUnsignedOf<std::uint32_t>(required(root, "schema_version"));
        if (trace.schemaVersion != kRawInputProbeSchemaVersion) {
            throw std::runtime_error("unsupported trace schema version");
        }
        trace.platform = stringOf(required(root, "platform"));
        trace.sourceKind = enumOf<RawProbeSourceKind>(required(root, "source_kind"), 1, 3);
        if (stringOf(required(root, "source_kind_name")) !=
            rawProbeSourceKindName(trace.sourceKind)) {
            throw std::runtime_error("trace source kind name does not match value");
        }
        trace.architectureBits = boundedUnsignedOf<std::uint16_t>(required(root, "architecture_bits"));
        if (trace.architectureBits != 32 && trace.architectureBits != 64) {
            throw std::runtime_error("invalid trace architecture");
        }
        trace.processId = boundedUnsignedOf<std::uint32_t>(required(root, "process_id"));
        trace.threadId = boundedUnsignedOf<std::uint32_t>(required(root, "thread_id"));
        const auto& nativeSizes = objectOf(required(root, "native_sizes"));
        trace.rawInputHeaderBytes = boundedUnsignedOf<std::uint32_t>(required(nativeSizes, "raw_input_header_bytes"));
        trace.rawInputBytes = boundedUnsignedOf<std::uint32_t>(required(nativeSizes, "raw_input_bytes"));
        trace.rawInputBufferAlignmentBytes = boundedUnsignedOf<std::uint32_t>(
            required(nativeSizes, "raw_input_buffer_alignment_bytes"));
        if (trace.rawInputBufferAlignmentBytes != 4 &&
            trace.rawInputBufferAlignmentBytes != 8) {
            throw std::runtime_error("invalid raw input buffer alignment");
        }
        const auto& limits = objectOf(required(root, "limits"));
        trace.limits.maximumRegistrations = boundedUnsignedOf<std::uint32_t>(required(limits, "maximum_registrations"));
        trace.limits.maximumTraceEvents = boundedUnsignedOf<std::uint32_t>(required(limits, "maximum_trace_events"));
        trace.limits.maximumRawPacketBytes = boundedUnsignedOf<std::uint32_t>(required(limits, "maximum_raw_packet_bytes"));
        trace.limits.maximumRawBufferPackets = boundedUnsignedOf<std::uint32_t>(required(limits, "maximum_raw_buffer_packets"));
        trace.limits.maximumFixtureBytes = boundedUnsignedOf<std::uint32_t>(required(limits, "maximum_fixture_bytes"));
        if (trace.limits != RawTraceLimits{}) {
            throw std::runtime_error("trace limits do not match schema v1");
        }
        trace.physicalInputObserved = boolOf(required(root, "physical_input_observed"));
        trace.deviceChangeObserved = boolOf(required(root, "device_change_observed"));
        trace.traceOverflowed = boolOf(required(root, "trace_overflowed"));

        trace.registrationEvents = parseBoundedArray<RawRegistrationEvent>(
            required(root, "registration_events"), kMaxTraceEvents,
            [](const JsonValue& value) {
                const auto& object = objectOf(value);
                RawRegistrationEvent event;
                event.context = parseContext(required(object, "context"));
                event.operation = enumOf<RawProbeOperation>(required(object, "operation"), 1, 13);
                if (stringOf(required(object, "operation_name")) !=
                    rawProbeOperationName(event.operation)) {
                    throw std::runtime_error("registration operation name does not match value");
                }
                event.request = parseRegistration(required(object, "request"));
                event.call = parseApi(required(object, "call"));
                event.before = parseSnapshot(required(object, "before"));
                event.after = parseSnapshot(required(object, "after"));
                return event;
            }, "registration event count exceeds limit");

        trace.messageEvents = parseBoundedArray<RawMessageEvent>(
            required(root, "message_events"), kMaxTraceEvents,
            [](const JsonValue& value) {
                const auto& object = objectOf(value);
                RawMessageEvent event;
                event.context = parseContext(required(object, "context"));
                event.messageKind = enumOf<RawMessageKind>(required(object, "message_kind"), 1, 2);
                event.messageId = boundedUnsignedOf<std::uint32_t>(required(object, "message_id"));
                event.messageTimeMilliseconds = boundedUnsignedOf<std::uint32_t>(
                    required(object, "message_time_milliseconds"));
                event.windowRuntimeValue = unsignedOf(required(object, "window_runtime_value"));
                event.wParamRuntimeValue = unsignedOf(required(object, "wparam_runtime_value"));
                event.lParamRuntimeValue = unsignedOf(required(object, "lparam_runtime_value"));
                event.inputCode = enumOf<RawInputCodeKind>(required(object, "input_code"), 1, 3);
                event.deviceChange = enumOf<RawDeviceChangeKind>(required(object, "device_change"), 1, 3);
                event.headerQuery = parseData(required(object, "header_query"));
                event.inputQueryAndRead = parseData(required(object, "input_query_and_read"));
                return event;
            }, "message event count exceeds limit");

        trace.dataEvents = parseBoundedArray<RawDataQueryEvent>(
            required(root, "data_events"), kMaxTraceEvents,
            [](const JsonValue& value) { return parseData(value); },
            "data event count exceeds limit");

        trace.bufferEvents = parseBoundedArray<RawBufferQueryEvent>(
            required(root, "buffer_events"), kMaxTraceEvents,
            [](const JsonValue& value) {
                const auto& object = objectOf(value);
                RawBufferQueryEvent event;
                event.context = parseContext(required(object, "context"));
                event.requestedBufferBytes = boundedUnsignedOf<std::uint32_t>(required(object, "requested_buffer_bytes"));
                if (event.requestedBufferBytes > kMaxRawPacketBytes) {
                    throw std::runtime_error("raw buffer byte request exceeds limit");
                }
                event.call = parseApi(required(object, "call"));
                event.returnedRawInputCount = boundedUnsignedOf<std::uint32_t>(required(object, "returned_raw_input_count"));
                event.blocks = parseBoundedArray<RawBufferBlock>(
                    required(object, "blocks"), kMaxRawBufferPackets,
                    [](const JsonValue& blockValue) {
                        const auto& blockObject = objectOf(blockValue);
                        RawBufferBlock block;
                        block.offset = boundedUnsignedOf<std::uint32_t>(required(blockObject, "offset"));
                        block.alignedNextOffset = boundedUnsignedOf<std::uint32_t>(required(blockObject, "aligned_next_offset"));
                        block.header = parseHeader(required(blockObject, "header"));
                        return block;
                    }, "raw buffer packet count exceeds limit");
                return event;
            }, "buffer event count exceeds limit");

        trace.observations = parseBoundedArray<RawProbeObservation>(
            required(root, "observations"), kMaxTraceEvents,
            [](const JsonValue& value) {
                const auto& object = objectOf(value);
                RawProbeObservation observation;
                observation.name = stringOf(required(object, "name"));
                observation.result = enumOf<RawProbeResultKind>(required(object, "result"), 1, 9);
                observation.detail = stringOf(required(object, "detail"));
                return observation;
            }, "observation count exceeds limit");

        const std::size_t totalEvents = trace.registrationEvents.size() +
            trace.messageEvents.size() + trace.dataEvents.size() +
            trace.bufferEvents.size() + trace.observations.size();
        if (totalEvents > kMaxTraceEvents) {
            throw std::runtime_error("total trace event count exceeds limit");
        }
        result.trace = std::move(trace);
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}

RawProbeResultKind validateRawDataContract(
    const RawDataContractInput& input) noexcept {
    constexpr std::uint32_t kApiError = std::numeric_limits<std::uint32_t>::max();
    if (input.queryReturnValue == kApiError || input.readReturnValue == kApiError) {
        return RawProbeResultKind::ApiFailure;
    }
    if (input.headerBytes == 0 || input.maximumPacketBytes < input.headerBytes) {
        return RawProbeResultKind::InvalidContract;
    }
    if (input.querySizeAfter == 0 || input.rawDwSize == 0) {
        return RawProbeResultKind::InvalidContract;
    }
    if (input.querySizeAfter > input.maximumPacketBytes ||
        input.suppliedBufferBytes > input.maximumPacketBytes ||
        input.rawDwSize > input.maximumPacketBytes) {
        return RawProbeResultKind::Oversized;
    }
    if (input.querySizeAfter < input.headerBytes ||
        input.suppliedBufferBytes < input.headerBytes ||
        input.rawDwSize < input.headerBytes) {
        return RawProbeResultKind::Truncated;
    }
    if (input.suppliedBufferBytes < input.querySizeAfter ||
        input.readReturnValue > input.suppliedBufferBytes) {
        return RawProbeResultKind::Truncated;
    }
    if (input.readReturnValue != input.readSizeAfter ||
        input.readReturnValue != input.querySizeAfter ||
        input.rawDwSize != input.readReturnValue) {
        return RawProbeResultKind::SizeMismatch;
    }
    if (input.rawDwType > 2u) {
        return RawProbeResultKind::UnsupportedType;
    }
    return RawProbeResultKind::Success;
}

RawProbeResultKind validateRawRegistrationContract(
    const RawRegistrationContractInput& input) noexcept {
    constexpr std::uint32_t kRidevRemove = 0x00000001u;
    if (input.nativeCbSize == 0 || input.cbSize != input.nativeCbSize) {
        return RawProbeResultKind::InvalidContract;
    }
    const bool supportedUsage = input.request.usagePage == 0x01u &&
        (input.request.usage == 0x02u || input.request.usage == 0x06u);
    if (!supportedUsage) {
        return RawProbeResultKind::InvalidContract;
    }
    const bool remove = (input.request.flags & kRidevRemove) != 0;
    if (remove) {
        if (input.request.flags != kRidevRemove ||
            input.request.targetWindowRuntimeValue != 0) {
            return RawProbeResultKind::InvalidContract;
        }
    } else if (input.controlledTargetRequired &&
               input.request.targetWindowRuntimeValue == 0) {
        return RawProbeResultKind::InvalidContract;
    }
    return RawProbeResultKind::Success;
}

RawBufferParseResult parseRawInputBufferLayout(
    std::span<const std::byte> bytes,
    std::uint32_t returnedPacketCount,
    std::uint16_t architectureBits,
    std::uint32_t headerBytes,
    std::uint32_t alignmentBytes) {
    RawBufferParseResult result;
    if (bytes.size() > kMaxRawPacketBytes) {
        result.kind = RawProbeResultKind::Oversized;
        result.error = "raw input buffer exceeds byte limit";
        return result;
    }
    if (returnedPacketCount > kMaxRawBufferPackets) {
        result.kind = RawProbeResultKind::BoundsExceeded;
        result.error = "raw input buffer exceeds packet-count limit";
        return result;
    }
    if (returnedPacketCount == 0) {
        result.kind = RawProbeResultKind::Success;
        return result;
    }
    const std::size_t nativeAlignment = architectureBits == 32 ? 4u :
                                        architectureBits == 64 ? 8u : 0u;
    const std::size_t alignment = alignmentBytes == 0
        ? nativeAlignment : static_cast<std::size_t>(alignmentBytes);
    const std::size_t expectedHeaderBytes = architectureBits == 32 ? 16u : 24u;
    const bool validAlignment = architectureBits == 64
        ? alignment == 8u
        : architectureBits == 32 && (alignment == 4u || alignment == 8u);
    if (nativeAlignment == 0 || !validAlignment ||
        headerBytes != expectedHeaderBytes) {
        result.kind = RawProbeResultKind::InvalidContract;
        result.error = "raw input header size does not match architecture";
        return result;
    }
    std::size_t offset = 0;
    try {
        for (std::uint32_t index = 0; index < returnedPacketCount; ++index) {
            if (offset > bytes.size() || bytes.size() - offset < headerBytes) {
                result.kind = RawProbeResultKind::Truncated;
                result.error = "raw input buffer header is truncated";
                return result;
            }
            const std::uint32_t dwType = readU32(bytes, offset);
            const std::uint32_t dwSize = readU32(bytes, offset + 4);
            if (dwSize < headerBytes) {
                result.kind = RawProbeResultKind::InvalidContract;
                result.error = "raw input block does not make progress";
                return result;
            }
            if (dwSize > kMaxRawPacketBytes ||
                static_cast<std::size_t>(dwSize) > bytes.size() - offset) {
                result.kind = dwSize > kMaxRawPacketBytes
                                  ? RawProbeResultKind::Oversized
                                  : RawProbeResultKind::Truncated;
                result.error = "raw input block exceeds buffer bounds";
                return result;
            }
            if (static_cast<std::size_t>(dwSize) >
                std::numeric_limits<std::size_t>::max() - (alignment - 1u)) {
                result.kind = RawProbeResultKind::BoundsExceeded;
                result.error = "raw input block alignment overflows";
                return result;
            }
            const std::size_t alignedSize =
                (static_cast<std::size_t>(dwSize) + alignment - 1u) &
                ~(alignment - 1u);
            if (alignedSize == 0 || offset >
                std::numeric_limits<std::size_t>::max() - alignedSize) {
                result.kind = RawProbeResultKind::BoundsExceeded;
                result.error = "raw input next-block offset overflows";
                return result;
            }
            const std::size_t nextOffset = offset + alignedSize;
            if (index + 1u < returnedPacketCount && nextOffset > bytes.size()) {
                result.kind = RawProbeResultKind::Truncated;
                result.error = "raw input next block is outside buffer";
                return result;
            }
            RawBufferBlock block;
            block.offset = static_cast<std::uint32_t>(offset);
            block.alignedNextOffset = static_cast<std::uint32_t>(nextOffset);
            block.header.available = true;
            block.header.dwType = dwType;
            block.header.dwSize = dwSize;
            block.header.deviceRuntimeValue = readRuntimeValue(
                bytes, offset + 8, nativeAlignment);
            block.header.wParamRuntimeValue = readRuntimeValue(
                bytes, offset + 8 + nativeAlignment, nativeAlignment);
            result.blocks.push_back(block);
            offset = nextOffset;
        }
    } catch (const std::exception& error) {
        result.kind = RawProbeResultKind::Truncated;
        result.error = error.what();
        return result;
    }
    result.kind = RawProbeResultKind::Success;
    return result;
}

} // namespace hydra::rawprobe
