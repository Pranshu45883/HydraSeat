#!/usr/bin/env python3
"""Deterministic, non-destructive HydraSeat pre-merge orchestration gate.

This wrapper deliberately does not duplicate repository validation rules. It runs a
fixed allowlist of existing live validators, reports optional x64/x86 build-tree
availability separately, and leaves external/manual acceptance classes PENDING.
Only failures from the automated validator class affect the process exit status.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]

DEFAULT_TIMEOUT_SECONDS = 120
MAX_TIMEOUT_SECONDS = 300


@dataclass(frozen=True)
class ValidatorSpec:
    name: str
    script: str


@dataclass(frozen=True)
class AutomatedResult:
    name: str
    status: str
    return_code: int
    summary: str


@dataclass(frozen=True)
class AvailabilityResult:
    architecture: str
    status: str
    path: str
    detail: str


@dataclass(frozen=True)
class ManualGateResult:
    gate: str
    status: str
    detail: str


AUTOMATED_VALIDATORS: tuple[ValidatorSpec, ...] = (
    ValidatorSpec("implementation-roadmap", "tools/validate_implementation_roadmap.py"),
    ValidatorSpec("production-reachability", "tools/validate_production_reachability.py"),
    ValidatorSpec("release-scope", "tools/validate_release_scope.py"),
    ValidatorSpec("release-signing-manifest-structure", "tools/validate_release_signing_manifest.py"),
    ValidatorSpec("release-installer-contract", "tools/validate_release_installer_contract.py"),
)

MANUAL_GATE_NAMES: tuple[str, ...] = (
    "Physical",
    "Real-game",
    "Clean-machine",
    "Signing",
)


def _bounded_summary(stdout: str, stderr: str, maximum: int = 500) -> str:
    lines = [line.strip() for line in (stdout + "\n" + stderr).splitlines() if line.strip()]
    if not lines:
        return "no output"
    text = lines[-1]
    if len(text) <= maximum:
        return text
    return text[: maximum - 3] + "..."


def run_validator(root: Path, spec: ValidatorSpec, timeout_seconds: int) -> AutomatedResult:
    script = root / spec.script
    if not script.is_file():
        return AutomatedResult(
            spec.name,
            "FAIL",
            127,
            f"required validator missing: {spec.script}",
        )

    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    try:
        completed = subprocess.run(
            [sys.executable, str(script)],
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return AutomatedResult(
            spec.name,
            "FAIL",
            124,
            f"validator exceeded {timeout_seconds}s timeout",
        )
    except OSError as exc:
        return AutomatedResult(spec.name, "FAIL", 126, f"validator could not start: {exc}")

    return AutomatedResult(
        spec.name,
        "PASS" if completed.returncode == 0 else "FAIL",
        completed.returncode,
        _bounded_summary(completed.stdout, completed.stderr),
    )


def run_automated_validators(
    root: Path,
    specs: Sequence[ValidatorSpec],
    timeout_seconds: int,
) -> list[AutomatedResult]:
    return [run_validator(root, spec, timeout_seconds) for spec in specs]


def normalize_local_path(raw_path: Path) -> Path:
    text = str(raw_path)
    if os.name != "nt" and len(text) >= 3 and text[1] == ":" and text[2] in {"/", "\\"}:
        drive = text[0].lower()
        suffix = text[2:].replace("\\", "/").lstrip("/")
        return Path(f"/mnt/{drive}/{suffix}").resolve()
    return raw_path.expanduser().resolve()


def inspect_build_tree(architecture: str, raw_path: Path | None) -> AvailabilityResult:
    if raw_path is None:
        return AvailabilityResult(
            architecture,
            "UNSPECIFIED",
            "",
            "no build directory was supplied; availability is not inferred from dirty/generated trees",
        )

    path = normalize_local_path(raw_path)
    if not path.is_dir():
        return AvailabilityResult(
            architecture,
            "UNAVAILABLE",
            str(path),
            "build directory does not exist",
        )

    cache = path / "CMakeCache.txt"
    ctest_file = path / "CTestTestfile.cmake"
    if cache.is_file() and ctest_file.is_file():
        tool = shutil.which("ctest") or shutil.which("ctest.exe")
        detail = "CMakeCache.txt and CTestTestfile.cmake present"
        detail += "; ctest executable available" if tool else "; ctest executable unavailable"
        return AvailabilityResult(architecture, "AVAILABLE", str(path), detail)
    if cache.is_file() or ctest_file.is_file():
        missing = []
        if not cache.is_file():
            missing.append("CMakeCache.txt")
        if not ctest_file.is_file():
            missing.append("CTestTestfile.cmake")
        return AvailabilityResult(
            architecture,
            "PARTIAL",
            str(path),
            "missing " + ", ".join(missing),
        )
    return AvailabilityResult(
        architecture,
        "UNAVAILABLE",
        str(path),
        "directory has no configured CMake/CTest metadata",
    )


def manual_gate_results() -> list[ManualGateResult]:
    return [
        ManualGateResult(
            gate,
            "PENDING",
            "manual/external evidence is not evaluated or promoted by the automated pre-merge gate",
        )
        for gate in MANUAL_GATE_NAMES
    ]


def automated_passed(results: Sequence[AutomatedResult]) -> bool:
    return bool(results) and all(result.status == "PASS" for result in results)


def gate_exit_code(results: Sequence[AutomatedResult]) -> int:
    return 0 if automated_passed(results) else 1


def render_text(
    automated: Sequence[AutomatedResult],
    availability: Sequence[AvailabilityResult],
    manual: Sequence[ManualGateResult],
) -> str:
    lines = ["HydraSeat pre-merge gate", "[AUTOMATED]"]
    for result in automated:
        lines.append(
            f"{result.status:<4} {result.name} rc={result.return_code} :: {result.summary}"
        )
    passed = sum(result.status == "PASS" for result in automated)
    automated_state = "PASS" if automated_passed(automated) else "FAIL"
    lines.append(f"AUTOMATED_SUMMARY {automated_state} {passed}/{len(automated)}")

    lines.append("[BUILD_TEST_AVAILABILITY]")
    for result in availability:
        path = f" path={result.path}" if result.path else ""
        lines.append(f"{result.architecture} {result.status}{path} :: {result.detail}")

    lines.append("[MANUAL_GATES]")
    for result in manual:
        lines.append(f"{result.gate} {result.status} :: {result.detail}")

    lines.append(
        "OVERALL "
        + ("PASS" if automated_passed(automated) else "FAIL")
        + " :: automated validators are authoritative for this command; "
        "build availability is informational; manual gates remain PENDING"
    )
    return "\n".join(lines)


def render_json(
    automated: Sequence[AutomatedResult],
    availability: Sequence[AvailabilityResult],
    manual: Sequence[ManualGateResult],
) -> str:
    document = {
        "automated": [asdict(item) for item in automated],
        "automatedStatus": "PASS" if automated_passed(automated) else "FAIL",
        "buildTestAvailability": [asdict(item) for item in availability],
        "manualGates": [asdict(item) for item in manual],
        "overallStatus": "PASS" if automated_passed(automated) else "FAIL",
    }
    return json.dumps(document, indent=2, sort_keys=True)


def _assert(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def self_test() -> None:
    pass_spec = ValidatorSpec("fixture-pass", "tools/testdata/premerge_gate/pass_validator.py")
    fail_spec = ValidatorSpec("fixture-fail", "tools/testdata/premerge_gate/fail_validator.py")
    missing_spec = ValidatorSpec("fixture-missing", "tools/testdata/premerge_gate/missing_validator.py")

    pass_result = run_validator(ROOT, pass_spec, 5)
    _assert(pass_result.status == "PASS" and pass_result.return_code == 0, "pass validator")

    fail_result = run_validator(ROOT, fail_spec, 5)
    _assert(fail_result.status == "FAIL" and fail_result.return_code == 7, "fail validator")

    missing_result = run_validator(ROOT, missing_spec, 5)
    _assert(missing_result.status == "FAIL" and missing_result.return_code == 127, "missing validator")

    _assert(automated_passed([pass_result]), "all-pass aggregation")
    _assert(gate_exit_code([pass_result]) == 0, "all-pass exit code")
    _assert(not automated_passed([pass_result, fail_result]), "failure aggregation")
    _assert(gate_exit_code([pass_result, fail_result]) != 0, "failure exit code")

    with tempfile.TemporaryDirectory(prefix="hydraseat-premerge-self-test-") as temp_dir:
        root = Path(temp_dir)
        available = root / "available"
        available.mkdir()
        (available / "CMakeCache.txt").write_text("fixture\n", encoding="utf-8")
        (available / "CTestTestfile.cmake").write_text("# fixture\n", encoding="utf-8")
        _assert(inspect_build_tree("x64", available).status == "AVAILABLE", "available build tree")

        partial = root / "partial"
        partial.mkdir()
        (partial / "CMakeCache.txt").write_text("fixture\n", encoding="utf-8")
        _assert(inspect_build_tree("x86", partial).status == "PARTIAL", "partial build tree")
        _assert(inspect_build_tree("x86", root / "missing").status == "UNAVAILABLE", "missing build tree")

    _assert(inspect_build_tree("x64", None).status == "UNSPECIFIED", "unspecified build tree")
    manual = manual_gate_results()
    _assert(len(manual) == 4 and all(item.status == "PENDING" for item in manual), "manual gates pending")

    print("Pre-merge gate self-test passed: automated pass/fail/fail-closed, build availability, and manual PENDING classes verified.")


def positive_int(value: str) -> int:
    parsed = int(value)
    if not 1 <= parsed <= MAX_TIMEOUT_SECONDS:
        raise argparse.ArgumentTypeError(
            f"timeout must be between 1 and {MAX_TIMEOUT_SECONDS} seconds"
        )
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--x64-build-dir",
        type=Path,
        help="optional configured x64 CMake build directory; availability only, not executed",
    )
    parser.add_argument(
        "--x86-build-dir",
        type=Path,
        help="optional configured x86/Win32 CMake build directory; availability only, not executed",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=positive_int,
        default=DEFAULT_TIMEOUT_SECONDS,
        help=f"per-validator timeout, 1..{MAX_TIMEOUT_SECONDS} (default: {DEFAULT_TIMEOUT_SECONDS})",
    )
    parser.add_argument("--json", action="store_true", help="emit deterministic JSON instead of text")
    parser.add_argument("--self-test", action="store_true", help="run bounded orchestration self-test only")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.self_test:
        try:
            self_test()
        except (AssertionError, OSError, subprocess.SubprocessError) as exc:
            print(f"Pre-merge gate self-test failed: {exc}", file=sys.stderr)
            return 1
        return 0

    automated = run_automated_validators(ROOT, AUTOMATED_VALIDATORS, args.timeout_seconds)
    availability = [
        inspect_build_tree("x64", args.x64_build_dir),
        inspect_build_tree("x86", args.x86_build_dir),
    ]
    manual = manual_gate_results()

    if args.json:
        print(render_json(automated, availability, manual))
    else:
        print(render_text(automated, availability, manual))
    return gate_exit_code(automated)


if __name__ == "__main__":
    raise SystemExit(main())
