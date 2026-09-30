#pragma once

#include "hydra/hidhide_session_backend.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::test {

enum class SyntheticPhase3EvidenceMode {
    Accepted,
    Pending,
    GatePending,
    Stale,
    ReceiverMissing,
    ScopeMismatch,
};

class SyntheticPhase3EvidenceFixture final {
public:
    explicit SyntheticPhase3EvidenceFixture(
        SyntheticPhase3EvidenceMode mode = SyntheticPhase3EvidenceMode::Accepted)
        : root_(makeRoot()),
          profilePath_(root_ / "workspace_config.json"),
          sharedProfilePath_(root_ / "shared-case-profile.json"),
          gateATracePath_(root_ / "gate-a.jsonl"),
          gateBTracePath_(root_ / "gate-b.jsonl"),
          gateBSharedTracePath_(root_ / "gate-b-shared.jsonl"),
          gateCTracePath_(root_ / "gate-c.jsonl"),
          metricsPath_(root_ / "gate-c-metrics.json"),
          manifestPath_(root_ / "phase3-hardware-manifest.json") {
        writeText(profilePath_, kProfileJson);
        writeText(sharedProfilePath_, kSharedProfileJson);
        writeText(gateATracePath_, kGateATrace);
        writeText(gateBTracePath_, kGateBTrace);
        writeText(gateBSharedTracePath_, kGateBSharedTrace);
        writeText(gateCTracePath_, kGateCTrace);
        writeText(metricsPath_,
                  mode == SyntheticPhase3EvidenceMode::ReceiverMissing
                      ? kReceiverMissingMetricsJson
                      : kMetricsJson);

        const auto now = currentUnixSeconds();
        writeText(manifestPath_, manifestJson(mode, now));
        loadResult_ = loadPhase3HardwareAcceptanceEvidence(manifestPath_, now);
    }

    ~SyntheticPhase3EvidenceFixture() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    SyntheticPhase3EvidenceFixture(const SyntheticPhase3EvidenceFixture&) = delete;
    SyntheticPhase3EvidenceFixture& operator=(const SyntheticPhase3EvidenceFixture&) = delete;

    const Phase3HardwareEvidenceLoadResult& loadResult() const noexcept {
        return loadResult_;
    }

    const Phase3HardwareAcceptanceEvidence& evidence() const {
        if (!loadResult_.accepted()) {
            throw std::logic_error("synthetic P3-HW fixture did not produce accepted typed evidence: " +
                                   loadResult_.diagnostic);
        }
        return *loadResult_.evidence;
    }

    const std::filesystem::path& manifestPath() const noexcept { return manifestPath_; }

    void tamperGateCTrace() {
        std::ofstream output(gateCTracePath_, std::ios::binary | std::ios::app);
        if (!output.is_open()) throw std::runtime_error("could not tamper synthetic Gate C trace");
        output << '\n';
        if (!output) throw std::runtime_error("could not append synthetic Gate C tamper byte");
    }

    void tamperProfile() {
        std::ofstream output(profilePath_, std::ios::binary | std::ios::app);
        if (!output.is_open()) throw std::runtime_error("could not tamper synthetic P3-HW profile");
        output << '\n';
        if (!output) throw std::runtime_error("could not append synthetic profile tamper byte");
    }

    void tamperManifest() {
        std::ofstream output(manifestPath_, std::ios::binary | std::ios::app);
        if (!output.is_open()) throw std::runtime_error("could not tamper synthetic P3-HW manifest");
        output << '\n';
        if (!output) throw std::runtime_error("could not append synthetic manifest tamper byte");
    }

    static std::vector<std::wstring> requestedDeviceInstanceIds() {
        return {
            L"HID\\VID_1111&PID_0001\\K1",
            L"HID\\VID_1111&PID_0002\\M1",
            L"HID\\VID_2222&PID_0001\\K2",
            L"HID\\VID_2222&PID_0002\\M2",
        };
    }

private:
    static constexpr std::string_view kProfileSha256 =
        "865919fb160b1887e9cf0e3a89d3bc56923d2cea0af0241bd4766b6d81b6d93a";
    static constexpr std::string_view kSharedProfileSha256 =
        "81f14f29173685f293132cd155bb2e7d873758d4df234a9d15452e1f8d247dfb";
    static constexpr std::string_view kGateATraceSha256 =
        "1ee330f6bdd3722c679ba2f0d68677bec43912153c78ba88f38b26c2cf8c2990";
    static constexpr std::string_view kGateBTraceSha256 =
        "0d4779f04a2d0d4275f66d1b587c6a71b5487428f13323ef8110a2e5231026fa";
    static constexpr std::string_view kGateBSharedTraceSha256 =
        "0ca585a89ac87c16cfba38f786390301b58ad223c4b3b5e9686d5c21cdaf9631";
    static constexpr std::string_view kGateCTraceSha256 = kGateBTraceSha256;
    static constexpr std::string_view kMetricsSha256 =
        "adf01a1debe2731d46b2d97a0d54bc286460db9f3aa2c21a4421cc868efef7c1";
    static constexpr std::string_view kReceiverMissingMetricsSha256 =
        "986097bde9568484e469bd16a357bf509c4d535cd63df42030661cf1746df988";

