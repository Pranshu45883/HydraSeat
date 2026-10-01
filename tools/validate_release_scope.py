#!/usr/bin/env python3
"""Fail-closed validator for the P10-SCOPE-01 HydraSeat v1 release matrix."""

from __future__ import annotations

import copy
import datetime as dt
import json
import pathlib
import re
import sys
import tempfile
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_SCOPE = ROOT / "config" / "release-scope-v1.json"

ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,95}$")
VALIDATION_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,95}$")
DISPOSITIONS = {"release-target", "experimental", "deferred", "excluded"}
EXPECTED_SEMANTICS = DISPOSITIONS
EXPECTED_WINDOWS_RELEASE_TARGETS = {
    "windows-11-24h2": (11, "24H2", 26100),
    "windows-11-25h2": (11, "25H2", 26200),
    "windows-11-26h1": (11, "26H1", 28000),
}
EXPECTED_WINDOWS_EXPERIMENTAL = {
    "windows-10-22h2-esu": (10, "22H2", 19045),
}
EXPECTED_CAPABILITIES = {
    "display.physical": "release-target",
    "display.virtual": "deferred",
    "input.keyboard-mouse": "release-target",
    "controller.xinput": "release-target",
    "controller.directinput": "release-target",
    "controller.raw-hid-sdl-vendor": "experimental",
    "audio.core-audio": "release-target",
    "provider.steam-readonly-discovery": "release-target",
    "provider.custom-exe": "release-target",
    "provider.epic-ea-gog": "deferred",
    "same-game.two-instance": "release-target",
    "protected-title.experiment": "experimental",
    "optional-device-driver": "experimental",
    "community.compatibility-sharing": "release-target",
}
EXPECTED_NON_GOALS = {
    "n-seat-v1",
    "general-independent-desktops",
    "vm-rdp-streaming-required",
    "universal-same-game-multi-instance",
    "anti-cheat-drm-account-launcher-bypass",
    "anti-cheat-safety-certification",
    "mandatory-cloud-account-or-telemetry",
    "compatibility-with-every-game",
}
EXPECTED_PRIVILEGED_OPERATIONS = {
    "install",
    "repair",
    "uninstall",
    "optional-component",
    "explicit-system-recovery",
}


def fail(message: str) -> None:
    raise ValueError(message)


