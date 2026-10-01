#!/usr/bin/env python3
"""Fail-closed static validation for the P8-SIGN-01 release signing allowlist."""

from __future__ import annotations

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "config" / "release-signing-manifest.json"
ID_RE = re.compile(r"^[A-Za-z0-9._:-]{1,96}$")
TARGET_RE = re.compile(r"^[A-Za-z0-9_]{1,96}$")
EXE_RE = re.compile(r"^[A-Za-z0-9._-]{1,128}\.exe$")
SCRIPT_RE = re.compile(r"^[A-Za-z0-9._-]{1,128}\.ps1$")
ALLOWED_ARCHITECTURES = {"x64"}
EXPECTED_TARGETS = {
    "HydraSeat": "HydraSeat.exe",
    "hydra_host": "hydra_host.exe",
    "hydra_seat_ui": "hydra_seat_ui.exe",
    "hydra_watchdog": "hydra_watchdog.exe",
    "hydra_reset": "hydra_reset.exe",
    "hydraseat_profilectl": "hydraseat_profilectl.exe",
    "hydraseat_community_validate": "hydraseat_community_validate.exe",
}
EXPECTED_SCRIPT_ID = "installer-script"
EXPECTED_SCRIPT_SOURCE = "tools/install_hydraseat.ps1"
EXPECTED_SCRIPT_FILE = "install_hydraseat.ps1"
EXPECTED_ARTIFACT_COUNT = len(EXPECTED_TARGETS) + 1


def fail(message: str) -> None:
    raise ValueError(message)


def validate_architectures(value: object) -> None:
    if not isinstance(value, list) or set(value) != ALLOWED_ARCHITECTURES:
        fail("each release artifact must explicitly target the reviewed x64 host architecture")
    if len(value) != len(ALLOWED_ARCHITECTURES):
        fail("architecture list must not contain duplicates")


def safe_basename(value: object, pattern: re.Pattern[str]) -> str:
    if not isinstance(value, str) or not pattern.fullmatch(value):
        fail("release file name has an invalid extension or basename")
    if "/" in value or "\\" in value or ".." in value or "*" in value or "?" in value:
        fail("release file name cannot contain path traversal or wildcard syntax")
    return value


