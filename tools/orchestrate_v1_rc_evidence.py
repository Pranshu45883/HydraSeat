#!/usr/bin/env python3
"""Deterministic HydraSeat v1 release-candidate evidence orchestration.

This tool assembles already-produced, exact-RC-bound evidence. It never runs a
physical/game/install/signing acceptance scenario and never upgrades one evidence
class into another. A structurally valid pack may remain incomplete.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import validate_v1_acceptance_campaign as campaign_validator


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CAMPAIGN_ROOT = REPO_ROOT / "out" / "v1-acceptance"
CAMPAIGN_FILE_NAME = "v1-acceptance-campaign.json"
INDEX_FILE_NAME = "rc-evidence-index.json"
SUMMARY_FILE_NAME = "rc-evidence-summary.txt"
PACK_FILE_NAME = "rc-evidence-pack.zip"
RECEIPT_SCHEMA_VERSION = 1
INDEX_SCHEMA_VERSION = 1
PACK_SCHEMA_VERSION = 1
MAX_RECEIPT_BYTES = 64 * 1024
MAX_ARTIFACT_BYTES = campaign_validator.MAX_BYTES
MAX_PROFILE_BYTES = 16 * 1024 * 1024
MAX_INSTALL_STATE_BYTES = 64 * 1024
MAX_RELEASE_ARTIFACT_BYTES = 8 * 1024 * 1024 * 1024
MAX_PACK_BYTES = 32 * 1024 * 1024
REPARSE_POINT_ATTRIBUTE = 0x400
PORTABLE_LEAF = re.compile(r"^[A-Za-z0-9._+@-]{1,128}$")
WINDOWS_RESERVED = {
    "CON", "PRN", "AUX", "NUL",
    *(f"COM{index}" for index in range(1, 10)),
    *(f"LPT{index}" for index in range(1, 10)),
}
SENSITIVE_KEYS = {
    "password", "passwd", "token", "access_token", "refresh_token", "cookie", "cookies",
    "secret", "client_secret", "private_key", "accountref", "account_ref", "accountid",
    "account_id", "username", "user_name", "email", "email_address",
    "machine_name", "computer_name", "hostname", "device_serial", "serial_number",
    "command_line", "cmdline", "argv", "home_directory", "user_profile", "userprofile",
}
PRIVATE_PATH = re.compile(
    r"(?i)(?:[A-Z]:[\\/]Users[\\/]|/home/|/Users/|\\\\[^\\/\s]+[\\/][^\\/\s]+)"
)
MACHINE_NAME = re.compile(r"(?i)\b(?:DESKTOP|LAPTOP)-[A-Z0-9-]{3,}\b")
EMAIL_ADDRESS = re.compile(r"(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b")
WINDOWS_SID = re.compile(r"\bS-1-5-(?:18|19|20|21(?:-\d+){4})\b", re.IGNORECASE)
SECRET_MARKER = re.compile(r"(?i)(?:-----BEGIN [A-Z ]*PRIVATE KEY-----|\bBearer\s+[A-Za-z0-9._~+/-]{8,})")


@dataclass(frozen=True)
class ExpectedGate:
    gate_id: str
    evidence_class: str
    origin: str
    manual_required: bool
    campaign_stage: str | None


EXPECTED_GATES: tuple[ExpectedGate, ...] = (
    ExpectedGate("Preflight", "Controlled", "ControlledProcess", False, "Preflight"),
    ExpectedGate("Phase3Physical", "Physical", "Physical", True, "Phase3Physical"),
    ExpectedGate("DisplayReconnect", "Physical", "Physical", True, "DisplayReconnect"),
    ExpectedGate("DifferentGames", "RealGame", "Physical", True, "DifferentGames"),
    ExpectedGate("SameTitle", "RealGame", "Physical", True, "SameTitle"),
    ExpectedGate("SeatIndependence", "RealGame", "Physical", True, "SeatIndependence"),
    ExpectedGate("ReturnToWindows", "RealGame", "Physical", True, "ReturnToWindows"),
    ExpectedGate("InstallRepairUninstall", "CleanMachineInstall", "Physical", True,
                 "InstallRepairUninstall"),
    ExpectedGate("RebootStartup", "CleanMachineInstall", "Physical", True, "RebootStartup"),
    ExpectedGate("UpdateRollback", "CleanMachineInstall", "Physical", True, "UpdateRollback"),
    ExpectedGate("Offline", "Controlled", "ControlledProcess", False, "Offline"),
    ExpectedGate("FaultRecovery", "Manual", "Physical", True, "FaultRecovery"),
    ExpectedGate("PerformanceSoak", "RealGame", "Physical", True, "PerformanceSoak"),
    # Signing/deployment is deliberately pack-level. The current campaign schema has
    # no signing stage, and this tool must not silently let another stage satisfy it.
    ExpectedGate("SigningDeployment", "SigningDeployment", "Physical", True, None),
)
GATE_BY_ID = {gate.gate_id: gate for gate in EXPECTED_GATES}
GATE_ORDER = {gate.gate_id: index for index, gate in enumerate(EXPECTED_GATES)}
VALID_CURRENT_STATUSES = {"PASS", "FAIL", "PENDING", "MISSING"}
INVALID_CURRENT_STATUSES = {"STALE", "FOREIGN", "TAMPERED", "MISSING_FILE", "INVALID"}

RECEIPT_KEYS = {
    "schema_version", "campaign_schema_version", "campaign_id", "session_run_id", "gate_id",
    "evidence_id", "test_name", "origin", "evidence_class", "created_unix",
    "artifact_name", "artifact_sha256", "artifact_bytes", "rc_commit_sha",
    "release_artifact_sha256", "release_artifact_name", "release_revision", "architecture",
    "profile_sha256", "install_state_sha256", "windows_build", "topology_fingerprint_sha256",
    "scenario_identity", "automated_passed", "human_verdict", "note",
}
INDEX_KEYS = {
    "schema_version", "campaign_schema_version", "campaign_identity", "identity_sha256",
    "expected", "evidence",
}
INDEX_EVIDENCE_KEYS = {
    "gate_id", "evidence_id", "test_name", "receipt_file", "receipt_sha256",
    "artifact_name", "artifact_sha256",
}
GENERAL_BINDING_KEYS = {
    "schema_version", "campaign_schema_version", "campaign_id", "session_run_id", "stage",
    "test_name", "origin", "evidence_class", "created_unix", "rc_commit_sha",
    "release_artifact_sha256", "release_artifact_name", "release_revision", "architecture",
    "profile_sha256", "install_state_sha256", "windows_build", "topology_fingerprint_sha256",
    "scenario_identity", "automated_passed",
}


class RcEvidenceError(ValueError):
    """Fail-closed RC orchestration error."""


class RcEvidenceIncomplete(RcEvidenceError):
    """Structurally valid evidence set that does not satisfy every required RC gate."""

    def __init__(self, report: "AssessmentReport"):
        super().__init__("required RC evidence is incomplete")
        self.report = report


@dataclass(frozen=True)
class GateAssessment:
    gate: ExpectedGate
    status: str
    evidence_id: str = ""
    detail: str = ""


@dataclass
class SessionState:
    root: Path
    session: Path
    campaign: dict
    index: dict


@dataclass
class AssessmentReport:
    state: SessionState
    gates: list[GateAssessment]

    @property
    def complete(self) -> bool:
        return all(item.status == "PASS" for item in self.gates)

    @property
    def has_invalid_evidence(self) -> bool:
        return any(item.status in INVALID_CURRENT_STATUSES for item in self.gates)



def _raise_constant(value: str):
    raise RcEvidenceError(f"non-finite JSON constant is forbidden: {value}")


def _canonical_json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def _json_bytes(data: bytes, label: str, maximum: int) -> dict:
    if not data or len(data) > maximum:
        raise RcEvidenceError(f"{label} is empty or exceeds its bounded size")
    try:
        value = json.loads(
            data.decode("utf-8", errors="strict"),
            object_pairs_hook=campaign_validator.no_duplicates,
            parse_constant=_raise_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, campaign_validator.ValidationError) as exc:
        raise RcEvidenceError(f"{label} is not strict UTF-8 JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise RcEvidenceError(f"{label} root must be an object")
    return value


def _exact_keys(value: dict, expected: set[str], label: str) -> None:
    if set(value) != expected:
        raise RcEvidenceError(f"{label} has unknown or missing fields")


def _is_reparse(path: Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return path.is_symlink() or bool(getattr(info, "st_file_attributes", 0) & REPARSE_POINT_ATTRIBUTE)


def _assert_plain_directory(path: Path, label: str, create: bool = False) -> None:
    if create:
        path.mkdir(parents=True, exist_ok=True)
    if not path.is_dir() or _is_reparse(path):
        raise RcEvidenceError(f"{label} must be a non-reparse directory")


def _assert_regular_file(path: Path, label: str, maximum: int, allow_empty: bool = False) -> int:
    if _is_reparse(path) or not path.is_file():
        raise RcEvidenceError(f"{label} must be a non-reparse regular file")
    size = path.stat().st_size
    if size < (0 if allow_empty else 1) or size > maximum:
        raise RcEvidenceError(f"{label} size is outside its bounded range")
    return size


def _read_bounded(path: Path, label: str, maximum: int) -> bytes:
    size = _assert_regular_file(path, label, maximum)
    data = path.read_bytes()
    if len(data) != size:
        raise RcEvidenceError(f"{label} changed during bounded read")
    return data


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path, label: str, maximum: int) -> str:
    size = _assert_regular_file(path, label, maximum)
    digest = hashlib.sha256()
    observed = 0
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            observed += len(chunk)
            if observed > maximum:
                raise RcEvidenceError(f"{label} grew beyond its bound during hashing")
            digest.update(chunk)
    if observed != size:
        raise RcEvidenceError(f"{label} changed during hashing")
    return digest.hexdigest()


def _atomic_write(path: Path, data: bytes) -> None:
    _assert_plain_directory(path.parent, f"parent of {path.name}", create=True)
    temporary = Path(str(path) + ".new")
    if temporary.exists() or _is_reparse(temporary):
        if _is_reparse(temporary) or not temporary.is_file():
            raise RcEvidenceError(f"stale staging path for {path.name} is unsafe")
        temporary.unlink()
    with temporary.open("wb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
    if temporary.exists() or _is_reparse(temporary):
        raise RcEvidenceError(f"staging cleanup for {path.name} could not be verified")


def _portable_leaf(value: str, label: str) -> str:
    if not isinstance(value, str) or not PORTABLE_LEAF.fullmatch(value):
        raise RcEvidenceError(f"{label} must be a bounded portable leaf name")
    if Path(value).name != value or value in {".", ".."} or value.endswith((".", " ")):
        raise RcEvidenceError(f"{label} must not contain path traversal")
    stem = value.split(".", 1)[0].upper()
    if stem in WINDOWS_RESERVED:
        raise RcEvidenceError(f"{label} uses a reserved Windows device name")
    return value


def _token(value: object, label: str) -> str:
    if not isinstance(value, str) or not campaign_validator.TOKEN.fullmatch(value):
        raise RcEvidenceError(f"{label} is not a bounded identifier")
    return value


def _hex(value: object, length: int, label: str) -> str:
    pattern = campaign_validator.HEX40 if length == 40 else campaign_validator.HEX64
    if not isinstance(value, str) or not pattern.fullmatch(value):
        raise RcEvidenceError(f"{label} must be exactly {length} lowercase hexadecimal characters")
    return value


def _integer(value: object, label: str, minimum: int = 0, maximum: int = (1 << 64) - 1) -> int:
    try:
        return campaign_validator.integer(value, label, minimum, maximum)
    except campaign_validator.ValidationError as exc:
        raise RcEvidenceError(str(exc)) from exc


def _privacy_scan(value: object, label: str) -> None:
    def visit(node: object, path: str) -> None:
        if isinstance(node, dict):
            for key, child in node.items():
                normalized = str(key).lower()
                if normalized in SENSITIVE_KEYS:
                    raise RcEvidenceError(f"{label} contains prohibited private field: {path}{key}")
                visit(child, f"{path}{key}.")
        elif isinstance(node, list):
            for index, child in enumerate(node):
                visit(child, f"{path}{index}.")
        elif isinstance(node, str):
            if PRIVATE_PATH.search(node) or MACHINE_NAME.search(node) or EMAIL_ADDRESS.search(node) or \
               WINDOWS_SID.search(node) or SECRET_MARKER.search(node):
                raise RcEvidenceError(
                    f"{label} contains prohibited private path/account/machine/secret text")

    visit(value, "")


def _expected_json() -> list[dict]:
    return [
        {
            "gate_id": gate.gate_id,
            "evidence_class": gate.evidence_class,
            "origin": gate.origin,
            "manual_required": gate.manual_required,
            "campaign_stage": gate.campaign_stage,
        }
        for gate in EXPECTED_GATES
    ]


def _identity_sha(identity: dict) -> str:
    return _sha256(_canonical_json_bytes(identity))


def _campaign_skeleton(identity: dict, created_unix: int) -> dict:
    return {
        "schema_version": 2,
        "identity": identity,
        "created_unix": created_unix,
        "updated_unix": created_unix,
        "stages": [
            {
                "stage": stage,
                "state": "Pending",
                "attempt": 0,
                "started_unix": 0,
                "completed_unix": 0,
                "evidence": [],
                "diagnostic": "",
            }
            for stage in campaign_validator.STAGES
        ],
    }


def _index_skeleton(identity: dict) -> dict:
    return {
        "schema_version": INDEX_SCHEMA_VERSION,
        "campaign_schema_version": 2,
        "campaign_identity": identity,
        "identity_sha256": _identity_sha(identity),
        "expected": _expected_json(),
        "evidence": [],
    }


def _session_path(root: Path, campaign_id: str, create: bool = False) -> Path:
    _portable_leaf(campaign_id, "campaign id")
    _assert_plain_directory(root, "RC campaign root", create=True)
    session = root / campaign_id
    if create:
        _assert_plain_directory(session, "RC session", create=True)
    else:
        _assert_plain_directory(session, "RC session")
    return session


def _load_install_state(path: Path) -> tuple[dict, bytes]:
    data = _read_bounded(path, "install-state input", MAX_INSTALL_STATE_BYTES)
    return _json_bytes(data, "install-state input", MAX_INSTALL_STATE_BYTES), data


def initialize_session(
    root: Path,
    campaign_id: str,
    rc_commit_sha: str,
    release_artifact: Path,
    release_revision: int,
    architecture: str,
    profile_file: Path,
    install_state: Path,
    windows_build: str,
    topology_sha256: str,
    scenario_id: str,
    created_unix: int,
) -> SessionState:
    _portable_leaf(campaign_id, "campaign id")
    _hex(rc_commit_sha, 40, "RC commit")
    if architecture not in {"x64", "x86", "arm64"}:
        raise RcEvidenceError("architecture must be x64, x86, or arm64")
    _integer(release_revision, "release revision", 1)
    _token(windows_build, "Windows build")
    _hex(topology_sha256, 64, "topology fingerprint")
    _token(scenario_id, "scenario identity")
    _integer(created_unix, "created_unix", 1)

    release_name = _portable_leaf(release_artifact.name, "release artifact name")
    release_sha = _sha256_file(release_artifact, "release artifact", MAX_RELEASE_ARTIFACT_BYTES)
    profile_sha = _sha256_file(profile_file, "profile/release-scope input", MAX_PROFILE_BYTES)
    install_doc, install_bytes = _load_install_state(install_state)
    install_sha = _sha256(install_bytes)
    if install_doc.get("commitSha") != rc_commit_sha or \
       install_doc.get("releaseRevision") != release_revision or \
       install_doc.get("architecture") != architecture:
        raise RcEvidenceError("install-state input does not match exact RC commit/revision/architecture")

    identity = {
        "campaign_id": campaign_id,
        "rc_commit_sha": rc_commit_sha,
        "release_artifact_sha256": release_sha,
        "release_artifact_name": release_name,
        "release_revision": release_revision,
        "architecture": architecture,
        "profile_sha256": profile_sha,
        "install_state_sha256": install_sha,
        "windows_build": windows_build,
        "topology_fingerprint_sha256": topology_sha256,
        "seat_ids": [1, 2],
        "scenario_identity": scenario_id,
        "session_run_id": campaign_id,
    }
    campaign = _campaign_skeleton(identity, created_unix)
    try:
        campaign_validator.validate_document(campaign, created_unix)
    except campaign_validator.ValidationError as exc:
        raise RcEvidenceError(f"campaign initialization failed strict validation: {exc}") from exc

    session = _session_path(root, campaign_id, create=True)
    campaign_path = session / CAMPAIGN_FILE_NAME
    index_path = session / INDEX_FILE_NAME
    if campaign_path.exists() or index_path.exists():
        raise RcEvidenceError("RC campaign already exists; use resume/summary/verify instead of reinitializing")
    for directory in (session / "incoming", session / "receipts", session / "artifacts"):
        _assert_plain_directory(directory, directory.name, create=True)
    index = _index_skeleton(identity)
    _atomic_write(campaign_path, _canonical_json_bytes(campaign))
    _atomic_write(index_path, _canonical_json_bytes(index))
    return SessionState(root=root, session=session, campaign=campaign, index=index)


def _validate_index(index: dict, identity: dict) -> None:
    _exact_keys(index, INDEX_KEYS, "RC evidence index")
    if index["schema_version"] != INDEX_SCHEMA_VERSION or index["campaign_schema_version"] != 2:
        raise RcEvidenceError("unsupported RC evidence index schema")
    if index["campaign_identity"] != identity or index["identity_sha256"] != _identity_sha(identity):
        raise RcEvidenceError("RC evidence index is bound to a different campaign identity")
    if index["expected"] != _expected_json():
        raise RcEvidenceError("RC evidence expected-scenario manifest is unknown or reordered")
    evidence = index["evidence"]
    if not isinstance(evidence, list) or len(evidence) > len(EXPECTED_GATES):
        raise RcEvidenceError("RC evidence index count exceeds expected gates")
    observed_gate: set[str] = set()
    observed_id: set[str] = set()
    observed_test: set[str] = set()
    observed_artifact: set[str] = set()
    previous_key: tuple[int, str] | None = None
    for item in evidence:
        if not isinstance(item, dict):
            raise RcEvidenceError("RC evidence index item must be an object")
        _exact_keys(item, INDEX_EVIDENCE_KEYS, "RC evidence index item")
        gate_id = _token(item["gate_id"], "index gate id")
        if gate_id not in GATE_BY_ID:
            raise RcEvidenceError("RC evidence index contains an unknown gate")
        evidence_id = _token(item["evidence_id"], "index evidence id")
        test_name = _token(item["test_name"], "index test name")
        receipt_file = _portable_leaf(item["receipt_file"], "index receipt file")
        artifact_name = _portable_leaf(item["artifact_name"], "index artifact name")
        receipt_sha = _hex(item["receipt_sha256"], 64, "index receipt hash")
        artifact_sha = _hex(item["artifact_sha256"], 64, "index artifact hash")
        if receipt_file != receipt_sha + ".json":
            raise RcEvidenceError("receipt storage name is not derived from exact receipt bytes")
        artifact_key = artifact_name.casefold()
        if gate_id in observed_gate or evidence_id in observed_id or test_name in observed_test or \
           artifact_key in observed_artifact:
            raise RcEvidenceError("RC evidence index contains a conflicting duplicate identity")
        observed_gate.add(gate_id)
        observed_id.add(evidence_id)
        observed_test.add(test_name)
        observed_artifact.add(artifact_key)
        key = (GATE_ORDER[gate_id], evidence_id)
        if previous_key is not None and key <= previous_key:
            raise RcEvidenceError("RC evidence index is not in deterministic gate/evidence order")
        previous_key = key


def _load_campaign_structure(path: Path) -> dict:
    data = _read_bounded(path, "acceptance campaign", campaign_validator.MAX_BYTES)
    campaign = _json_bytes(data, "acceptance campaign", campaign_validator.MAX_BYTES)
    evidence_times = [
        item.get("created_unix", 0)
        for record in campaign.get("stages", []) if isinstance(record, dict)
        for item in record.get("evidence", []) if isinstance(item, dict)
    ]
    structure_now = max(
        [campaign.get("created_unix", 1), campaign.get("updated_unix", 1), *evidence_times]
    )
    try:
        campaign_validator.validate_document(campaign, structure_now)
    except campaign_validator.ValidationError as exc:
        raise RcEvidenceError(f"acceptance campaign is structurally invalid: {exc}") from exc
    return campaign


def _cleanup_unreferenced_files(session: Path, index: dict) -> None:
    expected_receipts = {item["receipt_file"] for item in index["evidence"]}
    expected_artifacts = {item["artifact_name"] for item in index["evidence"]}
    for directory_name, expected in (("receipts", expected_receipts), ("artifacts", expected_artifacts)):
        directory = session / directory_name
        _assert_plain_directory(directory, directory_name, create=True)
        for child in directory.iterdir():
            if _is_reparse(child) or not child.is_file():
                raise RcEvidenceError(f"{directory_name} contains an unsafe non-regular entry")
            if child.name not in expected:
                child.unlink()
                if child.exists() or _is_reparse(child):
                    raise RcEvidenceError(f"unreferenced {directory_name} residue could not be removed")


def load_session(root: Path, campaign_id: str) -> SessionState:
    session = _session_path(root, campaign_id)
    campaign_path = session / CAMPAIGN_FILE_NAME
    index_path = session / INDEX_FILE_NAME
    for staged in (Path(str(campaign_path) + ".new"), Path(str(index_path) + ".new")):
        if staged.exists() or _is_reparse(staged):
            if _is_reparse(staged) or not staged.is_file():
                raise RcEvidenceError("stale orchestration staging path is unsafe")
            staged.unlink()
    campaign = _load_campaign_structure(campaign_path)
    if campaign["identity"]["campaign_id"] != campaign_id or \
       campaign["identity"]["session_run_id"] != campaign_id or session.name != campaign_id:
        raise RcEvidenceError("campaign/session directory identity mismatch")
    index_data = _read_bounded(index_path, "RC evidence index", campaign_validator.MAX_BYTES)
    index = _json_bytes(index_data, "RC evidence index", campaign_validator.MAX_BYTES)
    _validate_index(index, campaign["identity"])
    state = SessionState(root=root, session=session, campaign=campaign, index=index)
    _sync_campaign_evidence(state)
    _cleanup_unreferenced_files(session, state.index)
    _repair_interrupted_campaign_mirror(state)
    return state


def _gate_for_receipt(receipt: dict) -> ExpectedGate:
    gate_id = _token(receipt.get("gate_id"), "receipt gate id")
    gate = GATE_BY_ID.get(gate_id)
    if gate is None:
        raise RcEvidenceError("receipt references an unknown RC gate")
    return gate


def _validate_general_binding(binding: dict, receipt: dict, identity: dict, gate: ExpectedGate) -> None:
    _exact_keys(binding, GENERAL_BINDING_KEYS, "evidence release_binding")
    expected = {
        "schema_version": 2,
        "campaign_schema_version": 2,
        "campaign_id": identity["campaign_id"],
        "session_run_id": identity["session_run_id"],
        "stage": gate.gate_id,
        "test_name": receipt["test_name"],
        "origin": gate.origin,
        "evidence_class": gate.evidence_class,
        "created_unix": receipt["created_unix"],
        "rc_commit_sha": identity["rc_commit_sha"],
        "release_artifact_sha256": identity["release_artifact_sha256"],
        "release_artifact_name": identity["release_artifact_name"],
        "release_revision": identity["release_revision"],
        "architecture": identity["architecture"],
        "profile_sha256": identity["profile_sha256"],
        "install_state_sha256": identity["install_state_sha256"],
        "windows_build": identity["windows_build"],
        "topology_fingerprint_sha256": identity["topology_fingerprint_sha256"],
        "scenario_identity": identity["scenario_identity"],
        "automated_passed": receipt["automated_passed"],
    }
    if binding != expected:
        raise RcEvidenceError("evidence artifact release_binding is foreign or inconsistent with exact RC receipt")


def _validate_artifact_document(document: dict, receipt: dict, identity: dict,
                                gate: ExpectedGate, now: int, freshness: bool) -> None:
    _privacy_scan(document, "evidence artifact")
    if gate.gate_id == "Phase3Physical":
        try:
            if document["schema_version"] != 1 or document["state"] != "MANUAL_PASS" or \
               document["manual_verdict"] != "PASS" or \
               document["environment"]["architecture"] != identity["architecture"] or \
               document["environment"]["windows_build"] != identity["windows_build"] or \
               document["profile"]["sha256"] != identity["profile_sha256"] or \
               document["session_id"] != identity["session_run_id"] or \
               document["manual_verdict_unix"] != receipt["created_unix"]:
                raise RcEvidenceError("Phase 3 artifact is foreign, partial, or not an exact MANUAL_PASS receipt")
            valid_until = _integer(document["evidence_valid_until_unix"],
                                   "Phase 3 evidence_valid_until_unix", 1)
            if freshness and valid_until < now:
                raise RcEvidenceError("Phase 3 artifact is stale")
        except (KeyError, TypeError) as exc:
            raise RcEvidenceError("Phase 3 artifact is partial or malformed") from exc
        return

    binding = document.get("release_binding")
    if binding is None:
        if gate.gate_id != "Preflight":
            raise RcEvidenceError(f"{gate.gate_id} evidence artifact lacks exact release_binding")
        required = {
            "schema_version", "commit_sha", "release_revision", "architecture",
            "install_state_sha256", "all_owned_files_verified",
        }
        if not required.issubset(document) or document["schema_version"] != 1 or \
           document["commit_sha"] != identity["rc_commit_sha"] or \
           document["release_revision"] != identity["release_revision"] or \
           document["architecture"] != identity["architecture"] or \
           document["install_state_sha256"] != identity["install_state_sha256"] or \
           document["all_owned_files_verified"] is not True:
            raise RcEvidenceError("generated Preflight probe is foreign or not bound to the exact installed RC")
        return
    if not isinstance(binding, dict):
        raise RcEvidenceError("evidence release_binding must be an object")
    _validate_general_binding(binding, receipt, identity, gate)


def _validate_receipt(receipt: dict, identity: dict, artifact_bytes: bytes,
                      now: int, freshness: bool = True) -> ExpectedGate:
    _exact_keys(receipt, RECEIPT_KEYS, "RC evidence receipt")
    if receipt["schema_version"] != RECEIPT_SCHEMA_VERSION or receipt["campaign_schema_version"] != 2:
        raise RcEvidenceError("unsupported RC evidence receipt schema")
    gate = _gate_for_receipt(receipt)
    evidence_id = _token(receipt["evidence_id"], "receipt evidence id")
    test_name = _token(receipt["test_name"], "receipt test name")
    _ = evidence_id, test_name
    artifact_name = _portable_leaf(receipt["artifact_name"], "receipt artifact name")
    artifact_sha = _hex(receipt["artifact_sha256"], 64, "receipt artifact hash")
    artifact_size = _integer(receipt["artifact_bytes"], "receipt artifact bytes", 1, MAX_ARTIFACT_BYTES)
    if len(artifact_bytes) != artifact_size or _sha256(artifact_bytes) != artifact_sha:
        raise RcEvidenceError("evidence artifact hash/size mismatch; exact bytes changed")
    created = _integer(receipt["created_unix"], "receipt created_unix", 1)
    if freshness and (created > now + 300 or now - min(now, created) > campaign_validator.MAX_AGE):
        raise RcEvidenceError("receipt evidence is stale or implausibly in the future")
    if receipt["evidence_class"] != gate.evidence_class or receipt["origin"] != gate.origin:
        raise RcEvidenceError("receipt evidence class/origin cannot satisfy this independent RC gate")
    if receipt["human_verdict"] not in campaign_validator.VERDICTS:
        raise RcEvidenceError("receipt human verdict is invalid")
    if not gate.manual_required and receipt["human_verdict"] != "Pending":
        raise RcEvidenceError("automated Controlled gate cannot carry a fabricated manual verdict")
    if type(receipt["automated_passed"]) is not bool:
        raise RcEvidenceError("receipt automated_passed must be a JSON boolean")
    if receipt["automated_passed"] is False and receipt["human_verdict"] == "Pass":
        raise RcEvidenceError("manual Pass cannot override failed automated evidence")
    if not isinstance(receipt["note"], str) or len(receipt["note"].encode("utf-8")) > campaign_validator.MAX_NOTE:
        raise RcEvidenceError("receipt note is oversized")

    exact_identity_fields = (
        "campaign_id", "session_run_id", "rc_commit_sha", "release_artifact_sha256",
        "release_artifact_name", "release_revision", "architecture", "profile_sha256",
        "install_state_sha256", "windows_build", "topology_fingerprint_sha256",
        "scenario_identity",
    )
    for field in exact_identity_fields:
        if receipt[field] != identity[field]:
            raise RcEvidenceError(f"receipt {field} is foreign to this exact RC campaign")
    _hex(receipt["rc_commit_sha"], 40, "receipt RC commit")
    for field in ("release_artifact_sha256", "profile_sha256", "install_state_sha256",
                  "topology_fingerprint_sha256"):
        _hex(receipt[field], 64, f"receipt {field}")
    _integer(receipt["release_revision"], "receipt release revision", 1)
    if receipt["architecture"] not in {"x64", "x86", "arm64"}:
        raise RcEvidenceError("receipt architecture is invalid")
    for field in ("campaign_id", "session_run_id", "release_artifact_name", "windows_build",
                  "scenario_identity"):
        _token(receipt[field], f"receipt {field}")
    _privacy_scan(receipt, "RC evidence receipt")

    artifact = _json_bytes(artifact_bytes, "evidence artifact", MAX_ARTIFACT_BYTES)
    _validate_artifact_document(artifact, receipt, identity, gate, now, freshness)
    _ = artifact_name
    return gate


def _receipt_to_campaign_item(receipt: dict) -> dict:
    return {
        "schema_version": 2,
        "evidence_id": receipt["evidence_id"],
        "stage": receipt["gate_id"],
        "origin": receipt["origin"],
        "created_unix": receipt["created_unix"],
        "content_sha256": receipt["artifact_sha256"],
        "evidence_artifact_name": receipt["artifact_name"],
        "test_name": receipt["test_name"],
        "rc_commit_sha": receipt["rc_commit_sha"],
        "release_artifact_sha256": receipt["release_artifact_sha256"],
        "release_revision": receipt["release_revision"],
        "architecture": receipt["architecture"],
        "profile_sha256": receipt["profile_sha256"],
        "install_state_sha256": receipt["install_state_sha256"],
        "scenario_identity": receipt["scenario_identity"],
        "automated_passed": receipt["automated_passed"],
        "human_verdict": receipt["human_verdict"],
        "note": receipt["note"],
        "campaign_schema_version": 2,
        "campaign_id": receipt["campaign_id"],
        "session_run_id": receipt["session_run_id"],
        "release_artifact_name": receipt["release_artifact_name"],
        "windows_build": receipt["windows_build"],
        "topology_fingerprint_sha256": receipt["topology_fingerprint_sha256"],
        "evidence_class": receipt["evidence_class"],
    }


def _campaign_item_to_receipt(item: dict, artifact_bytes: bytes) -> dict:
    return {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "campaign_schema_version": item["campaign_schema_version"],
        "campaign_id": item["campaign_id"],
        "session_run_id": item["session_run_id"],
        "gate_id": item["stage"],
        "evidence_id": item["evidence_id"],
        "test_name": item["test_name"],
        "origin": item["origin"],
        "evidence_class": item["evidence_class"],
        "created_unix": item["created_unix"],
        "artifact_name": item["evidence_artifact_name"],
        "artifact_sha256": item["content_sha256"],
        "artifact_bytes": len(artifact_bytes),
        "rc_commit_sha": item["rc_commit_sha"],
        "release_artifact_sha256": item["release_artifact_sha256"],
        "release_artifact_name": item["release_artifact_name"],
        "release_revision": item["release_revision"],
        "architecture": item["architecture"],
        "profile_sha256": item["profile_sha256"],
        "install_state_sha256": item["install_state_sha256"],
        "windows_build": item["windows_build"],
        "topology_fingerprint_sha256": item["topology_fingerprint_sha256"],
        "scenario_identity": item["scenario_identity"],
        "automated_passed": item["automated_passed"],
        "human_verdict": item["human_verdict"],
        "note": item["note"],
    }


def _stage_state(receipt: dict, gate: ExpectedGate) -> tuple[str, str]:
    if receipt["automated_passed"] is False or receipt["human_verdict"] == "Fail":
        return "Failed", "orchestrated evidence reports failure"
    if gate.manual_required and receipt["human_verdict"] == "Pending":
        return "AwaitingManualReview", "orchestrated evidence awaits manual review"
    if gate.manual_required and receipt["human_verdict"] != "Pass":
        return "Failed", "orchestrated evidence lacks final manual pass"
    return "Passed", ""


def _read_stored_receipt(state: SessionState, entry: dict, verify_hash: bool = True) -> tuple[dict, bytes]:
    path = state.session / "receipts" / entry["receipt_file"]
    data = _read_bounded(path, "stored evidence receipt", MAX_RECEIPT_BYTES)
    if verify_hash and _sha256(data) != entry["receipt_sha256"]:
        raise RcEvidenceError("stored evidence receipt hash mismatch; exact receipt bytes changed")
    receipt = _json_bytes(data, "stored evidence receipt", MAX_RECEIPT_BYTES)
    return receipt, data


def _sync_campaign_evidence(state: SessionState) -> None:
    existing_by_gate = {item["gate_id"]: item for item in state.index["evidence"]}
    candidate_entries = list(state.index["evidence"])
    changed = False
    for record in state.campaign["stages"]:
        if not record["evidence"]:
            continue
        if len(record["evidence"]) != 1:
            raise RcEvidenceError(
                f"campaign stage {record['stage']} has multiple evidence items; RC gate authority is ambiguous")
        gate = GATE_BY_ID[record["stage"]]
        item = record["evidence"][0]
        artifact_name = _portable_leaf(item["evidence_artifact_name"], "campaign evidence artifact name")
        existing = existing_by_gate.get(gate.gate_id)
        if existing is not None:
            # Do not touch the artifact here. Already-indexed bytes are assessed later so
            # summary can report MISSING_FILE/TAMPERED/STALE instead of load aborting.
            stored_receipt, _ = _read_stored_receipt(state, existing)
            if _receipt_to_campaign_item(stored_receipt) != item or \
               existing["evidence_id"] != item["evidence_id"] or \
               existing["test_name"] != item["test_name"] or \
               existing["artifact_name"] != artifact_name or \
               existing["artifact_sha256"] != item["content_sha256"]:
                raise RcEvidenceError(
                    f"campaign evidence for {gate.gate_id} conflicts with the indexed exact receipt")
            continue

        artifact_path = state.session / "artifacts" / artifact_name
        artifact_bytes = _read_bounded(
            artifact_path, "campaign-attached evidence artifact", MAX_ARTIFACT_BYTES)
        if _sha256(artifact_bytes) != item["content_sha256"]:
            raise RcEvidenceError("campaign-attached evidence artifact hash mismatch")
        receipt = _campaign_item_to_receipt(item, artifact_bytes)
        structural_now = max(state.campaign["created_unix"], receipt["created_unix"])
        _validate_receipt(
            receipt, state.campaign["identity"], artifact_bytes,
            structural_now, freshness=False)
        receipt_bytes = _canonical_json_bytes(receipt)
        candidate = _entry_for_receipt(receipt, receipt_bytes)
        receipt_destination = state.session / "receipts" / candidate["receipt_file"]
        if receipt_destination.exists() or _is_reparse(receipt_destination):
            if _is_reparse(receipt_destination) or not receipt_destination.is_file() or \
               _read_bounded(receipt_destination, "campaign-derived receipt", MAX_RECEIPT_BYTES) != receipt_bytes:
                raise RcEvidenceError("campaign-derived receipt destination conflicts with existing bytes")
        else:
            _atomic_write(receipt_destination, receipt_bytes)
        candidate_entries.append(candidate)
        existing_by_gate[gate.gate_id] = candidate
        changed = True
    if changed:
        updated = dict(state.index)
        updated["evidence"] = _sort_index_evidence(candidate_entries)
        _validate_index(updated, state.campaign["identity"])
        _atomic_write(state.session / INDEX_FILE_NAME, _canonical_json_bytes(updated))
        state.index = updated


def _rebuild_campaign(state: SessionState, receipts: dict[str, dict]) -> dict:
    campaign = _campaign_skeleton(state.campaign["identity"], state.campaign["created_unix"])
    latest = campaign["created_unix"]
    for record in campaign["stages"]:
        receipt = receipts.get(record["stage"])
        if receipt is None:
            continue
        gate = GATE_BY_ID[record["stage"]]
        state_name, diagnostic = _stage_state(receipt, gate)
        record.update({
            "state": state_name,
            "attempt": 1,
            "started_unix": receipt["created_unix"],
            "completed_unix": receipt["created_unix"],
            "evidence": [_receipt_to_campaign_item(receipt)],
            "diagnostic": diagnostic,
        })
        latest = max(latest, receipt["created_unix"])
    campaign["updated_unix"] = latest
    try:
        campaign_validator.validate_document(campaign, max(latest, campaign["created_unix"]))
    except campaign_validator.ValidationError as exc:
        raise RcEvidenceError(f"orchestrated campaign mirror is invalid: {exc}") from exc
    return campaign


def _campaign_evidence_ids(campaign: dict) -> set[str]:
    return {
        item["evidence_id"]
        for record in campaign["stages"]
        for item in record["evidence"]
    }


def _campaign_record_matches_receipt(record: dict, receipt: dict, gate: ExpectedGate) -> bool:
    expected_state, _ = _stage_state(receipt, gate)
    return record["state"] == expected_state and \
           len(record["evidence"]) == 1 and \
           record["evidence"][0] == _receipt_to_campaign_item(receipt)


def _repair_interrupted_campaign_mirror(state: SessionState) -> None:
    receipts: dict[str, dict] = {}
    for entry in state.index["evidence"]:
        gate = GATE_BY_ID[entry["gate_id"]]
        if gate.campaign_stage is None:
            continue
        try:
            receipt, _ = _read_stored_receipt(state, entry)
            artifact_path = state.session / "artifacts" / entry["artifact_name"]
            artifact_bytes = _read_bounded(
                artifact_path, "interrupted-commit evidence artifact", MAX_ARTIFACT_BYTES)
            if _sha256(artifact_bytes) != entry["artifact_sha256"]:
                return
            structural_now = max(
                state.campaign["created_unix"],
                receipt.get("created_unix", state.campaign["created_unix"]),
            )
            _validate_receipt(
                receipt, state.campaign["identity"], artifact_bytes,
                structural_now, freshness=False)
        except (RcEvidenceError, TypeError):
            return
        if receipt["evidence_id"] != entry["evidence_id"] or \
           receipt["test_name"] != entry["test_name"] or \
           receipt["artifact_name"] != entry["artifact_name"] or \
           receipt["artifact_sha256"] != entry["artifact_sha256"]:
            return
        receipts[gate.campaign_stage] = receipt
    derived = _rebuild_campaign(state, receipts)
    existing_ids = _campaign_evidence_ids(state.campaign)
    derived_ids = _campaign_evidence_ids(derived)
    if not existing_ids.issubset(derived_ids):
        raise RcEvidenceError("campaign contains evidence not owned by the RC orchestration index")

    candidate = json.loads(json.dumps(state.campaign))
    changed = False
    for index, record in enumerate(candidate["stages"]):
        receipt = receipts.get(record["stage"])
        if receipt is None:
            continue
        gate = GATE_BY_ID[record["stage"]]
        if record["evidence"]:
            if not _campaign_record_matches_receipt(record, receipt, gate):
                raise RcEvidenceError("campaign/index mirror conflict is not a recoverable interrupted commit")
            continue
        if record["state"] != "Pending" or record["attempt"] != 0 or \
           record["started_unix"] != 0 or record["completed_unix"] != 0:
            raise RcEvidenceError("indexed evidence cannot repair a non-pending campaign stage")
        candidate["stages"][index] = derived["stages"][index]
        changed = True

    if not changed:
        return
    candidate["updated_unix"] = max(candidate["updated_unix"], derived["updated_unix"])
    evidence_times = [
        item["created_unix"] for record in candidate["stages"] for item in record["evidence"]
    ]
    structural_now = max([candidate["updated_unix"], candidate["created_unix"], *evidence_times])
    try:
        campaign_validator.validate_document(candidate, structural_now)
    except campaign_validator.ValidationError as exc:
        raise RcEvidenceError(f"interrupted campaign mirror repair is invalid: {exc}") from exc
    _atomic_write(state.session / CAMPAIGN_FILE_NAME, _canonical_json_bytes(candidate))
    state.campaign = candidate


def _entry_for_receipt(receipt: dict, receipt_bytes: bytes) -> dict:
    receipt_sha = _sha256(receipt_bytes)
    return {
        "gate_id": receipt["gate_id"],
        "evidence_id": receipt["evidence_id"],
        "test_name": receipt["test_name"],
        "receipt_file": receipt_sha + ".json",
        "receipt_sha256": receipt_sha,
        "artifact_name": receipt["artifact_name"],
        "artifact_sha256": receipt["artifact_sha256"],
    }


def _sort_index_evidence(items: list[dict]) -> list[dict]:
    return sorted(items, key=lambda item: (GATE_ORDER[item["gate_id"]], item["evidence_id"]))


def _classify_error(error: RcEvidenceError) -> str:
    text = str(error).lower()
    if "stale" in text or "future" in text or "expired" in text:
        return "STALE"
    if "foreign" in text or "different campaign" in text or "identity mismatch" in text:
        return "FOREIGN"
    if "hash" in text or "exact bytes changed" in text or "tamper" in text:
        return "TAMPERED"
    if "missing" in text or "not a non-reparse regular file" in text or \
       "must be a non-reparse regular file" in text:
        return "MISSING_FILE"
    return "INVALID"


def _validate_campaign_mirror(state: SessionState, receipt: dict, gate: ExpectedGate) -> None:
    if gate.campaign_stage is None:
        return
    record = state.campaign["stages"][campaign_validator.STAGES.index(gate.campaign_stage)]
    if not _campaign_record_matches_receipt(record, receipt, gate):
        raise RcEvidenceError("campaign mirror does not match the exact accepted evidence receipt")


def assess_session(root: Path, campaign_id: str, now: int) -> AssessmentReport:
    state = load_session(root, campaign_id)
    entries = {item["gate_id"]: item for item in state.index["evidence"]}
    assessments: list[GateAssessment] = []
    for gate in EXPECTED_GATES:
        entry = entries.get(gate.gate_id)
        if entry is None:
            assessments.append(GateAssessment(gate, "MISSING", detail="required evidence not ingested"))
            continue
        try:
            receipt, _ = _read_stored_receipt(state, entry)
            artifact_path = state.session / "artifacts" / entry["artifact_name"]
            artifact_bytes = _read_bounded(artifact_path, "stored evidence artifact", MAX_ARTIFACT_BYTES)
            if _sha256(artifact_bytes) != entry["artifact_sha256"]:
                raise RcEvidenceError("stored evidence artifact hash mismatch; exact bytes changed")
            _validate_receipt(receipt, state.campaign["identity"], artifact_bytes, now, freshness=True)
            if receipt["evidence_id"] != entry["evidence_id"] or \
               receipt["test_name"] != entry["test_name"] or \
               receipt["artifact_name"] != entry["artifact_name"] or \
               receipt["artifact_sha256"] != entry["artifact_sha256"]:
                raise RcEvidenceError("stored receipt metadata is foreign to the orchestration index")
            _validate_campaign_mirror(state, receipt, gate)
            if receipt["automated_passed"] is False or receipt["human_verdict"] == "Fail":
                status = "FAIL"
            elif gate.manual_required and receipt["human_verdict"] == "Pending":
                status = "PENDING"
            elif gate.manual_required and receipt["human_verdict"] != "Pass":
                status = "FAIL"
            else:
                status = "PASS"
            assessments.append(GateAssessment(gate, status, receipt["evidence_id"], "validated exact receipt/artifact"))
        except RcEvidenceError as exc:
            assessments.append(GateAssessment(gate, _classify_error(exc), entry["evidence_id"], str(exc)))
    return AssessmentReport(state=state, gates=assessments)


def add_evidence(root: Path, campaign_id: str, receipt_leaf: str, now: int) -> bool:
    receipt_leaf = _portable_leaf(receipt_leaf, "incoming receipt name")
    report = assess_session(root, campaign_id, now)
    if report.has_invalid_evidence:
        raise RcEvidenceError("existing accepted evidence is invalid/stale/tampered; repair it before ingesting more")
    state = report.state
    receipt_path = state.session / "incoming" / receipt_leaf
    receipt_bytes = _read_bounded(receipt_path, "incoming evidence receipt", MAX_RECEIPT_BYTES)
    receipt = _json_bytes(receipt_bytes, "incoming evidence receipt", MAX_RECEIPT_BYTES)
    gate = _gate_for_receipt(receipt)
    artifact_name = _portable_leaf(receipt.get("artifact_name"), "incoming artifact name")
    artifact_path = state.session / "incoming" / artifact_name
    artifact_bytes = _read_bounded(artifact_path, "incoming evidence artifact", MAX_ARTIFACT_BYTES)
    _validate_receipt(receipt, state.campaign["identity"], artifact_bytes, now, freshness=True)
    new_entry = _entry_for_receipt(receipt, receipt_bytes)

    for existing in state.index["evidence"]:
        same_identity = existing["gate_id"] == new_entry["gate_id"] or \
                        existing["evidence_id"] == new_entry["evidence_id"] or \
                        existing["test_name"] == new_entry["test_name"] or \
                        existing["artifact_name"] == new_entry["artifact_name"]
        if not same_identity:
            continue
        if existing == new_entry:
            stored_receipt = state.session / "receipts" / existing["receipt_file"]
            stored_artifact = state.session / "artifacts" / existing["artifact_name"]
            if _read_bounded(stored_receipt, "stored duplicate receipt", MAX_RECEIPT_BYTES) == receipt_bytes and \
               _read_bounded(stored_artifact, "stored duplicate artifact", MAX_ARTIFACT_BYTES) == artifact_bytes:
                return False
        raise RcEvidenceError("conflicting duplicate gate/evidence/test/artifact identity is rejected")

    receipt_destination = state.session / "receipts" / new_entry["receipt_file"]
    artifact_destination = state.session / "artifacts" / new_entry["artifact_name"]
    if receipt_destination.exists() or artifact_destination.exists() or \
       _is_reparse(receipt_destination) or _is_reparse(artifact_destination):
        raise RcEvidenceError("accepted evidence destination already exists outside the orchestration index")
    _atomic_write(receipt_destination, receipt_bytes)
    _atomic_write(artifact_destination, artifact_bytes)

    new_index = dict(state.index)
    new_index["evidence"] = _sort_index_evidence([*state.index["evidence"], new_entry])
    _validate_index(new_index, state.campaign["identity"])
    # Index is the orchestration commit point. If the process stops after this write,
    # load_session() safely recognizes an old campaign as a subset and rebuilds it.
    _atomic_write(state.session / INDEX_FILE_NAME, _canonical_json_bytes(new_index))
    state.index = new_index
    receipts: dict[str, dict] = {}
    for entry in new_index["evidence"]:
        expected = GATE_BY_ID[entry["gate_id"]]
        if expected.campaign_stage is None:
            continue
        stored, _ = _read_stored_receipt(state, entry)
        receipts[expected.campaign_stage] = stored
    campaign = _rebuild_campaign(state, receipts)
    _atomic_write(state.session / CAMPAIGN_FILE_NAME, _canonical_json_bytes(campaign))
    return True


def _summary_text(report: AssessmentReport) -> str:
    identity = report.state.campaign["identity"]
    lines = [
        f"RC identity: {identity['campaign_id']}",
        f"Commit: {identity['rc_commit_sha']}",
        f"Artifact: {identity['release_artifact_name']} {identity['release_artifact_sha256']}",
        f"Revision: {identity['release_revision']}",
        f"Architecture: {identity['architecture']}",
        f"Profile SHA-256: {identity['profile_sha256']}",
        f"Install-state SHA-256: {identity['install_state_sha256']}",
        f"Session/run: {identity['session_run_id']}",
        f"Scenario: {identity['scenario_identity']}",
        f"Topology/input SHA-256: {identity['topology_fingerprint_sha256']}",
        "",
        "Pack structure: VALID",
        "Synthetic: non-qualifying supplemental class; never substitutes for a required gate.",
        "",
    ]
    for evidence_class in ("Controlled", "Physical", "Manual", "RealGame", "CleanMachineInstall",
                           "SigningDeployment"):
        lines.append(evidence_class + ":")
        for item in report.gates:
            if item.gate.evidence_class != evidence_class:
                continue
            suffix = f"  evidence={item.evidence_id}" if item.evidence_id else ""
            lines.append(f"  {item.status:<12} {item.gate.gate_id}{suffix}")
        lines.append("")
    if report.complete:
        lines.extend([
            "RC EVIDENCE RESULT: COMPLETE",
            "Release decision: this tool does not declare the release READY; control-tower/legal/deployment approval remains separate.",
        ])
    else:
        lines.extend([
            "RC RESULT: NOT READY / INCOMPLETE",
            "Missing, pending, failed, stale, foreign, or invalid evidence is never reduced to PASS.",
        ])
    return "\n".join(lines) + "\n"


def summarize_session(root: Path, campaign_id: str, now: int, write_file: bool = True) -> AssessmentReport:
    report = assess_session(root, campaign_id, now)
    text = _summary_text(report)
    if write_file:
        _atomic_write(report.state.session / SUMMARY_FILE_NAME, text.encode("utf-8"))
    print(text, end="")
    return report


def _packable_session(root: Path, campaign_id: str, now: int) -> AssessmentReport:
    report = assess_session(root, campaign_id, now)
    if report.has_invalid_evidence:
        invalid = ", ".join(item.gate.gate_id + "=" + item.status
                            for item in report.gates if item.status in INVALID_CURRENT_STATUSES)
        raise RcEvidenceError(f"accepted evidence failed revalidation: {invalid}")
    return report


def strict_verify_session(root: Path, campaign_id: str, now: int) -> AssessmentReport:
    report = _packable_session(root, campaign_id, now)
    if not report.complete:
        raise RcEvidenceIncomplete(report)
    return report


def _zip_entry(name: str, data: bytes) -> tuple[zipfile.ZipInfo, bytes]:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    info.flag_bits = 0
    return info, data


def _safe_pack_name(name: str) -> None:
    if not name or "\\" in name or name.startswith("/") or re.match(r"^[A-Za-z]:", name):
        raise RcEvidenceError("pack entry has an absolute or platform-specific path")
    parts = name.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise RcEvidenceError("pack entry contains path traversal")


def _pack_manifest(report: AssessmentReport, files: list[dict]) -> dict:
    gates = []
    entry_by_gate = {item["gate_id"]: item for item in report.state.index["evidence"]}
    assessment_by_gate = {item.gate.gate_id: item for item in report.gates}
    for gate in EXPECTED_GATES:
        assessment = assessment_by_gate[gate.gate_id]
        record = {
            "gate_id": gate.gate_id,
            "evidence_class": gate.evidence_class,
            "origin": gate.origin,
            "manual_required": gate.manual_required,
            "campaign_stage": gate.campaign_stage,
            "status": assessment.status,
        }
        entry = entry_by_gate.get(gate.gate_id)
        if entry is not None:
            record["evidence_id"] = entry["evidence_id"]
            record["receipt_sha256"] = entry["receipt_sha256"]
            record["artifact_sha256"] = entry["artifact_sha256"]
        gates.append(record)
    return {
        "schema_version": PACK_SCHEMA_VERSION,
        "campaign_schema_version": 2,
        "campaign_identity": report.state.campaign["identity"],
        "identity_sha256": _identity_sha(report.state.campaign["identity"]),
        "structure": "VALID",
        "evidence_qualification": "COMPLETE" if report.complete else "INCOMPLETE",
        "gates": gates,
        "files": files,
    }


def pack_session(root: Path, campaign_id: str, now: int) -> Path:
    report = _packable_session(root, campaign_id, now)
    # FAIL/PENDING/MISSING are valid evidence-pack states. Stale/foreign/tampered
    # evidence is not packable even though summary can still report it.
    summary = _summary_text(report).encode("utf-8")
    state = report.state
    _privacy_scan(state.campaign, "acceptance campaign metadata")
    _privacy_scan(state.index, "RC evidence index metadata")
    content: dict[str, bytes] = {
        f"campaign/{CAMPAIGN_FILE_NAME}": _canonical_json_bytes(state.campaign),
        f"orchestration/{INDEX_FILE_NAME}": _canonical_json_bytes(state.index),
        f"orchestration/{SUMMARY_FILE_NAME}": summary,
    }
    for entry in state.index["evidence"]:
        receipt_path = state.session / "receipts" / entry["receipt_file"]
        artifact_path = state.session / "artifacts" / entry["artifact_name"]
        content[f"receipts/{entry['receipt_file']}"] = _read_bounded(
            receipt_path, "pack receipt", MAX_RECEIPT_BYTES)
        content[f"artifacts/{entry['artifact_name']}"] = _read_bounded(
            artifact_path, "pack artifact", MAX_ARTIFACT_BYTES)
    for name in content:
        _safe_pack_name(name)
    files = [
        {"path": name, "sha256": _sha256(content[name]), "bytes": len(content[name])}
        for name in sorted(content)
    ]
    manifest = _pack_manifest(report, files)
    content["manifest.json"] = _canonical_json_bytes(manifest)

    destination = state.session / PACK_FILE_NAME
    temporary = Path(str(destination) + ".new")
    if temporary.exists() or _is_reparse(temporary):
        if _is_reparse(temporary) or not temporary.is_file():
            raise RcEvidenceError("stale pack staging path is unsafe")
        temporary.unlink()
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED, allowZip64=False) as archive:
        for name in sorted(content):
            info, data = _zip_entry(name, content[name])
            archive.writestr(info, data)
    if temporary.stat().st_size > MAX_PACK_BYTES:
        temporary.unlink(missing_ok=True)
        raise RcEvidenceError("RC evidence pack exceeds its bounded size")
    os.replace(temporary, destination)
    _atomic_write(state.session / SUMMARY_FILE_NAME, summary)
    return destination


def _read_pack_entries(pack_path: Path) -> dict[str, bytes]:
    _assert_regular_file(pack_path, "RC evidence pack", MAX_PACK_BYTES)
    content: dict[str, bytes] = {}
    with zipfile.ZipFile(pack_path, "r") as archive:
        infos = archive.infolist()
        names = [info.filename for info in infos]
        if len(names) != len(set(names)):
            raise RcEvidenceError("RC evidence pack contains duplicate entry names")
        total = 0
        for info in infos:
            _safe_pack_name(info.filename)
            unix_type = (info.external_attr >> 16) & 0o170000
            if info.is_dir() or info.compress_type != zipfile.ZIP_STORED or info.flag_bits & 0x1 or \
               (info.create_system == 3 and unix_type not in {0, 0o100000}):
                raise RcEvidenceError(
                    "RC evidence pack contains unsupported directory/compression/encryption/symlink entry")
            if info.file_size < 0 or info.file_size > MAX_ARTIFACT_BYTES:
                raise RcEvidenceError("RC evidence pack entry exceeds bounded size")
            total += info.file_size
            if total > MAX_PACK_BYTES:
                raise RcEvidenceError("RC evidence pack expanded size exceeds bound")
            data = archive.read(info)
            if len(data) != info.file_size:
                raise RcEvidenceError("RC evidence pack entry read was incomplete")
            content[info.filename] = data
    return content


def _assessment_from_pack_content(content: dict[str, bytes], now: int) -> AssessmentReport:
    manifest_bytes = content.get("manifest.json")
    campaign_bytes = content.get(f"campaign/{CAMPAIGN_FILE_NAME}")
    index_bytes = content.get(f"orchestration/{INDEX_FILE_NAME}")
    summary_bytes = content.get(f"orchestration/{SUMMARY_FILE_NAME}")
    if None in (manifest_bytes, campaign_bytes, index_bytes, summary_bytes):
        raise RcEvidenceError("RC evidence pack is missing mandatory metadata entries")
    manifest = _json_bytes(manifest_bytes, "pack manifest", campaign_validator.MAX_BYTES)
    manifest_keys = {
        "schema_version", "campaign_schema_version", "campaign_identity", "identity_sha256",
        "structure", "evidence_qualification", "gates", "files",
    }
    _exact_keys(manifest, manifest_keys, "pack manifest")
    if manifest["schema_version"] != PACK_SCHEMA_VERSION or manifest["campaign_schema_version"] != 2 or \
       manifest["structure"] != "VALID" or manifest["evidence_qualification"] not in {"COMPLETE", "INCOMPLETE"}:
        raise RcEvidenceError("pack manifest schema/structure/qualification is invalid")
    files = manifest["files"]
    if not isinstance(files, list):
        raise RcEvidenceError("pack manifest files must be an array")
    expected_non_manifest = set(content) - {"manifest.json"}
    declared: set[str] = set()
    for item in files:
        if not isinstance(item, dict) or set(item) != {"path", "sha256", "bytes"}:
            raise RcEvidenceError("pack manifest file record is malformed")
        name = item["path"]
        _safe_pack_name(name)
        if name in declared or name not in expected_non_manifest:
            raise RcEvidenceError("pack manifest file allowlist is duplicate or mismatched")
        declared.add(name)
        _hex(item["sha256"], 64, "pack file hash")
        _integer(item["bytes"], "pack file size", 1, MAX_PACK_BYTES)
        if len(content[name]) != item["bytes"] or _sha256(content[name]) != item["sha256"]:
            raise RcEvidenceError(f"packed file bytes fail manifest integrity: {name}")
    if declared != expected_non_manifest:
        raise RcEvidenceError("pack contains a file outside the manifest allowlist")

    campaign = _json_bytes(campaign_bytes, "packed campaign", campaign_validator.MAX_BYTES)
    evidence_times = [
        item.get("created_unix", 0)
        for record in campaign.get("stages", []) if isinstance(record, dict)
        for item in record.get("evidence", []) if isinstance(item, dict)
    ]
    structure_now = max([campaign.get("created_unix", 1), campaign.get("updated_unix", 1), *evidence_times])
    try:
        campaign_validator.validate_document(campaign, structure_now)
    except campaign_validator.ValidationError as exc:
        raise RcEvidenceError(f"packed campaign is structurally invalid: {exc}") from exc
    index = _json_bytes(index_bytes, "packed RC evidence index", campaign_validator.MAX_BYTES)
    _validate_index(index, campaign["identity"])
    _privacy_scan(campaign, "packed acceptance campaign metadata")
    _privacy_scan(index, "packed RC evidence index metadata")
    if manifest["campaign_identity"] != campaign["identity"] or \
       manifest["identity_sha256"] != _identity_sha(campaign["identity"]):
        raise RcEvidenceError("pack manifest identity is foreign to packed campaign")

    pseudo_state = SessionState(root=Path("."), session=Path("."), campaign=campaign, index=index)
    assessments: list[GateAssessment] = []
    entries = {item["gate_id"]: item for item in index["evidence"]}
    receipts_for_campaign: dict[str, dict] = {}
    for gate in EXPECTED_GATES:
        entry = entries.get(gate.gate_id)
        if entry is None:
            assessments.append(GateAssessment(gate, "MISSING", detail="required evidence not packed"))
            continue
        receipt_name = f"receipts/{entry['receipt_file']}"
        artifact_name = f"artifacts/{entry['artifact_name']}"
        if receipt_name not in content or artifact_name not in content:
            raise RcEvidenceError("pack index references a missing receipt/artifact")
        receipt_bytes = content[receipt_name]
        if _sha256(receipt_bytes) != entry["receipt_sha256"]:
            raise RcEvidenceError("packed receipt exact-byte hash does not match index")
        receipt = _json_bytes(receipt_bytes, "packed receipt", MAX_RECEIPT_BYTES)
        artifact_bytes = content[artifact_name]
        if _sha256(artifact_bytes) != entry["artifact_sha256"]:
            raise RcEvidenceError("packed artifact exact-byte hash does not match index")
        _validate_receipt(receipt, campaign["identity"], artifact_bytes, now, freshness=True)
        if gate.campaign_stage is not None:
            receipts_for_campaign[gate.campaign_stage] = receipt
        if receipt["automated_passed"] is False or receipt["human_verdict"] == "Fail":
            status = "FAIL"
        elif gate.manual_required and receipt["human_verdict"] == "Pending":
            status = "PENDING"
        else:
            status = "PASS"
        assessments.append(GateAssessment(gate, status, receipt["evidence_id"], "validated exact packed receipt/artifact"))
    expected_campaign_ids = {receipt["evidence_id"] for receipt in receipts_for_campaign.values()}
    if _campaign_evidence_ids(campaign) != expected_campaign_ids:
        raise RcEvidenceError("packed campaign contains unindexed or missing campaign evidence")
    for stage, receipt in receipts_for_campaign.items():
        _validate_campaign_mirror(pseudo_state, receipt, GATE_BY_ID[stage])
    report = AssessmentReport(state=pseudo_state, gates=assessments)
    expected_manifest = _pack_manifest(report, files)
    if manifest != expected_manifest:
        raise RcEvidenceError("pack manifest does not match deterministic evidence assessment")
    if summary_bytes != _summary_text(report).encode("utf-8"):
        raise RcEvidenceError("packed human summary does not match deterministic evidence assessment")
    return report


def verify_pack(pack_path: Path, now: int) -> AssessmentReport:
    return _assessment_from_pack_content(_read_pack_entries(pack_path), now)


def _fixture_binding(identity: dict, gate: ExpectedGate, created: int, test_name: str,
                     automated_passed: bool) -> dict:
    return {
        "schema_version": 2,
        "campaign_schema_version": 2,
        "campaign_id": identity["campaign_id"],
        "session_run_id": identity["session_run_id"],
        "stage": gate.gate_id,
        "test_name": test_name,
        "origin": gate.origin,
        "evidence_class": gate.evidence_class,
        "created_unix": created,
        "rc_commit_sha": identity["rc_commit_sha"],
        "release_artifact_sha256": identity["release_artifact_sha256"],
        "release_artifact_name": identity["release_artifact_name"],
        "release_revision": identity["release_revision"],
        "architecture": identity["architecture"],
        "profile_sha256": identity["profile_sha256"],
        "install_state_sha256": identity["install_state_sha256"],
        "windows_build": identity["windows_build"],
        "topology_fingerprint_sha256": identity["topology_fingerprint_sha256"],
        "scenario_identity": identity["scenario_identity"],
        "automated_passed": automated_passed,
    }


def _fixture_artifact(identity: dict, gate: ExpectedGate, created: int, test_name: str,
                      automated_passed: bool, now: int) -> bytes:
    if gate.gate_id == "Phase3Physical":
        document = {
            "schema_version": 1,
            "state": "MANUAL_PASS",
            "manual_verdict": "PASS",
            "manual_verdict_unix": created,
            "evidence_valid_until_unix": now + campaign_validator.MAX_AGE,
            "environment": {
                "architecture": identity["architecture"],
                "windows_build": identity["windows_build"],
            },
            "profile": {"sha256": identity["profile_sha256"]},
            "session_id": identity["session_run_id"],
        }
    else:
        document = {"release_binding": _fixture_binding(identity, gate, created, test_name, automated_passed)}
    return _canonical_json_bytes(document)


def _fixture_receipt(identity: dict, gate: ExpectedGate, created: int, artifact_name: str,
                     artifact_bytes: bytes, evidence_id: str, test_name: str,
                     automated_passed: bool = True, human_verdict: str | None = None) -> dict:
    if human_verdict is None:
        human_verdict = "Pass" if gate.manual_required else "Pending"
    return {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "campaign_schema_version": 2,
        "campaign_id": identity["campaign_id"],
        "session_run_id": identity["session_run_id"],
        "gate_id": gate.gate_id,
        "evidence_id": evidence_id,
        "test_name": test_name,
        "origin": gate.origin,
        "evidence_class": gate.evidence_class,
        "created_unix": created,
        "artifact_name": artifact_name,
        "artifact_sha256": _sha256(artifact_bytes),
        "artifact_bytes": len(artifact_bytes),
        "rc_commit_sha": identity["rc_commit_sha"],
        "release_artifact_sha256": identity["release_artifact_sha256"],
        "release_artifact_name": identity["release_artifact_name"],
        "release_revision": identity["release_revision"],
        "architecture": identity["architecture"],
        "profile_sha256": identity["profile_sha256"],
        "install_state_sha256": identity["install_state_sha256"],
        "windows_build": identity["windows_build"],
        "topology_fingerprint_sha256": identity["topology_fingerprint_sha256"],
        "scenario_identity": identity["scenario_identity"],
        "automated_passed": automated_passed,
        "human_verdict": human_verdict,
        "note": "bounded self-test evidence",
    }


def _self_inputs(base: Path, rc_commit: str, revision: int, architecture: str) -> tuple[Path, Path, Path]:
    input_root = base / "inputs"
    _assert_plain_directory(input_root, "self-test input root", create=True)
    release = input_root / "HydraSeat-x64.zip"
    profile = input_root / "release-scope.json"
    state = input_root / "install-state.json"
    release.write_bytes(b"hydraseat-release-artifact\n")
    profile.write_bytes(b"{\"release_scope\":\"selftest\"}\n")
    state.write_bytes(_canonical_json_bytes({
        "schemaVersion": 1,
        "releaseVersion": "1.0.0",
        "releaseRevision": revision,
        "commitSha": rc_commit,
        "architecture": architecture,
    }))
    return release, profile, state


def _self_init(root: Path, campaign_id: str, now: int) -> SessionState:
    rc_commit = "a" * 40
    release, profile, state = _self_inputs(root.parent / (root.name + "-input"), rc_commit, 7, "x64")
    return initialize_session(
        root, campaign_id, rc_commit, release, 7, "x64", profile, state,
        "10.0.26100", "e" * 64, "rc-two-seat", now - 1000,
    )


def _write_fixture_incoming(root: Path, campaign_id: str, gate_id: str, now: int,
                            mutate_receipt: Callable[[dict], None] | None = None,
                            mutate_artifact: Callable[[dict], None] | None = None,
                            alter_artifact_after_receipt: bool = False,
                            omit_artifact: bool = False,
                            created: int | None = None) -> str:
    state = load_session(root, campaign_id)
    gate = GATE_BY_ID[gate_id]
    created = now - 100 if created is None else created
    safe_gate = gate_id.lower()
    artifact_name = f"{safe_gate}-evidence.json"
    evidence_id = f"ev-{safe_gate}"
    test_name = f"test-{safe_gate}"
    artifact = _fixture_artifact(state.campaign["identity"], gate, created, test_name, True, now)
    if mutate_artifact is not None:
        artifact_document = _json_bytes(artifact, "self-test artifact", MAX_ARTIFACT_BYTES)
        mutate_artifact(artifact_document)
        artifact = _canonical_json_bytes(artifact_document)
    receipt = _fixture_receipt(state.campaign["identity"], gate, created, artifact_name,
                               artifact, evidence_id, test_name)
    if mutate_receipt is not None:
        mutate_receipt(receipt)
    receipt_name = f"{safe_gate}.receipt.json"
    incoming = state.session / "incoming"
    _atomic_write(incoming / receipt_name, _canonical_json_bytes(receipt))
    if not omit_artifact:
        artifact_to_write = artifact + (b" " if alter_artifact_after_receipt else b"")
        # Path-traversal tests intentionally do not attempt to materialize outside incoming.
        if isinstance(receipt.get("artifact_name"), str) and PORTABLE_LEAF.fullmatch(receipt["artifact_name"]):
            _atomic_write(incoming / receipt["artifact_name"], artifact_to_write)
    return receipt_name


def _expect_error(label: str, operation: Callable[[], object]) -> None:
    try:
        operation()
    except RcEvidenceError:
        return
    raise AssertionError(f"self-test expected rejection: {label}")


def _rewrite_zip(source: Path, destination: Path, mutate_name: str) -> None:
    content = _read_pack_entries(source)
    content[mutate_name] = content[mutate_name] + b"tampered"
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_STORED, allowZip64=False) as archive:
        for name in sorted(content):
            info, data = _zip_entry(name, content[name])
            archive.writestr(info, data)


def self_test() -> None:
    now = 2_000_000_000
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)

        # 1. deterministic initialization.
        root_a = base / "init-a"
        root_b = base / "init-b"
        state_a = _self_init(root_a, "deterministic", now)
        state_b = _self_init(root_b, "deterministic", now)
        if _canonical_json_bytes(state_a.campaign) != _canonical_json_bytes(state_b.campaign) or \
           _canonical_json_bytes(state_a.index) != _canonical_json_bytes(state_b.index):
            raise AssertionError("deterministic initialization differs for identical exact inputs")

        # Existing campaignctl/runner evidence is adopted before unreferenced cleanup.
        runner_root = base / "runner-sync"
        runner_state = _self_init(runner_root, "runner-sync", now)
        runner_gate = GATE_BY_ID["Preflight"]
        runner_created = now - 100
        runner_artifact_name = "runner-preflight.json"
        runner_test_name = "runner-preflight"
        runner_artifact = _fixture_artifact(
            runner_state.campaign["identity"], runner_gate, runner_created,
            runner_test_name, True, now)
        runner_receipt = _fixture_receipt(
            runner_state.campaign["identity"], runner_gate, runner_created,
            runner_artifact_name, runner_artifact, "runner-preflight-evidence", runner_test_name)
        _atomic_write(runner_state.session / "artifacts" / runner_artifact_name, runner_artifact)
        runner_campaign = json.loads(json.dumps(runner_state.campaign))
        runner_campaign["stages"][0].update({
            "state": "Passed", "attempt": 3,
            "started_unix": runner_created - 10, "completed_unix": runner_created + 10,
            "evidence": [_receipt_to_campaign_item(runner_receipt)],
            "diagnostic": "runner-owned diagnostic retained",
        })
        runner_campaign["updated_unix"] = runner_created + 10
        campaign_validator.validate_document(runner_campaign, now)
        _atomic_write(runner_state.session / CAMPAIGN_FILE_NAME, _canonical_json_bytes(runner_campaign))
        runner_report = assess_session(runner_root, "runner-sync", now)
        if runner_report.gates[0].status != "PASS" or len(runner_report.state.index["evidence"]) != 1 or \
           runner_report.state.campaign["stages"][0]["attempt"] != 3 or \
           runner_report.state.campaign["stages"][0]["diagnostic"] != "runner-owned diagnostic retained":
            raise AssertionError("existing campaignctl/runner evidence was not adopted without losing campaign history")

        # 2-3. exact duplicate is idempotent; conflicting duplicate is rejected.
        duplicate_root = base / "duplicate"
        _self_init(duplicate_root, "duplicate", now)
        receipt_name = _write_fixture_incoming(duplicate_root, "duplicate", "Preflight", now)
        if not add_evidence(duplicate_root, "duplicate", receipt_name, now):
            raise AssertionError("first exact receipt was not ingested")
        if add_evidence(duplicate_root, "duplicate", receipt_name, now):
            raise AssertionError("exact duplicate was not idempotent")
        receipt_path = duplicate_root / "duplicate" / "incoming" / receipt_name
        changed = _json_bytes(receipt_path.read_bytes(), "duplicate fixture", MAX_RECEIPT_BYTES)
        changed["note"] = "conflicting duplicate bytes"
        _atomic_write(receipt_path, _canonical_json_bytes(changed))
        _expect_error("conflicting duplicate", lambda: add_evidence(duplicate_root, "duplicate", receipt_name, now))

        resume_tamper_root = base / "resume-tamper"
        _self_init(resume_tamper_root, "resume-tamper", now)
        resume_tamper_receipt = _write_fixture_incoming(
            resume_tamper_root, "resume-tamper", "Preflight", now)
        add_evidence(resume_tamper_root, "resume-tamper", resume_tamper_receipt, now)
        resume_tamper_state = load_session(resume_tamper_root, "resume-tamper")
        tampered_artifact = resume_tamper_state.session / "artifacts" / \
            resume_tamper_state.index["evidence"][0]["artifact_name"]
        tampered_artifact.write_bytes(tampered_artifact.read_bytes() + b"tampered")
        tamper_report = assess_session(resume_tamper_root, "resume-tamper", now)
        if tamper_report.gates[0].status != "TAMPERED":
            raise AssertionError("resume did not surface changed accepted evidence bytes as TAMPERED")
        _expect_error("tampered accepted evidence cannot be packed",
                      lambda: pack_session(resume_tamper_root, "resume-tamper", now))

        resume_missing_root = base / "resume-missing"
        _self_init(resume_missing_root, "resume-missing", now)
        resume_missing_receipt = _write_fixture_incoming(
            resume_missing_root, "resume-missing", "Preflight", now)
        add_evidence(resume_missing_root, "resume-missing", resume_missing_receipt, now)
        resume_missing_state = load_session(resume_missing_root, "resume-missing")
        missing_artifact = resume_missing_state.session / "artifacts" / \
            resume_missing_state.index["evidence"][0]["artifact_name"]
        missing_artifact.unlink()
        missing_report = assess_session(resume_missing_root, "resume-missing", now)
        if missing_report.gates[0].status != "MISSING_FILE":
            raise AssertionError("resume did not surface removed accepted evidence as MISSING_FILE")

        resume_stale_root = base / "resume-stale"
        _self_init(resume_stale_root, "resume-stale", now)
        resume_stale_receipt = _write_fixture_incoming(
            resume_stale_root, "resume-stale", "Preflight", now)
        add_evidence(resume_stale_root, "resume-stale", resume_stale_receipt, now)
        stale_report = assess_session(
            resume_stale_root, "resume-stale", now + campaign_validator.MAX_AGE + 1000)
        if stale_report.gates[0].status != "STALE":
            raise AssertionError("resume did not surface aged accepted evidence as STALE")

        # 4-13. wrong exact identities, class/origin, bytes, missing file, stale, traversal.
        mutations: list[tuple[str, Callable[[dict], None]]] = [
            ("wrong build", lambda r: r.__setitem__("rc_commit_sha", "f" * 40)),
            ("wrong release artifact", lambda r: r.__setitem__("release_artifact_sha256", "f" * 64)),
            ("wrong release revision", lambda r: r.__setitem__("release_revision", 8)),
            ("wrong architecture", lambda r: r.__setitem__("architecture", "arm64")),
            ("wrong profile", lambda r: r.__setitem__("profile_sha256", "f" * 64)),
            ("wrong install state", lambda r: r.__setitem__("install_state_sha256", "f" * 64)),
            ("wrong topology input", lambda r: r.__setitem__("topology_fingerprint_sha256", "f" * 64)),
            ("wrong session", lambda r: r.__setitem__("session_run_id", "foreign-session")),
            ("wrong scenario", lambda r: r.__setitem__("scenario_identity", "foreign-scenario")),
            ("wrong evidence class", lambda r: r.__setitem__("evidence_class", "Physical")),
        ]
        for index, (label, mutation) in enumerate(mutations):
            root = base / f"reject-{index}"
            campaign_id = f"reject-{index}"
            _self_init(root, campaign_id, now)
            name = _write_fixture_incoming(root, campaign_id, "Preflight", now, mutate_receipt=mutation)
            _expect_error(label, lambda root=root, campaign_id=campaign_id, name=name:
                          add_evidence(root, campaign_id, name, now))

        hash_root = base / "hash"
        _self_init(hash_root, "hash", now)
        hash_name = _write_fixture_incoming(hash_root, "hash", "Preflight", now,
                                            alter_artifact_after_receipt=True)
        _expect_error("hash mismatch", lambda: add_evidence(hash_root, "hash", hash_name, now))

        missing_root = base / "missing"
        _self_init(missing_root, "missing", now)
        missing_name = _write_fixture_incoming(missing_root, "missing", "Preflight", now,
                                               omit_artifact=True)
        _expect_error("missing evidence file", lambda: add_evidence(missing_root, "missing", missing_name, now))

        stale_root = base / "stale"
        _self_init(stale_root, "stale", now)
        stale_name = _write_fixture_incoming(
            stale_root, "stale", "Preflight", now,
            created=now - campaign_validator.MAX_AGE - 1,
        )
        _expect_error("stale evidence", lambda: add_evidence(stale_root, "stale", stale_name, now))

        traversal_root = base / "traversal"
        _self_init(traversal_root, "traversal", now)
        traversal_name = _write_fixture_incoming(
            traversal_root, "traversal", "Preflight", now,
            mutate_receipt=lambda r: r.__setitem__("artifact_name", "../secret.json"),
            omit_artifact=True,
        )
        _expect_error("path traversal", lambda: add_evidence(traversal_root, "traversal", traversal_name, now))

        malformed_root = base / "malformed"
        malformed_state = _self_init(malformed_root, "malformed", now)
        _atomic_write(malformed_state.session / "incoming" / "malformed.receipt.json", b"{not-json\n")
        _expect_error("malformed receipt",
                      lambda: add_evidence(malformed_root, "malformed", "malformed.receipt.json", now))

        oversized_root = base / "oversized"
        oversized_state = _self_init(oversized_root, "oversized", now)
        _atomic_write(oversized_state.session / "incoming" / "oversized.receipt.json",
                      b"x" * (MAX_RECEIPT_BYTES + 1))
        _expect_error("oversized receipt",
                      lambda: add_evidence(oversized_root, "oversized", "oversized.receipt.json", now))

        privacy_root = base / "privacy"
        _self_init(privacy_root, "privacy", now)
        privacy_name = _write_fixture_incoming(
            privacy_root, "privacy", "Preflight", now,
            mutate_artifact=lambda document: document.__setitem__("username", "private-user"),
        )
        _expect_error("private username in evidence artifact",
                      lambda: add_evidence(privacy_root, "privacy", privacy_name, now))

        # 15-19. partial summary/strict verification and evidence-class separation.
        partial_root = base / "partial"
        _self_init(partial_root, "partial", now)
        offline_name = _write_fixture_incoming(partial_root, "partial", "Offline", now)
        add_evidence(partial_root, "partial", offline_name, now)  # out-of-order terminal receipt
        preflight_name = _write_fixture_incoming(partial_root, "partial", "Preflight", now)
        add_evidence(partial_root, "partial", preflight_name, now)
        partial_report = assess_session(partial_root, "partial", now)
        partial_text = _summary_text(partial_report)
        if partial_report.complete or "Physical:" not in partial_text or "Manual:" not in partial_text or \
           "MISSING" not in partial_text or "RC RESULT: NOT READY / INCOMPLETE" not in partial_text:
            raise AssertionError("partial controlled-only campaign does not expose missing manual/physical gates")
        try:
            strict_verify_session(partial_root, "partial", now)
            raise AssertionError("strict verification treated incomplete campaign as complete")
        except RcEvidenceIncomplete as incomplete:
            if incomplete.report.complete:
                raise AssertionError("strict incomplete failure carried a complete report")
        partial_pack = pack_session(partial_root, "partial", now)
        partial_pack_report = verify_pack(partial_pack, now)
        if partial_pack_report.complete:
            raise AssertionError("structurally valid incomplete pack was misreported as qualified")

        controlled_physical_root = base / "controlled-physical"
        _self_init(controlled_physical_root, "controlled-physical", now)
        controlled_name = _write_fixture_incoming(
            controlled_physical_root, "controlled-physical", "Phase3Physical", now,
            mutate_receipt=lambda r: r.__setitem__("origin", "ControlledProcess"),
        )
        _expect_error("Controlled cannot satisfy Physical",
                      lambda: add_evidence(controlled_physical_root, "controlled-physical", controlled_name, now))

        synthetic_root = base / "synthetic"
        _self_init(synthetic_root, "synthetic", now)
        synthetic_name = _write_fixture_incoming(
            synthetic_root, "synthetic", "Preflight", now,
            mutate_receipt=lambda r: (r.__setitem__("origin", "Synthetic"),
                                      r.__setitem__("evidence_class", "Synthetic")),
        )
        _expect_error("Synthetic cannot satisfy Controlled",
                      lambda: add_evidence(synthetic_root, "synthetic", synthetic_name, now))

        synthetic_physical_root = base / "synthetic-physical"
        _self_init(synthetic_physical_root, "synthetic-physical", now)
        synthetic_physical_name = _write_fixture_incoming(
            synthetic_physical_root, "synthetic-physical", "Phase3Physical", now,
            mutate_receipt=lambda r: (r.__setitem__("origin", "Synthetic"),
                                      r.__setitem__("evidence_class", "Synthetic")),
        )
        _expect_error("Synthetic cannot satisfy Physical",
                      lambda: add_evidence(
                          synthetic_physical_root, "synthetic-physical", synthetic_physical_name, now))

        # 14, 20-21. deterministic ordering/pack bytes, exact round-trip, tamper rejection.
        complete_roots = [base / "complete-a", base / "complete-b"]
        orders = [list(reversed([gate.gate_id for gate in EXPECTED_GATES])),
                  [gate.gate_id for gate in EXPECTED_GATES]]
        packs: list[Path] = []
        for root, order in zip(complete_roots, orders):
            _self_init(root, "complete", now)
            for gate_id in order:
                receipt = _write_fixture_incoming(root, "complete", gate_id, now,
                                                  created=now - 200 + GATE_ORDER[gate_id])
                add_evidence(root, "complete", receipt, now)
            complete_report = strict_verify_session(root, "complete", now)
            if not complete_report.complete:
                raise AssertionError("complete exact evidence set did not qualify")
            pack = pack_session(root, "complete", now)
            if not verify_pack(pack, now).complete:
                raise AssertionError("valid exact pack failed round-trip verification")
            packs.append(pack)
        if packs[0].read_bytes() != packs[1].read_bytes():
            raise AssertionError("pack bytes depend on evidence ingestion/filesystem order")

        entries = _read_pack_entries(packs[0])
        artifact_entry = next(name for name in sorted(entries) if name.startswith("artifacts/"))
        altered_pack = base / "altered-pack.zip"
        _rewrite_zip(packs[0], altered_pack, artifact_entry)
        _expect_error("altered packed artifact", lambda: verify_pack(altered_pack, now))

    print("RC evidence orchestration self-test passed: all 21 requested cases plus malformed/oversized/privacy and incomplete-pack checks.")


def _now(value: int | None) -> int:
    return int(time.time()) if value is None else value


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Deterministic HydraSeat v1 RC evidence orchestration; never manufactures manual evidence."
    )
    sub = parser.add_subparsers(dest="command", required=True)

    init = sub.add_parser("init", help="initialize one exact RC campaign under out/v1-acceptance")
    init.add_argument("--campaign-id", required=True)
    init.add_argument("--rc-commit-sha", required=True)
    init.add_argument("--release-artifact", required=True, type=Path)
    init.add_argument("--release-revision", required=True, type=int)
    init.add_argument("--architecture", required=True, choices=("x64", "x86", "arm64"))
    init.add_argument("--profile-file", required=True, type=Path)
    init.add_argument("--install-state", required=True, type=Path)
    init.add_argument("--windows-build", required=True)
    init.add_argument("--topology-sha256", required=True)
    init.add_argument("--scenario-id", required=True)
    init.add_argument("--created-unix", type=int)

    add = sub.add_parser("add-evidence", help="ingest one receipt+artifact from the fixed session incoming directory")
    add.add_argument("--campaign-id", required=True)
    add.add_argument("--receipt", required=True, help="leaf receipt filename under <session>/incoming")
    add.add_argument("--now-unix", type=int)

    summary = sub.add_parser("summary", help="print/write a human-readable incomplete-or-complete evidence summary")
    summary.add_argument("--campaign-id", required=True)
    summary.add_argument("--now-unix", type=int)

    resume = sub.add_parser("resume", help="revalidate/resume a partial campaign and print its current evidence summary")
    resume.add_argument("--campaign-id", required=True)
    resume.add_argument("--now-unix", type=int)

    verify = sub.add_parser("verify", help="strict CI verification; exits 2 while any required gate is not PASS")
    verify.add_argument("--campaign-id", required=True)
    verify.add_argument("--now-unix", type=int)

    pack = sub.add_parser("pack", help="create deterministic allowlisted pack; incomplete valid packs are allowed")
    pack.add_argument("--campaign-id", required=True)
    pack.add_argument("--now-unix", type=int)

    verify_pack_parser = sub.add_parser("verify-pack", help="revalidate the fixed deterministic pack for a campaign")
    verify_pack_parser.add_argument("--campaign-id", required=True)
    verify_pack_parser.add_argument("--now-unix", type=int)

    sub.add_parser("self-test", help="run deterministic orchestration regression coverage")
    return parser


def main() -> int:
    args = _build_parser().parse_args()
    try:
        if args.command == "self-test":
            self_test()
            return 0
        if args.command == "init":
            state = initialize_session(
                DEFAULT_CAMPAIGN_ROOT,
                args.campaign_id,
                args.rc_commit_sha,
                args.release_artifact,
                args.release_revision,
                args.architecture,
                args.profile_file,
                args.install_state,
                args.windows_build,
                args.topology_sha256,
                args.scenario_id,
                _now(args.created_unix),
            )
            print(f"Initialized exact RC campaign: {state.campaign['identity']['campaign_id']}")
            print(f"Incoming receipts: out/v1-acceptance/{args.campaign_id}/incoming")
            return 0
        if args.command == "add-evidence":
            added = add_evidence(DEFAULT_CAMPAIGN_ROOT, args.campaign_id, args.receipt, _now(args.now_unix))
            print("Evidence ingested and exact bytes frozen." if added else "Exact duplicate receipt/artifact is already ingested (idempotent).")
            return 0
        if args.command == "summary":
            summarize_session(DEFAULT_CAMPAIGN_ROOT, args.campaign_id, _now(args.now_unix))
            return 0
        if args.command == "resume":
            summarize_session(DEFAULT_CAMPAIGN_ROOT, args.campaign_id, _now(args.now_unix))
            return 0
        if args.command == "verify":
            report = strict_verify_session(DEFAULT_CAMPAIGN_ROOT, args.campaign_id, _now(args.now_unix))
            text = _summary_text(report)
            _atomic_write(report.state.session / SUMMARY_FILE_NAME, text.encode("utf-8"))
            print(text, end="")
            return 0
        if args.command == "pack":
            pack = pack_session(DEFAULT_CAMPAIGN_ROOT, args.campaign_id, _now(args.now_unix))
            report = verify_pack(pack, _now(args.now_unix))
            print(f"Deterministic RC evidence pack structurally valid: out/v1-acceptance/{args.campaign_id}/{PACK_FILE_NAME}")
            print("Evidence qualification: " + ("COMPLETE (not release approval)" if report.complete else "INCOMPLETE / NOT READY"))
            return 0
        if args.command == "verify-pack":
            pack = _session_path(DEFAULT_CAMPAIGN_ROOT, args.campaign_id) / PACK_FILE_NAME
            report = verify_pack(pack, _now(args.now_unix))
            print("RC evidence pack revalidation: PASS")
            print("Evidence qualification: " + ("COMPLETE (not release approval)" if report.complete else "INCOMPLETE / NOT READY"))
            return 0 if report.complete else 2
        raise RcEvidenceError("unknown orchestration command")
    except RcEvidenceIncomplete as exc:
        text = _summary_text(exc.report)
        _atomic_write(exc.report.state.session / SUMMARY_FILE_NAME, text.encode("utf-8"))
        print(text, end="")
        return 2
    except (RcEvidenceError, OSError, zipfile.BadZipFile) as exc:
        print(f"RC evidence orchestration failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