def require_exact_keys(value: Any, expected: set[str], context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(f"{context} must be an object")
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        fail(f"{context} fields mismatch; missing={missing}, extra={extra}")
    return value


def require_text(value: Any, context: str, *, maximum: int = 1024) -> str:
    if not isinstance(value, str) or not (1 <= len(value) <= maximum):
        fail(f"{context} must be bounded non-empty text")
    if "\x00" in value:
        fail(f"{context} cannot contain NUL")
    return value


def require_bool(value: Any, expected: bool, context: str) -> None:
    if value is not expected:
        fail(f"{context} must be {str(expected).lower()}")


def require_id(value: Any, context: str) -> str:
    text = require_text(value, context, maximum=96)
    if not ID_RE.fullmatch(text):
        fail(f"{context} contains invalid characters")
    return text


def validate_scope_semantics(value: Any) -> None:
    semantics = require_exact_keys(value, EXPECTED_SEMANTICS, "scopeSemantics")
    for disposition, description in semantics.items():
        require_text(description, f"scopeSemantics.{disposition}", maximum=512)
    if "not a support claim" not in semantics["release-target"].lower():
        fail("release-target semantics must explicitly deny an unvalidated support claim")


def validate_product_boundary(value: Any) -> None:
    boundary = require_exact_keys(
        value,
        {
            "maxActiveSeats",
            "gameOnly",
            "singleInteractiveWindowsSession",
            "minimalSeatLauncher",
            "generalIndependentDesktopPerSeat",
            "vmRdpStreamingRequired",
            "mandatoryHydraSeatCloudAccount",
        },
        "productBoundary",
    )
    if boundary["maxActiveSeats"] != 2:
        fail("HydraSeat v1 must freeze maxActiveSeats at exactly 2")
    require_bool(boundary["gameOnly"], True, "productBoundary.gameOnly")
    require_bool(
        boundary["singleInteractiveWindowsSession"],
        True,
        "productBoundary.singleInteractiveWindowsSession",
    )
    require_bool(boundary["minimalSeatLauncher"], True, "productBoundary.minimalSeatLauncher")
    require_bool(
        boundary["generalIndependentDesktopPerSeat"],
        False,
        "productBoundary.generalIndependentDesktopPerSeat",
    )
    require_bool(boundary["vmRdpStreamingRequired"], False, "productBoundary.vmRdpStreamingRequired")
    require_bool(
        boundary["mandatoryHydraSeatCloudAccount"],
        False,
        "productBoundary.mandatoryHydraSeatCloudAccount",
    )


def validate_windows_family(entry: Any) -> tuple[str, str]:
    item = require_exact_keys(
        entry,
        {"id", "majorVersion", "release", "baseBuild", "disposition", "validation"},
        "hostPlatform.windowsBuildFamilies[]",
    )
    item_id = require_id(item["id"], "windows build id")
    if type(item["majorVersion"]) is not int or item["majorVersion"] not in {10, 11}:
        fail(f"{item_id}: majorVersion must be 10 or 11")
    release = require_text(item["release"], f"{item_id}.release", maximum=16)
    if type(item["baseBuild"]) is not int or not (10000 <= item["baseBuild"] <= 99999):
        fail(f"{item_id}: baseBuild is outside the bounded Windows build range")
    disposition = require_text(item["disposition"], f"{item_id}.disposition", maximum=32)
    if disposition not in DISPOSITIONS:
        fail(f"{item_id}: unknown disposition {disposition}")
    validation = require_text(item["validation"], f"{item_id}.validation", maximum=96)
    if not VALIDATION_RE.fullmatch(validation):
        fail(f"{item_id}: validation token is invalid")
    return item_id, disposition


def validate_host_platform(value: Any) -> None:
    host = require_exact_keys(
        value,
        {
            "operatingSystem",
            "hostArchitecture",
            "targetProcessArchitectures",
            "windowsBuildFamilies",
            "cpuBaseline",
            "gpuBaseline",
        },
        "hostPlatform",
    )
    if host["operatingSystem"] != "Windows":
        fail("v1 host operatingSystem must be Windows")
    if host["hostArchitecture"] != "x64":
        fail("v1 GA host architecture is frozen to x64")
    architectures = host["targetProcessArchitectures"]
    if not isinstance(architectures, list) or architectures != ["x64", "x86"]:
        fail("targetProcessArchitectures must be the canonical [x64, x86] compatibility order")
    require_text(host["cpuBaseline"], "hostPlatform.cpuBaseline", maximum=512)
    require_text(host["gpuBaseline"], "hostPlatform.gpuBaseline", maximum=512)

    families = host["windowsBuildFamilies"]
    if not isinstance(families, list) or not families or len(families) > 16:
        fail("windowsBuildFamilies must contain 1..16 explicit build families")
    seen: set[str] = set()
    by_id: dict[str, dict[str, Any]] = {}
    dispositions: dict[str, str] = {}
    for raw_entry in families:
        item_id, disposition = validate_windows_family(raw_entry)
        if item_id in seen:
            fail(f"duplicate windows build id: {item_id}")
        seen.add(item_id)
        by_id[item_id] = raw_entry
        dispositions[item_id] = disposition

    expected_ids = set(EXPECTED_WINDOWS_RELEASE_TARGETS) | set(EXPECTED_WINDOWS_EXPERIMENTAL)
    if seen != expected_ids:
        fail(f"windows build family freeze drift; expected={sorted(expected_ids)}, actual={sorted(seen)}")

    for item_id, expected in EXPECTED_WINDOWS_RELEASE_TARGETS.items():
        entry = by_id[item_id]
        if (entry["majorVersion"], entry["release"], entry["baseBuild"]) != expected:
            fail(f"{item_id}: frozen Windows release/build tuple changed")
        if dispositions[item_id] != "release-target":
            fail(f"{item_id}: must remain inside the v1 release qualification matrix")

    for item_id, expected in EXPECTED_WINDOWS_EXPERIMENTAL.items():
        entry = by_id[item_id]
        if (entry["majorVersion"], entry["release"], entry["baseBuild"]) != expected:
            fail(f"{item_id}: frozen Windows release/build tuple changed")
        if dispositions[item_id] != "experimental":
            fail(f"{item_id}: Windows 10 must not become a GA release-target implicitly")


def validate_capabilities(value: Any) -> None:
    if not isinstance(value, list) or len(value) != len(EXPECTED_CAPABILITIES):
        fail("capabilities must contain the exact reviewed v1 capability matrix")
    seen: set[str] = set()
    for raw_entry in value:
        entry = require_exact_keys(
            raw_entry,
            {"id", "disposition", "validation", "notes"},
            "capabilities[]",
        )
        item_id = require_id(entry["id"], "capability id")
        if item_id in seen:
            fail(f"duplicate capability id: {item_id}")
        seen.add(item_id)
        disposition = require_text(entry["disposition"], f"{item_id}.disposition", maximum=32)
        if disposition not in DISPOSITIONS:
            fail(f"{item_id}: unknown capability disposition")
        validation = require_text(entry["validation"], f"{item_id}.validation", maximum=96)
        if not VALIDATION_RE.fullmatch(validation):
            fail(f"{item_id}: invalid validation token")
        require_text(entry["notes"], f"{item_id}.notes", maximum=768)
        expected = EXPECTED_CAPABILITIES.get(item_id)
        if expected is None:
            fail(f"unreviewed capability in v1 scope: {item_id}")
        if disposition != expected:
            fail(f"{item_id}: expected disposition {expected}, got {disposition}")
    if seen != set(EXPECTED_CAPABILITIES):
        fail("capability matrix is missing a reviewed v1 boundary")


def validate_runtime_policy(value: Any) -> None:
    policy = require_exact_keys(
        value,
        {
            "coreOfflineOperationRequired",
            "communitySharingDefaultEnabled",
            "compatibilityDataRefreshSeparateFromProgramUpdate",
            "programRuntimeDriverUpdateRequiresUserApproval",
            "normalUiRuntimeRequiresElevation",
            "privilegedOperations",
            "privilegedGeneralCommandExecutionAllowed",
        },
        "runtimePolicy",
    )
    require_bool(policy["coreOfflineOperationRequired"], True, "runtimePolicy.coreOfflineOperationRequired")
    require_bool(policy["communitySharingDefaultEnabled"], False, "runtimePolicy.communitySharingDefaultEnabled")
    require_bool(
        policy["compatibilityDataRefreshSeparateFromProgramUpdate"],
        True,
        "runtimePolicy.compatibilityDataRefreshSeparateFromProgramUpdate",
    )
    require_bool(
        policy["programRuntimeDriverUpdateRequiresUserApproval"],
        True,
        "runtimePolicy.programRuntimeDriverUpdateRequiresUserApproval",
    )
    require_bool(
        policy["normalUiRuntimeRequiresElevation"],
        False,
        "runtimePolicy.normalUiRuntimeRequiresElevation",
    )
    require_bool(
        policy["privilegedGeneralCommandExecutionAllowed"],
        False,
        "runtimePolicy.privilegedGeneralCommandExecutionAllowed",
    )
    operations = policy["privilegedOperations"]
    if not isinstance(operations, list) or set(operations) != EXPECTED_PRIVILEGED_OPERATIONS:
        fail("runtimePolicy.privilegedOperations must match the narrow reviewed allowlist")
    if len(operations) != len(EXPECTED_PRIVILEGED_OPERATIONS):
        fail("runtimePolicy.privilegedOperations cannot contain duplicates")


def validate_installer_policy(value: Any) -> None:
    policy = require_exact_keys(
        value,
        {
            "realWindowsInstallerRequired",
            "repairRequired",
            "uninstallRequired",
            "firstRunSeatWizardOptional",
            "setLaterRequired",
            "returnToOrdinaryWindowsBeforeRiskyRemoval",
            "removeOnlyHydraSeatOwnedState",
            "cleanMachineAcceptanceRequired",
        },
        "installerPolicy",
    )
    for key, expected in {
        "realWindowsInstallerRequired": True,
        "repairRequired": True,
        "uninstallRequired": True,
        "firstRunSeatWizardOptional": True,
        "setLaterRequired": True,
        "returnToOrdinaryWindowsBeforeRiskyRemoval": True,
        "removeOnlyHydraSeatOwnedState": True,
        "cleanMachineAcceptanceRequired": True,
    }.items():
        require_bool(policy[key], expected, f"installerPolicy.{key}")


def validate_non_goals(value: Any) -> None:
    if not isinstance(value, list) or set(value) != EXPECTED_NON_GOALS:
        fail("nonGoals must match the canonical v1 excluded-scope set")
    if len(value) != len(EXPECTED_NON_GOALS):
        fail("nonGoals cannot contain duplicates")
    for item in value:
        require_id(item, "nonGoal id")


def validate_external_research(value: Any, scope_date: str) -> None:
    if not isinstance(value, list) or len(value) != 1:
        fail("externalResearch must contain the exact official platform source used for this freeze")
    entry = require_exact_keys(
        value[0],
        {"id", "classification", "url", "consultedOn", "use"},
        "externalResearch[]",
    )
    if entry["id"] != "microsoft-windows-client-supported-versions":
        fail("unexpected external research source id")
    if entry["classification"] != "official-platform-documentation":
        fail("Windows support research must remain classified as official platform documentation")
    url = require_text(entry["url"], "externalResearch.url", maximum=512)
    if url != "https://learn.microsoft.com/en-us/windows/release-health/supported-versions-windows-client":
        fail("Windows support source must be the reviewed Microsoft Learn page")
    if entry["consultedOn"] != scope_date:
        fail("externalResearch.consultedOn must match scopeDate")
    use = require_text(entry["use"], "externalResearch.use", maximum=512).lower()
    if "no source code copied" not in use:
        fail("external research record must state that no source code was copied")


def validate(path: pathlib.Path) -> None:
    raw = path.read_text(encoding="utf-8")
    data = json.loads(raw)
    root = require_exact_keys(
        data,
        {
            "schemaVersion",
            "productVersion",
            "scopeDate",
            "scopeSemantics",
            "productBoundary",
            "hostPlatform",
            "capabilities",
            "runtimePolicy",
            "installerPolicy",
            "nonGoals",
            "externalResearch",
        },
        "release scope",
    )
    if root["schemaVersion"] != 1:
        fail("unsupported release scope schemaVersion")
    if root["productVersion"] != "v1":
        fail("release scope productVersion must be v1")
    scope_date = require_text(root["scopeDate"], "scopeDate", maximum=10)
    try:
        parsed = dt.date.fromisoformat(scope_date)
    except ValueError as exc:
        fail(f"scopeDate must be ISO YYYY-MM-DD: {exc}")
    if parsed.isoformat() != scope_date:
        fail("scopeDate must use canonical ISO YYYY-MM-DD form")

    validate_scope_semantics(root["scopeSemantics"])
    validate_product_boundary(root["productBoundary"])
    validate_host_platform(root["hostPlatform"])
    validate_capabilities(root["capabilities"])
    validate_runtime_policy(root["runtimePolicy"])
    validate_installer_policy(root["installerPolicy"])
    validate_non_goals(root["nonGoals"])
    validate_external_research(root["externalResearch"], scope_date)


def expect_rejected(source: dict[str, Any], mutate: Any, label: str) -> None:
    bad = copy.deepcopy(source)
    mutate(bad)
    with tempfile.TemporaryDirectory(prefix="hydraseat-release-scope-") as temp_dir:
        path = pathlib.Path(temp_dir) / "scope.json"
        path.write_text(json.dumps(bad), encoding="utf-8")
        try:
            validate(path)
        except ValueError:
            return
    fail(f"self-test failed to reject {label}")


def self_test() -> None:
    validate(DEFAULT_SCOPE)
    source = json.loads(DEFAULT_SCOPE.read_text(encoding="utf-8"))

    expect_rejected(
        source,
        lambda value: value["productBoundary"].__setitem__("maxActiveSeats", 3),
        "three active Seats",
    )
    expect_rejected(
        source,
        lambda value: value["hostPlatform"].__setitem__("hostArchitecture", "x86"),
        "x86 GA host architecture",
    )
    expect_rejected(
        source,
        lambda value: value["hostPlatform"]["windowsBuildFamilies"][3].__setitem__(
            "disposition", "release-target"
        ),
        "implicit Windows 10 GA support claim",
    )
    expect_rejected(
        source,
        lambda value: next(
            item for item in value["capabilities"] if item["id"] == "display.virtual"
        ).__setitem__("disposition", "release-target"),
        "virtual display promotion",
    )
    expect_rejected(
        source,
        lambda value: value["runtimePolicy"].__setitem__(
            "privilegedGeneralCommandExecutionAllowed", True
        ),
        "general privileged command execution",
    )
    expect_rejected(
        source,
        lambda value: value["nonGoals"].remove("anti-cheat-drm-account-launcher-bypass"),
        "removal of no-bypass boundary",
    )


def main(argv: list[str]) -> int:
    path = DEFAULT_SCOPE
    if len(argv) == 2 and argv[1] == "--self-test":
        try:
            self_test()
        except (OSError, json.JSONDecodeError, ValueError) as exc:
            print(f"Release scope validator self-test failed: {exc}", file=sys.stderr)
            return 1
        print("Release scope validator self-test passed.")
        return 0
    if len(argv) > 2:
        print("usage: validate_release_scope.py [scope.json|--self-test]", file=sys.stderr)
        return 2
    if len(argv) == 2:
        path = pathlib.Path(argv[1]).resolve()
    try:
        validate(path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"Release scope invalid: {exc}", file=sys.stderr)
        return 1
    print(f"Release scope valid: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