def validate(path: pathlib.Path) -> None:
    raw = path.read_text(encoding="utf-8")
    data = json.loads(raw)
    if not isinstance(data, dict) or set(data) != {
        "schemaVersion",
        "releaseScope",
        "artifacts",
        "excludedArtifactClasses",
    }:
        fail("manifest must contain exactly schemaVersion/releaseScope/artifacts/excludedArtifactClasses")
    if data["schemaVersion"] != 1:
        fail("unsupported release signing manifest version")
    if not isinstance(data["releaseScope"], str) or not (1 <= len(data["releaseScope"]) <= 512):
        fail("releaseScope must be bounded non-empty text")
    if not isinstance(data["excludedArtifactClasses"], list) or not data["excludedArtifactClasses"]:
        fail("excluded artifact classes must be explicit")
    for item in data["excludedArtifactClasses"]:
        if not isinstance(item, str) or not (1 <= len(item) <= 256):
            fail("excluded artifact class text is invalid")

    artifacts = data["artifacts"]
    if not isinstance(artifacts, list) or len(artifacts) != EXPECTED_ARTIFACT_COUNT:
        fail("release signing allowlist must contain the exact reviewed v1 artifact count")

    seen_ids: set[str] = set()
    seen_targets: set[str] = set()
    saw_script = False
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            fail("artifact entry must be an object")
        artifact_id = artifact.get("id")
        kind = artifact.get("kind")
        architectures = artifact.get("architectures")
        if not isinstance(artifact_id, str) or not ID_RE.fullmatch(artifact_id):
            fail("artifact id is invalid")
        if artifact_id in seen_ids:
            fail("duplicate artifact id")
        seen_ids.add(artifact_id)
        validate_architectures(architectures)

        if kind == "cmake-executable":
            if set(artifact) != {"id", "kind", "target", "fileName", "architectures"}:
                fail("CMake executable entry contains unknown/missing fields")
            target = artifact["target"]
            if not isinstance(target, str) or not TARGET_RE.fullmatch(target):
                fail("target name is invalid")
            if target in seen_targets:
                fail("duplicate CMake target")
            seen_targets.add(target)
            if target not in EXPECTED_TARGETS:
                fail(f"unreviewed release signing target: {target}")
            file_name = safe_basename(artifact["fileName"], EXE_RE)
            if EXPECTED_TARGETS[target] != file_name:
                fail(f"file name does not match reviewed target: {target}")
        elif kind == "powershell-script":
            if set(artifact) != {"id", "kind", "sourcePath", "fileName", "architectures"}:
                fail("PowerShell script entry contains unknown/missing fields")
            if artifact_id != EXPECTED_SCRIPT_ID or saw_script:
                fail("only the reviewed installer PowerShell script may be signed")
            source_path = artifact["sourcePath"]
            if source_path != EXPECTED_SCRIPT_SOURCE:
                fail("PowerShell signing source must be the reviewed installer script")
            if ".." in source_path or "*" in source_path or "?" in source_path or "\\" in source_path:
                fail("PowerShell sourcePath is unsafe")
            resolved = (ROOT / source_path).resolve()
            if resolved != (ROOT / EXPECTED_SCRIPT_SOURCE).resolve() or not resolved.is_file():
                fail("reviewed installer script is missing or resolves outside the expected file")
            file_name = safe_basename(artifact["fileName"], SCRIPT_RE)
            if file_name != EXPECTED_SCRIPT_FILE:
                fail("PowerShell release file name is not the reviewed installer script")
            saw_script = True
        else:
            fail("unknown release signing artifact kind")

    if seen_targets != set(EXPECTED_TARGETS):
        fail("release signing allowlist is missing a reviewed v1 CMake target")
    if not saw_script:
        fail("release signing allowlist is missing the reviewed installer script")


def self_test() -> None:
    validate(DEFAULT_MANIFEST)
    source = json.loads(DEFAULT_MANIFEST.read_text(encoding="utf-8"))
    tmp = ROOT / ".ai-bridge" / "release-signing-validator-self-test.json"
    tmp.parent.mkdir(parents=True, exist_ok=True)
    try:
        cases = []
        bad = json.loads(json.dumps(source))
        bad["artifacts"][0]["fileName"] = "..\\evil.exe"
        cases.append(bad)
        bad = json.loads(json.dumps(source))
        bad["artifacts"][-1]["sourcePath"] = "tools/other.ps1"
        cases.append(bad)
        bad = json.loads(json.dumps(source))
        bad["artifacts"].append(json.loads(json.dumps(source["artifacts"][0])))
        cases.append(bad)
        bad = json.loads(json.dumps(source))
        bad["artifacts"][0]["architectures"] = ["x64", "x86"]
        cases.append(bad)
        for index, case in enumerate(cases):
            tmp.write_text(json.dumps(case), encoding="utf-8")
            try:
                validate(tmp)
            except ValueError:
                pass
            else:
                fail(f"self-test case {index} was not rejected")
    finally:
        tmp.unlink(missing_ok=True)


def main(argv: list[str]) -> int:
    path = DEFAULT_MANIFEST
    if len(argv) == 2 and argv[1] == "--self-test":
        self_test()
        print("Release signing manifest validator self-test passed.")
        return 0
    if len(argv) > 2:
        print("usage: validate_release_signing_manifest.py [manifest.json|--self-test]", file=sys.stderr)
        return 2
    if len(argv) == 2:
        path = pathlib.Path(argv[1]).resolve()
    try:
        validate(path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"Release signing manifest invalid: {exc}", file=sys.stderr)
        return 1
    print(f"Release signing manifest valid: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