    static constexpr std::string_view kProfileJson =
        R"json({"schema_version":2,"shareable_resources":[],"seats":[{"id":1,"active":true,"keyboards":["Keyboard:HID\\VID_1111&PID_0001\\K1"],"mice":["Mouse:HID\\VID_1111&PID_0002\\M1"]},{"id":2,"active":true,"keyboards":["Keyboard:HID\\VID_2222&PID_0001\\K2"],"mice":["Mouse:HID\\VID_2222&PID_0002\\M2"]}]})json";
    static constexpr std::string_view kSharedProfileJson =
        R"json({"schema_version":2,"fixture":"ambiguous-shared-case"})json";
    static constexpr std::string_view kGateATrace =
        R"json({"record":"input","device_id":"Keyboard:HID\\VID_1111&PID_0001\\K1","route":"UnassignedDevice","physical_suppression_requested":false,"isolation_guarantee":"diagnostic_route_only_native_os_input_not_suppressed"}
{"record":"input","device_id":"Mouse:HID\\VID_1111&PID_0002\\M1","route":"UnassignedDevice","physical_suppression_requested":false,"isolation_guarantee":"diagnostic_route_only_native_os_input_not_suppressed"}
{"record":"input","device_id":"Keyboard:HID\\VID_2222&PID_0001\\K2","route":"UnassignedDevice","physical_suppression_requested":false,"isolation_guarantee":"diagnostic_route_only_native_os_input_not_suppressed"}
{"record":"input","device_id":"Mouse:HID\\VID_2222&PID_0002\\M2","route":"UnassignedDevice","physical_suppression_requested":false,"isolation_guarantee":"diagnostic_route_only_native_os_input_not_suppressed"}
{"record":"device_change","device_id":"Keyboard:HID\\VID_1111&PID_0001\\K1","change":"Removal"}
{"record":"device_change","device_id":"Keyboard:HID\\VID_1111&PID_0001\\K1","change":"Arrival"}
)json";
    static constexpr std::string_view kGateBTrace =
        R"json({"record":"input","device_id":"Keyboard:HID\\VID_1111&PID_0001\\K1","route":"Routed","seat_id":1,"physical_suppression_requested":false,"isolation_guarantee":"diagnostic_route_only_native_os_input_not_suppressed"}
{"record":"input","device_id":"Mouse:HID\\VID_1111&PID_0002\\M1","route":"Routed","seat_id":1,"physical_suppression_requested":false,"isolation_guarantee":"diagnostic_route_only_native_os_input_not_suppressed"}
{"record":"input","device_id":"Keyboard:HID\\VID_2222&PID_0001\\K2","route":"Routed","seat_id":2,"physical_suppression_requested":false,"isolation_guarantee":"diagnostic_route_only_native_os_input_not_suppressed"}
{"record":"input","device_id":"Mouse:HID\\VID_2222&PID_0002\\M2","route":"Routed","seat_id":2,"physical_suppression_requested":false,"isolation_guarantee":"diagnostic_route_only_native_os_input_not_suppressed"}
)json";
    static constexpr std::string_view kGateBSharedTrace =
        R"json({"record":"input","device_id":"Keyboard:HID\\VID_1111&PID_0001\\K1","route":"AmbiguousSharedDevice","physical_suppression_requested":false,"isolation_guarantee":"diagnostic_route_only_native_os_input_not_suppressed"}
)json";
    static constexpr std::string_view kGateCTrace = kGateBTrace;
    static constexpr std::string_view kMetricsJson =
        R"json({"schema_version":1,"unique_input_events":4,"complete_input_events":4,"missing_stage_events":0,"receiver_verified_events":4,"missing_receiver_evidence_events":0,"cross_seat_events":0,"cross_process_events":0,"queue":{"dropped_frames":0},"recorder":{"rotation_drops":0,"contention_drops":0,"invalid_samples":0}})json";
    static constexpr std::string_view kReceiverMissingMetricsJson =
        R"json({"schema_version":1,"unique_input_events":4,"complete_input_events":4,"missing_stage_events":0,"receiver_verified_events":3,"missing_receiver_evidence_events":1,"cross_seat_events":0,"cross_process_events":0,"queue":{"dropped_frames":0},"recorder":{"rotation_drops":0,"contention_drops":0,"invalid_samples":0}})json";

    static std::filesystem::path makeRoot() {
        const auto stamp = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        auto root = std::filesystem::temp_directory_path() /
            ("hydraseat-p3-hw-synthetic-" + std::to_string(stamp));
        std::error_code error;
        std::filesystem::remove_all(root, error);
        error.clear();
        if (!std::filesystem::create_directories(root, error) || error) {
            throw std::runtime_error("could not create synthetic P3-HW test directory");
        }
        return root;
    }

    static std::uint64_t currentUnixSeconds() {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (seconds <= 0) throw std::runtime_error("wall clock unavailable in P3-HW test fixture");
        return static_cast<std::uint64_t>(seconds);
    }

    static void writeText(const std::filesystem::path& path, std::string_view text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) throw std::runtime_error("could not create synthetic P3-HW artifact");
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) throw std::runtime_error("could not write synthetic P3-HW artifact");
    }

    static std::string jsonEscape(std::string_view value) {
        std::string result;
        result.reserve(value.size());
        for (const char ch : value) {
            switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(ch); break;
            }
        }
        return result;
    }

    static std::string gateAManualChecks() {
        return R"json({"two_keyboards_distinct":"PASS","two_pointing_devices_distinct":"PASS","key_down_up_transitions":"PASS","composite_child_removal":"PASS","unplug_replug_identity":"PASS","soak_minimum_duration":"PASS","drop_counter_reviewed":"PASS"})json";
    }

    static std::string gateBManualChecks() {
        return R"json({"seat1_exclusive_routing":"PASS","seat2_exclusive_routing":"PASS","unassigned_fails_closed":"PASS","shared_ambiguous_fails_closed":"PASS","missing_target_explicit_failure":"PASS","trace_seat_target_reviewed":"PASS"})json";
    }

    static std::string gateCManualChecks() {
        return R"json({"two_controlled_targets_visible":"PASS","seat1_changes_only_target1":"PASS","seat2_changes_only_target2":"PASS","unassigned_shared_fail_closed":"PASS","normal_windows_input_not_claimed_suppressed":"PASS","cleanup_no_owned_child_left":"PASS","metrics_reviewed":"PASS"})json";
    }

    std::string manifestJson(SyntheticPhase3EvidenceMode mode,
                             std::uint64_t nowUnixSeconds) const {
        const bool pending = mode == SyntheticPhase3EvidenceMode::Pending;
        const bool gatePending = mode == SyntheticPhase3EvidenceMode::GatePending;
        const bool stale = mode == SyntheticPhase3EvidenceMode::Stale;
        const bool receiverMissing = mode == SyntheticPhase3EvidenceMode::ReceiverMissing;
        const bool scopeMismatch = mode == SyntheticPhase3EvidenceMode::ScopeMismatch;
        const auto manualUnix = stale
            ? nowUnixSeconds - kPhase3HardwareEvidenceValiditySeconds - 1u
            : nowUnixSeconds;
        const auto validUntil = manualUnix + kPhase3HardwareEvidenceValiditySeconds;
        const auto metricsHash = receiverMissing ? kReceiverMissingMetricsSha256 : kMetricsSha256;

        std::ostringstream json;
        json << '{'
             << "\"schema_version\":1,"
             << "\"session_id\":\"synthetic-unit-test-only\","
             << "\"created_utc\":\"2026-08-29T00:00:00Z\","
             << "\"updated_utc\":\"2026-08-29T00:11:00Z\","
             << "\"state\":\"" << (pending ? "READY_FOR_REVIEW" : "MANUAL_PASS") << "\","
             << "\"privacy\":{\"sensitive_key_ids_enabled\":false,\"notice_acknowledged\":true},"
             << "\"environment\":{\"windows_version\":\"test\",\"windows_build\":\"test\",\"architecture\":\"x64\",\"hardware_notes\":\"synthetic unit test; not physical acceptance\"},"
             << "\"profile\":{"
             << "\"source_path\":\"" << jsonEscape(profilePath_.generic_string()) << "\","
             << "\"sha256\":\"" << kProfileSha256 << "\","
             << "\"schema_version\":2,"
             << "\"expected_ownership\":["
             << "{\"device_id\":\"Keyboard:HID\\\\VID_1111&PID_0001\\\\K1\",\"category\":\"keyboard\",\"seat_id\":1},"
             << "{\"device_id\":\"Mouse:HID\\\\VID_1111&PID_0002\\\\M1\",\"category\":\"mouse\",\"seat_id\":1},"
             << "{\"device_id\":\"Keyboard:HID\\\\VID_2222&PID_0001\\\\K2\",\"category\":\"keyboard\",\"seat_id\":2},"
             << "{\"device_id\":\"Mouse:HID\\\\VID_2222&PID_0002\\\\M2\",\"category\":\"mouse\",\"seat_id\":2}],"
             << "\"native_hidhide_scope\":["
             << "{\"device_id\":\"Keyboard:HID\\\\VID_1111&PID_0001\\\\K1\",\"instance_id\":\"HID\\\\VID_1111&PID_0001\\\\K1\",\"category\":\"keyboard\",\"seat_id\":" << (scopeMismatch ? 2 : 1) << "},"
             << "{\"device_id\":\"Mouse:HID\\\\VID_1111&PID_0002\\\\M1\",\"instance_id\":\"HID\\\\VID_1111&PID_0002\\\\M1\",\"category\":\"mouse\",\"seat_id\":1},"
             << "{\"device_id\":\"Keyboard:HID\\\\VID_2222&PID_0001\\\\K2\",\"instance_id\":\"HID\\\\VID_2222&PID_0001\\\\K2\",\"category\":\"keyboard\",\"seat_id\":2},"
             << "{\"device_id\":\"Mouse:HID\\\\VID_2222&PID_0002\\\\M2\",\"instance_id\":\"HID\\\\VID_2222&PID_0002\\\\M2\",\"category\":\"mouse\",\"seat_id\":2}],"
             << "\"shareable_resources\":[],"
             << "\"shared_case\":{\"derived_profile\":\"shared-case-profile.json\",\"sha256\":\"" << kSharedProfileSha256 << "\",\"device_id\":\"Keyboard:HID\\\\VID_1111&PID_0001\\\\K1\",\"category\":\"keyboard\"}},"
             << "\"stages\":{"
             << "\"gate_a\":{\"status\":\"RECORDED\",\"verdict\":\"PASS\",\"started_utc\":\"2026-08-29T00:00:00Z\",\"ended_utc\":\"2026-08-29T00:11:00Z\",\"duration_seconds\":660,\"process_exit_code\":0,\"trace\":\"gate-a.jsonl\",\"trace_sha256\":\"" << kGateATraceSha256 << "\",\"metrics_report\":null,\"metrics_report_sha256\":null,\"auxiliary_traces\":[],\"auxiliary_trace_sha256\":[],\"manual_checks\":" << gateAManualChecks() << ",\"notes\":\"\"},"
             << "\"gate_b\":{\"status\":\"RECORDED\",\"verdict\":\"PASS\",\"started_utc\":\"2026-08-29T00:00:00Z\",\"ended_utc\":\"2026-08-29T00:11:00Z\",\"duration_seconds\":660,\"process_exit_code\":0,\"trace\":\"gate-b.jsonl\",\"trace_sha256\":\"" << kGateBTraceSha256 << "\",\"metrics_report\":null,\"metrics_report_sha256\":null,\"auxiliary_traces\":[\"gate-b-shared.jsonl\"],\"auxiliary_trace_sha256\":[\"" << kGateBSharedTraceSha256 << "\"],\"manual_checks\":" << gateBManualChecks() << ",\"notes\":\"\"},"
             << "\"gate_c\":{\"status\":\"RECORDED\",\"verdict\":\"" << (gatePending ? "PENDING" : "PASS") << "\",\"started_utc\":\"2026-08-29T00:00:00Z\",\"ended_utc\":\"2026-08-29T00:11:00Z\",\"duration_seconds\":660,\"process_exit_code\":0,\"trace\":\"gate-c.jsonl\",\"trace_sha256\":\"" << kGateCTraceSha256 << "\",\"metrics_report\":\"gate-c-metrics.json\",\"metrics_report_sha256\":\"" << metricsHash << "\",\"auxiliary_traces\":[],\"auxiliary_trace_sha256\":[],\"manual_checks\":" << gateCManualChecks() << ",\"notes\":\"\"}},"
             << "\"manual_verdict\":\"" << (pending ? "PENDING" : "PASS") << "\","
             << "\"manual_verdict_note\":\"synthetic unit test only\","
             << "\"manual_verdict_unix\":";
        if (pending) json << "null";
        else json << manualUnix;
        json << ",\"evidence_valid_until_unix\":";
        if (pending) json << "null";
        else json << validUntil;
        json << '}';
        return json.str();
    }

    std::filesystem::path root_;
    std::filesystem::path profilePath_;
    std::filesystem::path sharedProfilePath_;
    std::filesystem::path gateATracePath_;
    std::filesystem::path gateBTracePath_;
    std::filesystem::path gateBSharedTracePath_;
    std::filesystem::path gateCTracePath_;
    std::filesystem::path metricsPath_;
    std::filesystem::path manifestPath_;
    Phase3HardwareEvidenceLoadResult loadResult_;
};

} // namespace hydra::test
