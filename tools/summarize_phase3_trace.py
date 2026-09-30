#!/usr/bin/env python3
"""Summarize P3-HW-01 physical-acceptance evidence without inventing PASS.

The runner records process evidence plus explicit human checks. This parser can
reject malformed/privacy/cross-Seat evidence automatically, but a final PASS is
possible only when the manifest contains an explicit manual PASS and every
required manual check is PASS (or explicitly allowed NOT_APPLICABLE).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import tempfile
import time
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
MAX_TRACE_LINES = 2_000_000
MAX_TRACE_LINE_BYTES = 1_048_576
EVIDENCE_VALIDITY_SECONDS = 24 * 60 * 60
MAX_CLOCK_SKEW_SECONDS = 300
ISOLATION_GUARANTEE = "diagnostic_route_only_native_os_input_not_suppressed"
MANUAL_VALUES = {"PENDING", "PASS", "FAIL", "NOT_APPLICABLE"}

REQUIRED_MANUAL_CHECKS = {
    "gate_a": {
        "two_keyboards_distinct",
        "two_pointing_devices_distinct",
        "key_down_up_transitions",
        "composite_child_removal",
        "unplug_replug_identity",
        "soak_minimum_duration",
        "drop_counter_reviewed",
    },
    "gate_b": {
        "seat1_exclusive_routing",
        "seat2_exclusive_routing",
        "unassigned_fails_closed",
        "shared_ambiguous_fails_closed",
        "missing_target_explicit_failure",
        "trace_seat_target_reviewed",
    },
    "gate_c": {
        "two_controlled_targets_visible",
        "seat1_changes_only_target1",
        "seat2_changes_only_target2",
        "unassigned_shared_fail_closed",
        "normal_windows_input_not_claimed_suppressed",
        "cleanup_no_owned_child_left",
        "metrics_reviewed",
    },
}

OPTIONAL_NOT_APPLICABLE = {
    ("gate_a", "composite_child_removal"),
}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _is_sha256(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdefABCDEF" for character in value
    )


def _exact_keys(value: dict[str, Any], expected: set[str], label: str, errors: list[str]) -> None:
    actual = set(value)
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing:
        errors.append(f"{label} is missing required keys: {', '.join(missing)}")
    if extra:
        errors.append(f"{label} contains unexpected keys: {', '.join(extra)}")


def validate_manifest(manifest: Any) -> list[str]:
    errors: list[str] = []
    if not isinstance(manifest, dict):
        return ["manifest root must be an object"]

    _exact_keys(
        manifest,
        {
            "schema_version", "session_id", "created_utc", "updated_utc", "state",
            "privacy", "environment", "profile", "stages", "manual_verdict",
            "manual_verdict_note", "manual_verdict_unix", "evidence_valid_until_unix",
        },
        "manifest",
        errors,
    )
    if manifest.get("schema_version") != SCHEMA_VERSION:
        errors.append("manifest schema_version must be 1")
    if not isinstance(manifest.get("session_id"), str) or not manifest.get("session_id"):
        errors.append("manifest session_id is required")
    if manifest.get("state") not in {"IN_PROGRESS", "READY_FOR_REVIEW", "MANUAL_PASS", "MANUAL_FAIL"}:
        errors.append("manifest state is invalid")
    if manifest.get("manual_verdict") not in {"PENDING", "PASS", "FAIL"}:
        errors.append("manual_verdict must be PENDING, PASS, or FAIL")
    if manifest.get("manual_verdict_unix") is not None and not isinstance(manifest.get("manual_verdict_unix"), int):
        errors.append("manual_verdict_unix must be an integer or null")
    if manifest.get("evidence_valid_until_unix") is not None and not isinstance(manifest.get("evidence_valid_until_unix"), int):
        errors.append("evidence_valid_until_unix must be an integer or null")

    privacy = manifest.get("privacy")
    if not isinstance(privacy, dict):
        errors.append("manifest privacy object is required")
    else:
        _exact_keys(privacy, {"sensitive_key_ids_enabled", "notice_acknowledged"}, "privacy", errors)
        if not isinstance(privacy.get("sensitive_key_ids_enabled"), bool):
            errors.append("manifest privacy.sensitive_key_ids_enabled must be boolean")
        if not isinstance(privacy.get("notice_acknowledged"), bool):
            errors.append("manifest privacy.notice_acknowledged must be boolean")

    profile = manifest.get("profile")
    if not isinstance(profile, dict):
        errors.append("manifest profile object is required")
    else:
        _exact_keys(
            profile,
            {
                "source_path", "sha256", "schema_version", "expected_ownership",
                "native_hidhide_scope", "shareable_resources", "shared_case",
            },
            "profile",
            errors,
        )
        if profile.get("schema_version") != 2:
            errors.append("profile.schema_version must be 2")
        if not _is_sha256(profile.get("sha256")):
            errors.append("profile.sha256 must be a 64-character SHA-256 hex digest")
        ownership_rows = profile.get("expected_ownership")
        if not isinstance(ownership_rows, list):
            errors.append("profile.expected_ownership must be an array")
        else:
            if len(ownership_rows) < 4 or len(ownership_rows) > 64:
                errors.append("profile.expected_ownership must contain between four and 64 entries")
            for index, item in enumerate(ownership_rows):
                if not isinstance(item, dict):
                    errors.append(f"profile.expected_ownership[{index}] must be an object")
                    continue
                _exact_keys(item, {"device_id", "category", "seat_id"}, f"profile.expected_ownership[{index}]", errors)
                if not isinstance(item.get("device_id"), str) or not item.get("device_id"):
                    errors.append(f"profile.expected_ownership[{index}].device_id is required")
                if item.get("category") not in {"keyboard", "mouse"}:
                    errors.append(f"profile.expected_ownership[{index}].category is invalid")
                if not isinstance(item.get("seat_id"), int) or item["seat_id"] <= 0:
                    errors.append(f"profile.expected_ownership[{index}].seat_id is invalid")

        native_rows = profile.get("native_hidhide_scope")
        if not isinstance(native_rows, list):
            errors.append("profile.native_hidhide_scope must be an array")
        else:
            if len(native_rows) < 4 or len(native_rows) > 16:
                errors.append("profile.native_hidhide_scope must contain between four and 16 entries")
            for index, item in enumerate(native_rows):
                if not isinstance(item, dict):
                    errors.append(f"profile.native_hidhide_scope[{index}] must be an object")
                    continue
                _exact_keys(item, {"device_id", "instance_id", "category", "seat_id"}, f"profile.native_hidhide_scope[{index}]", errors)
                if not isinstance(item.get("device_id"), str) or not item.get("device_id"):
                    errors.append(f"profile.native_hidhide_scope[{index}].device_id is required")
                if not isinstance(item.get("instance_id"), str) or not item.get("instance_id"):
                    errors.append(f"profile.native_hidhide_scope[{index}].instance_id is required")
                if item.get("category") not in {"keyboard", "mouse"}:
                    errors.append(f"profile.native_hidhide_scope[{index}].category is invalid")
                if not isinstance(item.get("seat_id"), int) or item["seat_id"] <= 0:
                    errors.append(f"profile.native_hidhide_scope[{index}].seat_id is invalid")
        shared_case = profile.get("shared_case")
        if not isinstance(shared_case, dict):
            errors.append("profile.shared_case object is required")
        else:
            _exact_keys(shared_case, {"derived_profile", "sha256", "device_id", "category"}, "profile.shared_case", errors)
            if shared_case.get("category") not in {"keyboard", "mouse"}:
                errors.append("profile.shared_case.category must be keyboard or mouse")
            if not isinstance(shared_case.get("device_id"), str) or not shared_case.get("device_id"):
                errors.append("profile.shared_case.device_id is required")
            if not _is_sha256(shared_case.get("sha256")):
                errors.append("profile.shared_case.sha256 must be a SHA-256 digest")

    stages = manifest.get("stages")
    if not isinstance(stages, dict):
        errors.append("manifest stages object is required")
    else:
        _exact_keys(stages, set(REQUIRED_MANUAL_CHECKS), "stages", errors)
        stage_keys = {
            "status", "verdict", "started_utc", "ended_utc", "duration_seconds",
            "process_exit_code", "trace", "trace_sha256", "metrics_report",
            "metrics_report_sha256", "auxiliary_traces", "auxiliary_trace_sha256",
            "manual_checks", "notes",
        }
        for stage_name, required_checks in REQUIRED_MANUAL_CHECKS.items():
            stage = stages.get(stage_name)
            if not isinstance(stage, dict):
                errors.append(f"missing stage object: {stage_name}")
                continue
            _exact_keys(stage, stage_keys, stage_name, errors)
            if stage.get("verdict") not in {"PENDING", "PASS", "FAIL"}:
                errors.append(f"{stage_name}.verdict is invalid")
            checks = stage.get("manual_checks")
            if not isinstance(checks, dict):
                errors.append(f"{stage_name}.manual_checks must be an object")
            else:
                _exact_keys(checks, required_checks, f"{stage_name}.manual_checks", errors)
                for key, value in checks.items():
                    if value not in MANUAL_VALUES:
                        errors.append(f"invalid manual check value {stage_name}.{key}: {value!r}")
            if stage.get("trace") is not None and not _is_sha256(stage.get("trace_sha256")):
                errors.append(f"{stage_name}.trace_sha256 is required when trace is present")
            if stage.get("metrics_report") is not None and not _is_sha256(stage.get("metrics_report_sha256")):
                errors.append(f"{stage_name}.metrics_report_sha256 is required when metrics_report is present")
            auxiliary = stage.get("auxiliary_traces")
            auxiliary_hashes = stage.get("auxiliary_trace_sha256")
            if not isinstance(auxiliary, list) or not isinstance(auxiliary_hashes, list):
                errors.append(f"{stage_name} auxiliary trace/hash fields must be arrays")
            elif len(auxiliary) != len(auxiliary_hashes):
                errors.append(f"{stage_name} auxiliary trace/hash counts do not match")
            elif any(not _is_sha256(value) for value in auxiliary_hashes):
                errors.append(f"{stage_name} contains an invalid auxiliary trace hash")
    return errors


def expected_ownership(manifest: dict[str, Any]) -> tuple[dict[str, int], list[str]]:
    result: dict[str, int] = {}
    errors: list[str] = []
    for item in manifest.get("profile", {}).get("expected_ownership", []):
        if not isinstance(item, dict):
            errors.append("expected_ownership entry is not an object")
            continue
        device_id = item.get("device_id")
        seat_id = item.get("seat_id")
        category = item.get("category")
        if not isinstance(device_id, str) or not device_id:
            errors.append("expected ownership has empty device_id")
            continue
        if category not in {"keyboard", "mouse"}:
            errors.append(f"expected ownership for {device_id!r} has invalid category")
            continue
        if not isinstance(seat_id, int) or seat_id <= 0:
            errors.append(f"expected ownership for {device_id!r} has invalid seat_id")
            continue
        previous = result.get(device_id)
        if previous is not None and previous != seat_id:
            errors.append(
                f"device {device_id!r} has conflicting expected Seats {previous} and {seat_id}"
            )
            continue
        result[device_id] = seat_id
    return result, errors


def _ownership_tuples(value: Any) -> list[tuple[str, str, int]]:
    if not isinstance(value, list):
        return []
    rows: list[tuple[str, str, int]] = []
    for item in value:
        if not isinstance(item, dict):
            continue
        device_id = item.get("device_id")
        category = item.get("category")
        seat_id = item.get("seat_id")
        if isinstance(device_id, str) and category in {"keyboard", "mouse"} and isinstance(seat_id, int):
            rows.append((device_id, category, seat_id))
    return sorted(rows)


def _native_scope_tuples(value: Any) -> list[tuple[str, str, str, int]]:
    if not isinstance(value, list):
        return []
    rows: list[tuple[str, str, str, int]] = []
    for item in value:
        if not isinstance(item, dict):
            continue
        device_id = item.get("device_id")
        instance_id = item.get("instance_id")
        category = item.get("category")
        seat_id = item.get("seat_id")
        if (
            isinstance(device_id, str)
            and isinstance(instance_id, str)
            and category in {"keyboard", "mouse"}
            and isinstance(seat_id, int)
        ):
            rows.append((device_id, instance_id, category, seat_id))
    return sorted(rows)


def derive_profile_ownership(profile: Any) -> tuple[list[tuple[str, str, int]], list[tuple[str, str, str, int]], list[str]]:
    errors: list[str] = []
    if not isinstance(profile, dict) or profile.get("schema_version") != 2:
        return [], [], ["current profile must be a schema_version 2 object"]
    seats = profile.get("seats")
    if not isinstance(seats, list):
        return [], [], ["current profile seats must be an array"]
    active = [seat for seat in seats if isinstance(seat, dict) and seat.get("active") is True and isinstance(seat.get("id"), int)]
    active.sort(key=lambda seat: seat["id"])
    if len(active) < 2:
        return [], [], ["current profile has fewer than two active Seats"]

    shareable: set[tuple[str, str]] = set()
    for item in profile.get("shareable_resources", []):
        if isinstance(item, dict) and isinstance(item.get("type"), str) and isinstance(item.get("id"), str):
            shareable.add((item["type"], item["id"]))

    ownership: list[tuple[str, str, int]] = []
    seen: dict[tuple[str, str], int] = {}
    for seat in active:
        seat_id = seat["id"]
        for category, property_name in (("keyboard", "keyboards"), ("mouse", "mice")):
            values = seat.get(property_name, [])
            if not isinstance(values, list):
                errors.append(f"Seat {seat_id} {property_name} must be an array")
                continue
            for device_id in values:
                if not isinstance(device_id, str) or not device_id:
                    continue
                key = (category, device_id)
                if key in shareable:
                    continue
                previous = seen.get(key)
                if previous is not None and previous != seat_id:
                    errors.append(f"non-shareable {category} {device_id!r} is assigned to Seats {previous} and {seat_id}")
                    continue
                seen[key] = seat_id
                ownership.append((device_id, category, seat_id))
    ownership.sort()
    first_two_ids = {active[0]["id"], active[1]["id"]}
    native_scope: list[tuple[str, str, str, int]] = []
    for device_id, category, seat_id in ownership:
        if seat_id not in first_two_ids:
            continue
        prefix = "Keyboard:" if category == "keyboard" else "Mouse:"
        if not device_id.lower().startswith(prefix.lower()):
            errors.append(
                f"stable {category} identity {device_id!r} does not contain the expected {prefix} prefix"
            )
            continue
        instance_id = device_id[len(prefix):]
        if not instance_id:
            errors.append(f"stable identity {device_id!r} produced an empty HidHide instance ID")
            continue
        native_scope.append((device_id, instance_id, category, seat_id))
    native_scope.sort()
    for seat_id in first_two_ids:
        categories = {category for _, _, category, owner in native_scope if owner == seat_id}
        if categories != {"keyboard", "mouse"}:
            errors.append(f"Seat {seat_id} does not have both exclusive keyboard and mouse evidence")
    if len(native_scope) < 4 or len(native_scope) > 16:
        errors.append("current profile native HidHide scope must contain between four and sixteen devices")
    return ownership, native_scope, errors


def resolve_profile_path(manifest_path: Path, value: Any) -> Path | None:
    if not isinstance(value, str) or not value:
        return None
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = manifest_path.parent / candidate
    return candidate


def resolve_artifact(manifest_path: Path, value: Any) -> Path | None:
    if value is None or not isinstance(value, str) or not value:
        return None
    candidate = Path(value)
    if candidate.is_absolute() or any(part == ".." for part in candidate.parts):
        return None
    return manifest_path.parent / candidate


def verify_artifact_hash(path: Path | None, expected: Any, label: str, errors: list[str]) -> None:
    if path is None or not path.is_file():
        errors.append(f"{label} is missing")
        return
    if not _is_sha256(expected):
        errors.append(f"{label} SHA-256 is missing or malformed")
        return
    try:
        observed = sha256_file(path)
    except OSError as exc:
        errors.append(f"{label} could not be hashed: {exc}")
        return
    if observed.lower() != expected.lower():
        errors.append(f"{label} SHA-256 mismatch; evidence changed after recording")


def parse_trace(
    path: Path,
    *,
    sensitive_key_ids_enabled: bool,
    expected_seats: dict[str, int] | None,
) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "path": str(path),
        "present": path.is_file(),
        "lines": 0,
        "input_events": 0,
        "device_changes": 0,
        "arrivals": 0,
        "removals": 0,
        "unique_devices": 0,
        "redacted_key_records": 0,
        "sensitive_key_records": 0,
        "route_counts": {},
        "device_route_counts": {},
        "routed_seat_counts": {},
        "errors": [],
        "warnings": [],
    }
    if not path.is_file():
        summary["errors"].append("trace file is missing")
        return summary

    devices: set[str] = set()
    routes: Counter[str] = Counter()
    device_routes: dict[str, Counter[str]] = {}
    routed_seats: Counter[int] = Counter()
    with path.open("rb") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            if line_number > MAX_TRACE_LINES:
                summary["errors"].append(
                    f"trace exceeds bounded line count {MAX_TRACE_LINES}"
                )
                break
            if len(raw_line) > MAX_TRACE_LINE_BYTES:
                summary["errors"].append(
                    f"line {line_number} exceeds {MAX_TRACE_LINE_BYTES} bytes"
                )
                break
            if not raw_line.strip():
                continue
            summary["lines"] += 1
            try:
                record = json.loads(raw_line.decode("utf-8-sig"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                summary["errors"].append(f"line {line_number} is malformed JSON/UTF-8: {exc}")
                continue
            if not isinstance(record, dict):
                summary["errors"].append(f"line {line_number} is not a JSON object")
                continue
            device_id = record.get("device_id")
            if isinstance(device_id, str) and device_id:
                devices.add(device_id)
            record_type = record.get("record")
            if record_type == "device_change":
                summary["device_changes"] += 1
                change = record.get("change")
                if change == "Arrival":
                    summary["arrivals"] += 1
                elif change == "Removal":
                    summary["removals"] += 1
                else:
                    summary["warnings"].append(
                        f"line {line_number} has unknown device change {change!r}"
                    )
                continue
            if record_type != "input":
                summary["warnings"].append(
                    f"line {line_number} has unknown record type {record_type!r}"
                )
                continue

            summary["input_events"] += 1
            route = record.get("route")
            if isinstance(route, str):
                routes[route] += 1
                if isinstance(device_id, str) and device_id:
                    device_routes.setdefault(device_id, Counter())[route] += 1
                if route == "Routed" and isinstance(record.get("seat_id"), int):
                    routed_seats[record["seat_id"]] += 1
            if record.get("isolation_guarantee") != ISOLATION_GUARANTEE:
                summary["errors"].append(
                    f"line {line_number} has missing/incorrect isolation guarantee"
                )
            if record.get("physical_suppression_requested") is True:
                summary["errors"].append(
                    f"line {line_number} unexpectedly requests physical suppression"
                )

            redacted = record.get("key_code_redacted")
            vkey = record.get("vkey")
            if redacted is True and vkey is None:
                summary["redacted_key_records"] += 1
            elif redacted is False and isinstance(vkey, int):
                summary["sensitive_key_records"] += 1
                if not sensitive_key_ids_enabled:
                    summary["errors"].append(
                        f"line {line_number} contains a key identifier without manifest opt-in"
                    )
            elif vkey is not None:
                summary["errors"].append(
                    f"line {line_number} has non-canonical key privacy metadata"
                )

            if expected_seats is not None and route == "Routed":
                if not isinstance(device_id, str) or device_id not in expected_seats:
                    summary["errors"].append(
                        f"line {line_number} routes unexpected/unassigned device {device_id!r}"
                    )
                else:
                    seat_id = record.get("seat_id")
                    if seat_id != expected_seats[device_id]:
                        summary["errors"].append(
                            f"line {line_number} routes {device_id!r} to Seat {seat_id!r}; "
                            f"expected Seat {expected_seats[device_id]}"
                        )

    summary["unique_devices"] = len(devices)
    summary["route_counts"] = dict(sorted(routes.items()))
    summary["device_route_counts"] = {
        device_id: dict(sorted(counts.items()))
        for device_id, counts in sorted(device_routes.items())
    }
    summary["routed_seat_counts"] = {
        str(seat_id): count for seat_id, count in sorted(routed_seats.items())
    }
    if summary["lines"] == 0:
        summary["errors"].append("trace contains no evidence records")
    return summary


def parse_metrics(path: Path | None) -> dict[str, Any]:
    result: dict[str, Any] = {
        "path": str(path) if path else None,
        "present": bool(path and path.is_file()),
        "errors": [],
        "warnings": [],
    }
    if path is None or not path.is_file():
        result["errors"].append("Gate C metrics report is missing")
        return result
    try:
        report = load_json(path)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        result["errors"].append(f"metrics report is malformed: {exc}")
        return result
    if not isinstance(report, dict) or report.get("schema_version") != 1:
        result["errors"].append("metrics report schema_version must be 1")
        return result

    required_ints = [
        "unique_input_events",
        "complete_input_events",
        "missing_stage_events",
        "receiver_verified_events",
        "missing_receiver_evidence_events",
        "cross_seat_events",
        "cross_process_events",
    ]
    for key in required_ints:
        if not isinstance(report.get(key), int) or report[key] < 0:
            result["errors"].append(f"metrics report {key} must be a non-negative integer")
    queue = report.get("queue")
    recorder = report.get("recorder")
    if not isinstance(queue, dict):
        result["errors"].append("metrics report queue object is missing")
        queue = {}
    if not isinstance(recorder, dict):
        result["errors"].append("metrics report recorder object is missing")
        recorder = {}

    result.update(
        {
            "unique_input_events": report.get("unique_input_events"),
            "complete_input_events": report.get("complete_input_events"),
            "missing_stage_events": report.get("missing_stage_events"),
            "receiver_verified_events": report.get("receiver_verified_events"),
            "missing_receiver_evidence_events": report.get(
                "missing_receiver_evidence_events"
            ),
            "cross_seat_events": report.get("cross_seat_events"),
            "cross_process_events": report.get("cross_process_events"),
            "queue_high_water": queue.get("high_water"),
            "queue_dropped_frames": queue.get("dropped_frames"),
            "recorder_rotation_drops": recorder.get("rotation_drops"),
            "recorder_contention_drops": recorder.get("contention_drops"),
            "recorder_invalid_samples": recorder.get("invalid_samples"),
            "latency": report.get("latency"),
            "resource": report.get("resource"),
        }
    )

    unique = report.get("unique_input_events")
    complete = report.get("complete_input_events")
    missing_stage = report.get("missing_stage_events")
    receiver_verified = report.get("receiver_verified_events")
    missing_receiver = report.get("missing_receiver_evidence_events")
    if unique == 0:
        result["errors"].append("metrics report contains zero input events")
    if isinstance(unique, int) and isinstance(complete, int) and complete != unique:
        result["errors"].append("not every Gate C input event has complete stage evidence")
    if isinstance(missing_stage, int) and missing_stage != 0:
        result["errors"].append("Gate C metrics contain missing stage evidence")
    if isinstance(unique, int) and isinstance(receiver_verified, int) and receiver_verified != unique:
        result["errors"].append("not every Gate C input event has verified receiver identity")
    if isinstance(missing_receiver, int) and missing_receiver != 0:
        result["errors"].append(
            "Gate C metrics contain missing receiver evidence; zero-bleed is unproven"
        )
    if isinstance(report.get("cross_seat_events"), int) and report["cross_seat_events"] > 0:
        result["errors"].append("verified cross-Seat events are nonzero")
    if isinstance(report.get("cross_process_events"), int) and report["cross_process_events"] > 0:
        result["errors"].append("verified cross-process events are nonzero")
    if not isinstance(queue.get("dropped_frames"), int) or queue["dropped_frames"] != 0:
        result["errors"].append("Gate C writer queue dropped_frames must be zero")
    for key in ("rotation_drops", "contention_drops", "invalid_samples"):
        value = recorder.get(key)
        if not isinstance(value, int) or value != 0:
            result["errors"].append(f"metrics recorder {key} must be zero")
    return result


def manual_status(manifest: dict[str, Any]) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    pending: list[str] = []
    stages = manifest.get("stages", {})
    for stage_name, required in REQUIRED_MANUAL_CHECKS.items():
        checks = stages.get(stage_name, {}).get("manual_checks", {})
        for check_name in sorted(required):
            value = checks.get(check_name, "PENDING")
            if value == "FAIL":
                errors.append(f"manual check failed: {stage_name}.{check_name}")
            elif value == "PASS":
                continue
            elif value == "NOT_APPLICABLE" and (stage_name, check_name) in OPTIONAL_NOT_APPLICABLE:
                continue
            else:
                pending.append(f"{stage_name}.{check_name}")
    return errors, pending


def summarize(manifest_path: Path) -> dict[str, Any]:
    try:
        manifest = load_json(manifest_path)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return {
            "schema_version": SCHEMA_VERSION,
            "generated_utc": utc_now(),
            "final_verdict": "FAIL",
            "errors": [f"manifest could not be read: {exc}"],
            "warnings": [],
            "stages": {},
        }

    errors = validate_manifest(manifest)
    warnings: list[str] = []
    owner_map, ownership_errors = expected_ownership(manifest)
    errors.extend(ownership_errors)
    privacy = manifest.get("privacy", {})
    sensitive = bool(privacy.get("sensitive_key_ids_enabled", False))
    profile_manifest = manifest.get("profile", {}) if isinstance(manifest.get("profile"), dict) else {}

    profile_path = resolve_profile_path(manifest_path, profile_manifest.get("source_path"))
    current_ownership: list[tuple[str, str, int]] = []
    current_native_scope: list[tuple[str, str, str, int]] = []
    verify_artifact_hash(profile_path, profile_manifest.get("sha256"), "current profile", errors)
    if profile_path is not None and profile_path.is_file():
        try:
            current_profile = load_json(profile_path)
            current_ownership, current_native_scope, current_profile_errors = derive_profile_ownership(current_profile)
            errors.extend(current_profile_errors)
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            errors.append(f"current profile could not be parsed: {exc}")

    manifest_ownership = _ownership_tuples(profile_manifest.get("expected_ownership"))
    manifest_native_scope = _native_scope_tuples(profile_manifest.get("native_hidhide_scope"))
    if current_ownership and manifest_ownership != current_ownership:
        errors.append("current profile Seat/device ownership does not match recorded P3-HW ownership")
    if current_native_scope and manifest_native_scope != current_native_scope:
        errors.append("current profile native HidHide scope does not match recorded P3-HW scope")

    shared_case = profile_manifest.get("shared_case", {}) if isinstance(profile_manifest.get("shared_case"), dict) else {}
    shared_profile_path = resolve_artifact(manifest_path, shared_case.get("derived_profile"))
    verify_artifact_hash(shared_profile_path, shared_case.get("sha256"), "shared-case profile", errors)

    native_stable_ids = {row[0] for row in manifest_native_scope}
    stages_out: dict[str, Any] = {}

    for stage_name in ("gate_a", "gate_b", "gate_c"):
        stage = manifest.get("stages", {}).get(stage_name, {})
        stage_errors: list[str] = []
        stage_warnings: list[str] = []
        status = stage.get("status")
        exit_code = stage.get("process_exit_code")
        if status == "FAILED":
            stage_errors.append("runner recorded stage failure")
        if status not in {"RECORDED", "FAILED"}:
            stage_warnings.append(f"stage status is {status!r}, not RECORDED")
        if status == "RECORDED" and exit_code != 0:
            stage_errors.append(f"RECORDED stage requires process exit code 0, got {exit_code!r}")
        elif exit_code is not None and exit_code != 0:
            stage_errors.append(f"process exit code is {exit_code}")

        duration = stage.get("duration_seconds", 0)
        if stage_name == "gate_a" and status == "RECORDED":
            if not isinstance(duration, (int, float)) or duration < 600:
                stage_errors.append("Gate A duration is below the required 10-minute soak")

        trace_path = resolve_artifact(manifest_path, stage.get("trace"))
        trace_summary = None
        if stage.get("trace") is not None:
            verify_artifact_hash(
                trace_path,
                stage.get("trace_sha256"),
                f"{stage_name} trace",
                stage_errors,
            )
        if trace_path is not None:
            trace_summary = parse_trace(
                trace_path,
                sensitive_key_ids_enabled=sensitive,
                expected_seats=None if stage_name == "gate_a" else owner_map,
            )
            stage_errors.extend(trace_summary["errors"])
            stage_warnings.extend(trace_summary["warnings"])
            if stage_name == "gate_a":
                observed_devices = set(trace_summary.get("device_route_counts", {}))
                if not native_stable_ids.issubset(observed_devices):
                    missing = sorted(native_stable_ids - observed_devices)
                    stage_errors.append(
                        "Gate A trace is missing native-scope physical identities: " + ", ".join(missing)
                    )
                if trace_summary["removals"] <= 0 or trace_summary["arrivals"] <= 0:
                    stage_errors.append(
                        "Gate A trace must contain both removal and arrival hot-plug evidence"
                    )
            elif stage_name in {"gate_b", "gate_c"}:
                for stable_id in sorted(native_stable_ids):
                    routes = trace_summary.get("device_route_counts", {}).get(stable_id, {})
                    if routes.get("Routed", 0) <= 0:
                        stage_errors.append(
                            f"{stage_name} trace has no Routed evidence for native-scope device {stable_id!r}"
                        )
                expected_seat_ids = sorted({row[3] for row in manifest_native_scope})
                if len(expected_seat_ids) != 2:
                    stage_errors.append(f"{stage_name} requires exactly two native-scope Seat groups")
                for seat_id in expected_seat_ids:
                    if trace_summary["routed_seat_counts"].get(str(seat_id), 0) <= 0:
                        stage_errors.append(
                            f"{stage_name} trace has no routed evidence for expected Seat {seat_id}"
                        )
        elif status == "RECORDED":
            stage_errors.append("trace path is missing or unsafe")

        auxiliary_summaries: list[dict[str, Any]] = []
        if stage_name == "gate_b":
            auxiliary_values = stage.get("auxiliary_traces", [])
            auxiliary_hashes = stage.get("auxiliary_trace_sha256", [])
            if status == "RECORDED" and (
                not isinstance(auxiliary_values, list)
                or not isinstance(auxiliary_hashes, list)
                or len(auxiliary_values) != 1
                or len(auxiliary_hashes) != 1
            ):
                stage_errors.append("Gate B requires exactly one shared-case auxiliary trace and hash")
            elif isinstance(auxiliary_values, list) and isinstance(auxiliary_hashes, list):
                for index, auxiliary_value in enumerate(auxiliary_values):
                    auxiliary_path = resolve_artifact(manifest_path, auxiliary_value)
                    expected_hash = auxiliary_hashes[index] if index < len(auxiliary_hashes) else None
                    verify_artifact_hash(
                        auxiliary_path,
                        expected_hash,
                        f"Gate B auxiliary trace {index}",
                        stage_errors,
                    )
                    if auxiliary_path is None:
                        continue
                    auxiliary = parse_trace(
                        auxiliary_path,
                        sensitive_key_ids_enabled=sensitive,
                        expected_seats=None,
                    )
                    auxiliary_summaries.append(auxiliary)
                    stage_errors.extend(auxiliary["errors"])
                    stage_warnings.extend(auxiliary["warnings"])
                shared_device = shared_case.get("device_id")
                if auxiliary_summaries and isinstance(shared_device, str):
                    routes_for_shared = auxiliary_summaries[0].get("device_route_counts", {}).get(shared_device, {})
                    if routes_for_shared.get("AmbiguousSharedDevice", 0) <= 0:
                        stage_errors.append(
                            "shared-case device has no AmbiguousSharedDevice evidence"
                        )
                    if routes_for_shared.get("Routed", 0) > 0:
                        stage_errors.append(
                            "shared-case device was routed despite ambiguous ownership"
                        )

        metrics_summary = None
        if stage_name == "gate_c":
            metrics_path = resolve_artifact(manifest_path, stage.get("metrics_report"))
            if stage.get("metrics_report") is not None:
                verify_artifact_hash(
                    metrics_path,
                    stage.get("metrics_report_sha256"),
                    "Gate C metrics report",
                    stage_errors,
                )
            metrics_summary = parse_metrics(metrics_path)
            stage_errors.extend(metrics_summary["errors"])
            stage_warnings.extend(metrics_summary["warnings"])

        checks = stage.get("manual_checks", {}) if isinstance(stage, dict) else {}
        stage_pending = False
        for check_name in sorted(REQUIRED_MANUAL_CHECKS[stage_name]):
            value = checks.get(check_name, "PENDING") if isinstance(checks, dict) else "PENDING"
            if value == "FAIL":
                stage_errors.append(f"manual check failed: {check_name}")
            elif value == "PASS":
                continue
            elif value == "NOT_APPLICABLE" and (stage_name, check_name) in OPTIONAL_NOT_APPLICABLE:
                continue
            else:
                stage_pending = True

        if stage_errors:
            computed_verdict = "FAIL"
        elif status == "RECORDED" and exit_code == 0 and not stage_pending:
            computed_verdict = "PASS"
        else:
            computed_verdict = "PENDING"

        recorded_verdict = stage.get("verdict")
        if recorded_verdict in {"PASS", "FAIL"} and recorded_verdict != computed_verdict:
            stage_errors.append(
                f"stored verdict {recorded_verdict} disagrees with computed verdict {computed_verdict}"
            )
            computed_verdict = "FAIL"

        stages_out[stage_name] = {
            "status": status,
            "recorded_verdict": recorded_verdict,
            "verdict": computed_verdict,
            "process_exit_code": exit_code,
            "duration_seconds": duration,
            "trace": trace_summary,
            "auxiliary_traces": auxiliary_summaries,
            "metrics": metrics_summary,
            "errors": stage_errors,
            "warnings": stage_warnings,
        }
        errors.extend(f"{stage_name}: {item}" for item in stage_errors)
        warnings.extend(f"{stage_name}: {item}" for item in stage_warnings)

    manual_errors, pending_checks = manual_status(manifest)
    errors.extend(manual_errors)
    manual_verdict = manifest.get("manual_verdict", "PENDING")
    manifest_state = manifest.get("state")
    verdict_unix = manifest.get("manual_verdict_unix")
    valid_until = manifest.get("evidence_valid_until_unix")
    now_unix = int(time.time())

    if manual_verdict == "PASS":
        if manifest_state != "MANUAL_PASS":
            errors.append("manual_verdict=PASS requires state=MANUAL_PASS")
        if not isinstance(verdict_unix, int) or not isinstance(valid_until, int):
            errors.append("manual PASS requires verdict and evidence-validity timestamps")
        else:
            if valid_until != verdict_unix + EVIDENCE_VALIDITY_SECONDS:
                errors.append("evidence validity deadline does not match the fixed 24-hour P3-HW window")
            if verdict_unix > now_unix + MAX_CLOCK_SKEW_SECONDS:
                errors.append("manual PASS timestamp is implausibly in the future")
            if valid_until <= now_unix:
                errors.append("P3-HW physical evidence is stale")
    elif manifest_state == "MANUAL_PASS":
        errors.append("state=MANUAL_PASS without manual_verdict=PASS is inconsistent")

    stage_verdicts = [stages_out.get(name, {}).get("verdict") for name in ("gate_a", "gate_b", "gate_c")]
    if errors or manual_verdict == "FAIL" or "FAIL" in stage_verdicts:
        activation_verdict = "FAIL"
    elif manual_verdict == "PASS" and not pending_checks and stage_verdicts == ["PASS", "PASS", "PASS"]:
        activation_verdict = "PASS"
    else:
        activation_verdict = "PENDING"

    # P3-HW Gate A/B/C evidence can authorize a guarded native HidHide trial, but
    # these diagnostic traces explicitly do not claim native Windows device
    # suppression. Until a separate declared physical-suppression observation is
    # recorded, release-level physical isolation must remain PENDING. This keeps
    # synthetic/diagnostic PASS from becoming a release claim by implication.
    physical_isolation_release_verdict = "FAIL" if activation_verdict == "FAIL" else "PENDING"
    final_verdict = physical_isolation_release_verdict

    if activation_verdict == "PASS":
        warnings.append(
            "Gate A/B/C evidence is sufficient only for guarded native HidHide activation; "
            "physical native device suppression has not been recorded, so release-level physical isolation remains PENDING"
        )
    elif final_verdict == "PENDING" and not errors:
        warnings.append(
            "automatic evidence is not a physical PASS; all Gate verdicts plus explicit manual_verdict=PASS are required before guarded activation"
        )

    return {
        "schema_version": SCHEMA_VERSION,
        "session_id": manifest.get("session_id"),
        "generated_utc": utc_now(),
        "manifest_state": manifest_state,
        "manual_verdict": manual_verdict,
        "manual_verdict_unix": verdict_unix,
        "evidence_valid_until_unix": valid_until,
        "native_hidhide_activation_verdict": activation_verdict,
        "physical_isolation_release_verdict": physical_isolation_release_verdict,
        "physical_device_suppression_evidence_present": False,
        "final_verdict": final_verdict,
        "privacy": {
            "sensitive_key_ids_enabled": sensitive,
        },
        "profile": {
            "source_path": str(profile_path) if profile_path else None,
            "sha256": profile_manifest.get("sha256"),
            "native_hidhide_scope": profile_manifest.get("native_hidhide_scope", []),
        },
        "pending_manual_checks": pending_checks,
        "errors": errors,
        "warnings": warnings,
        "stages": stages_out,
    }


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")
    temporary.replace(path)


def _stage(check_names: set[str], trace: Path, metrics: Path | None = None) -> dict[str, Any]:
    return {
        "status": "RECORDED",
        "verdict": "PENDING",
        "started_utc": "2026-08-29T00:00:00Z",
        "ended_utc": "2026-08-29T00:11:00Z",
        "duration_seconds": 660,
        "process_exit_code": 0,
        "trace": trace.name,
        "trace_sha256": sha256_file(trace),
        "metrics_report": metrics.name if metrics else None,
        "metrics_report_sha256": sha256_file(metrics) if metrics else None,
        "auxiliary_traces": [],
        "auxiliary_trace_sha256": [],
        "manual_checks": {name: "PENDING" for name in sorted(check_names)},
        "notes": "",
    }


def run_self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="hydraseat-p3-hw-") as directory:
        root = Path(directory)
        profile_path = root / "workspace_config.json"
        shared_profile = root / "shared-case-profile.json"
        gate_a = root / "gate-a.jsonl"
        gate_b = root / "gate-b.jsonl"
        gate_b_shared = root / "gate-b-shared.jsonl"
        gate_c = root / "gate-c.jsonl"
        metrics = root / "gate-c-metrics.json"

        identities = [
            ("Keyboard:USB\\VID_1111&PID_0001\\A", "USB\\VID_1111&PID_0001\\A", "keyboard", 1, 1),
            ("Mouse:USB\\VID_1111&PID_0002\\A", "USB\\VID_1111&PID_0002\\A", "mouse", 1, 0),
            ("Keyboard:USB\\VID_2222&PID_0001\\B", "USB\\VID_2222&PID_0001\\B", "keyboard", 2, 1),
            ("Mouse:USB\\VID_2222&PID_0002\\B", "USB\\VID_2222&PID_0002\\B", "mouse", 2, 0),
        ]
        profile = {
            "schema_version": 2,
            "shareable_resources": [],
            "seats": [
                {
                    "id": 1,
                    "active": True,
                    "keyboards": [identities[0][0]],
                    "mice": [identities[1][0]],
                },
                {
                    "id": 2,
                    "active": True,
                    "keyboards": [identities[2][0]],
                    "mice": [identities[3][0]],
                },
            ],
        }
        write_json(profile_path, profile)
        write_json(shared_profile, {"fixture": "shared-case"})

        def input_record(sequence: int, stable_id: str, category: str, seat_id: int | None, route: str) -> dict[str, Any]:
            return {
                "record": "input",
                "sequence": sequence,
                "timestamp_us": 100 + sequence,
                "device_id": stable_id,
                "device_path": "fixture",
                "raw_type": 1 if category == "keyboard" else 0,
                "vkey": None,
                "key_code_redacted": True,
                "key_transition": "Down" if category == "keyboard" else "None",
                "delta_x": 0,
                "delta_y": 0,
                "wheel_delta": 0,
                "touchpad": False,
                "route": route,
                "seat_id": seat_id,
                "target_hwnd": 100 + seat_id if seat_id is not None else 0,
                "physical_suppression_requested": False,
                "isolation_guarantee": ISOLATION_GUARANTEE,
            }

        gate_a_records = [
            input_record(10 + index, stable, category, None, "UnassignedDevice")
            for index, (stable, _, category, _, _) in enumerate(identities)
        ]
        gate_a_records.extend(
            [
                {
                    "record": "device_change",
                    "sequence": 20,
                    "timestamp_us": 200,
                    "change": "Removal",
                    "device_id": identities[0][0],
                    "device_path": "fixture",
                    "raw_type": 1,
                    "touchpad": False,
                    "online": False,
                },
                {
                    "record": "device_change",
                    "sequence": 21,
                    "timestamp_us": 210,
                    "change": "Arrival",
                    "device_id": identities[0][0],
                    "device_path": "fixture",
                    "raw_type": 1,
                    "touchpad": False,
                    "online": True,
                },
            ]
        )
        routed_records = [
            input_record(30 + index, stable, category, seat_id, "Routed")
            for index, (stable, _, category, seat_id, _) in enumerate(identities)
        ]
        shared_record = input_record(40, identities[0][0], "keyboard", None, "AmbiguousSharedDevice")

        def write_trace(path: Path, records: list[dict[str, Any]]) -> None:
            with path.open("w", encoding="utf-8", newline="\n") as handle:
                for record in records:
                    handle.write(json.dumps(record, separators=(",", ":")) + "\n")

        write_trace(gate_a, gate_a_records)
        write_trace(gate_b, routed_records)
        write_trace(gate_b_shared, [shared_record])
        write_trace(gate_c, routed_records)

        clean_metrics = {
            "schema_version": 1,
            "trace_samples": 24,
            "unique_input_events": 4,
            "complete_input_events": 4,
            "missing_stage_events": 0,
            "receiver_verified_events": 4,
            "missing_receiver_evidence_events": 0,
            "cross_seat_events": 0,
            "cross_process_events": 0,
            "event_classes": {"key": 2, "button": 0, "movement": 2, "wheel": 0},
            "queue": {"high_water": 1, "dropped_frames": 0},
            "recorder": {"rotation_drops": 0, "contention_drops": 0, "invalid_samples": 0},
            "latency": {},
            "resource": {},
        }
        write_json(metrics, clean_metrics)

        expected_ownership_rows = [
            {"device_id": stable, "category": category, "seat_id": seat_id}
            for stable, _, category, seat_id, _ in identities
        ]
        native_scope_rows = [
            {
                "device_id": stable,
                "instance_id": instance_id,
                "category": category,
                "seat_id": seat_id,
            }
            for stable, instance_id, category, seat_id, _ in identities
        ]
        manifest = {
            "schema_version": 1,
            "session_id": "fixture",
            "created_utc": utc_now(),
            "updated_utc": utc_now(),
            "state": "READY_FOR_REVIEW",
            "privacy": {"sensitive_key_ids_enabled": False, "notice_acknowledged": True},
            "environment": {"windows_version": "fixture", "windows_build": "fixture", "architecture": "x64", "hardware_notes": "fixture"},
            "profile": {
                "source_path": str(profile_path),
                "sha256": sha256_file(profile_path),
                "schema_version": 2,
                "expected_ownership": expected_ownership_rows,
                "native_hidhide_scope": native_scope_rows,
                "shareable_resources": [],
                "shared_case": {
                    "derived_profile": shared_profile.name,
                    "sha256": sha256_file(shared_profile),
                    "device_id": identities[0][0],
                    "category": "keyboard",
                },
            },
            "stages": {
                "gate_a": _stage(REQUIRED_MANUAL_CHECKS["gate_a"], gate_a),
                "gate_b": _stage(REQUIRED_MANUAL_CHECKS["gate_b"], gate_b),
                "gate_c": _stage(REQUIRED_MANUAL_CHECKS["gate_c"], gate_c, metrics),
            },
            "manual_verdict": "PENDING",
            "manual_verdict_note": "",
            "manual_verdict_unix": None,
            "evidence_valid_until_unix": None,
        }
        manifest["stages"]["gate_b"]["auxiliary_traces"] = [gate_b_shared.name]
        manifest["stages"]["gate_b"]["auxiliary_trace_sha256"] = [sha256_file(gate_b_shared)]
        manifest_path = root / "manifest.json"
        write_json(manifest_path, manifest)

        pending = summarize(manifest_path)
        if pending["final_verdict"] != "PENDING" or pending["errors"]:
            raise AssertionError(f"process evidence alone must remain PENDING: {pending}")

        for stage_name, required in REQUIRED_MANUAL_CHECKS.items():
            for check_name in required:
                manifest["stages"][stage_name]["manual_checks"][check_name] = "PASS"
        now_unix = int(time.time())
        manifest["manual_verdict"] = "PASS"
        manifest["manual_verdict_unix"] = now_unix
        manifest["evidence_valid_until_unix"] = now_unix + EVIDENCE_VALIDITY_SECONDS
        manifest["state"] = "MANUAL_PASS"
        write_json(manifest_path, manifest)
        first_pass = summarize(manifest_path)
        if (
            first_pass["native_hidhide_activation_verdict"] != "PASS"
            or first_pass["physical_isolation_release_verdict"] != "PENDING"
            or first_pass["final_verdict"] != "PENDING"
            or first_pass["physical_device_suppression_evidence_present"] is not False
            or first_pass["errors"]
        ):
            raise AssertionError(
                f"clean Gate evidence may authorize guarded HidHide activation but must not claim physical suppression: {first_pass}"
            )
        for stage_name in ("gate_a", "gate_b", "gate_c"):
            manifest["stages"][stage_name]["verdict"] = first_pass["stages"][stage_name]["verdict"]
        write_json(manifest_path, manifest)
        passed = summarize(manifest_path)
        if (
            passed["native_hidhide_activation_verdict"] != "PASS"
            or passed["physical_isolation_release_verdict"] != "PENDING"
            or passed["final_verdict"] != "PENDING"
            or passed["errors"]
        ):
            raise AssertionError(
                f"persisted clean Gate verdicts must remain release-PENDING without physical suppression evidence: {passed}"
            )

        with gate_b.open("ab") as handle:
            handle.write(b"\n")
        tampered = summarize(manifest_path)
        if tampered["final_verdict"] != "FAIL" or not any(
            "SHA-256 mismatch" in item for item in tampered["errors"]
        ):
            raise AssertionError("post-recording trace tamper must fail closed")
        write_trace(gate_b, routed_records)

        manifest["manual_verdict_unix"] = now_unix - EVIDENCE_VALIDITY_SECONDS - 1
        manifest["evidence_valid_until_unix"] = manifest["manual_verdict_unix"] + EVIDENCE_VALIDITY_SECONDS
        write_json(manifest_path, manifest)
        stale = summarize(manifest_path)
        if stale["final_verdict"] != "FAIL" or not any("stale" in item for item in stale["errors"]):
            raise AssertionError("stale P3-HW evidence must fail closed")
        manifest["manual_verdict_unix"] = now_unix
        manifest["evidence_valid_until_unix"] = now_unix + EVIDENCE_VALIDITY_SECONDS

        manifest["manual_verdict"] = "PENDING"
        manifest["manual_verdict_unix"] = None
        manifest["evidence_valid_until_unix"] = None
        manifest["state"] = "READY_FOR_REVIEW"
        write_json(manifest_path, manifest)
        pending_again = summarize(manifest_path)
        if pending_again["final_verdict"] != "PENDING" or pending_again["errors"]:
            raise AssertionError("PENDING manual evidence must remain fail-closed without becoming FAIL")
        manifest["manual_verdict"] = "PASS"
        manifest["manual_verdict_unix"] = now_unix
        manifest["evidence_valid_until_unix"] = now_unix + EVIDENCE_VALIDITY_SECONDS
        manifest["state"] = "MANUAL_PASS"

        mismatched_profile = json.loads(json.dumps(profile))
        mismatched_profile["seats"][0]["keyboards"] = ["Keyboard:USB\\VID_DEAD&PID_BEEF\\X"]
        write_json(profile_path, mismatched_profile)
        manifest["profile"]["sha256"] = sha256_file(profile_path)
        write_json(manifest_path, manifest)
        mismatch = summarize(manifest_path)
        if mismatch["final_verdict"] != "FAIL" or not any(
            "ownership does not match" in item or "native HidHide scope does not match" in item
            for item in mismatch["errors"]
        ):
            raise AssertionError("profile/Seat-device mismatch must fail closed")
        write_json(profile_path, profile)
        manifest["profile"]["sha256"] = sha256_file(profile_path)

        bad_receiver = dict(clean_metrics)
        bad_receiver["receiver_verified_events"] = 3
        bad_receiver["missing_receiver_evidence_events"] = 1
        write_json(metrics, bad_receiver)
        manifest["stages"]["gate_c"]["metrics_report_sha256"] = sha256_file(metrics)
        write_json(manifest_path, manifest)
        receiver_fail = summarize(manifest_path)
        if receiver_fail["final_verdict"] != "FAIL" or not any(
            "missing receiver evidence" in item for item in receiver_fail["errors"]
        ):
            raise AssertionError("missing receiver identity must be a hard failure")
        write_json(metrics, clean_metrics)
        manifest["stages"]["gate_c"]["metrics_report_sha256"] = sha256_file(metrics)

        cross_seat = dict(clean_metrics)
        cross_seat["cross_seat_events"] = 1
        write_json(metrics, cross_seat)
        manifest["stages"]["gate_c"]["metrics_report_sha256"] = sha256_file(metrics)
        write_json(manifest_path, manifest)
        bleed_fail = summarize(manifest_path)
        if bleed_fail["final_verdict"] != "FAIL" or not any(
            "cross-Seat" in item for item in bleed_fail["errors"]
        ):
            raise AssertionError("verified cross-Seat evidence must fail closed")

    print("P3-HW-01 trace/report parser self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return run_self_test()
    if args.manifest is None:
        parser.error("--manifest is required unless --self-test is used")
    report = summarize(args.manifest)
    if args.output is not None:
        write_json(args.output, report)
    else:
        print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    return 0 if report["final_verdict"] != "FAIL" else 2


if __name__ == "__main__":
    raise SystemExit(main())
