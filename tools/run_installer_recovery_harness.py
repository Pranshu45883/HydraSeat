#!/usr/bin/env python3
"""Controlled installer/update interruption and restart-recovery acceptance harness.

All mutation is confined to one run directory immediately below the operating
system temporary directory's HydraSeatInstallerAcceptance folder. This tool does
not invoke the production installer and never represents clean-machine evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Any, Iterable

CONTROLLED_EVIDENCE_CLASS = "Controlled"
CONTROLLED_ROOT_NAME = "HydraSeatInstallerAcceptance"
CONTROLLED_INTERRUPT_EXIT = 86
CONTROLLED_REJECT_EXIT = 3
SCHEMA_VERSION = 1
MAX_TRANSACTIONS = 8
MAX_JSON_BYTES = 64 * 1024
MAX_OWNED_FILE_BYTES = 1024 * 1024

OWNED_FILES = (
    "HydraSeat.exe",
    "hydra_host.exe",
    "hydra_seat_ui.exe",
    "hydra_watchdog.exe",
    "hydra_reset.exe",
    "hydraseat_profilectl.exe",
    "hydraseat_community_validate.exe",
    "install_hydraseat.ps1",
)

FAULT_POINTS = (
    "before-snapshot",
    "after-snapshot-before-staging",
    "during-staging",
    "after-staging-before-verification",
    "after-staged-verification",
    "before-commit",
    "during-commit",
    "after-commit-before-cleanup",
    "during-rollback",
    "after-rollback-before-journal-cleanup",
)

PREVIOUS_RECOVERY_PHASES = {
    "Prepared", "Staging", "Staged", "StagedVerified", "PreCommit",
    "Committing", "RollingBack", "RolledBack",
}
VALID_PHASES = {"Snapshotting", "Committed", *PREVIOUS_RECOVERY_PHASES}
TX_ID_RE = re.compile(r"^[0-9a-f]{32}$")
RUN_ID_RE = re.compile(r"^run-[A-Za-z0-9._-]{1,96}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")


class HarnessError(RuntimeError):
    pass


@dataclass(frozen=True)
class Candidate:
    version: str
    revision: int
    files: dict[str, bytes]
    state: dict[str, Any]


def controlled_base() -> pathlib.Path:
    return pathlib.Path(tempfile.gettempdir()).resolve() / CONTROLLED_ROOT_NAME


def _norm(path: pathlib.Path) -> str:
    return os.path.normcase(os.path.abspath(os.fspath(path)))


def assert_safe_root(root: pathlib.Path) -> pathlib.Path:
    candidate = pathlib.Path(os.path.abspath(os.fspath(root)))
    base = pathlib.Path(os.path.abspath(os.fspath(controlled_base())))
    if candidate == base or candidate.parent != base or not RUN_ID_RE.fullmatch(candidate.name):
        raise HarnessError(
            f"controlled root must be one run-* directory directly below {base}"
        )
    for variable in ("ProgramFiles", "ProgramData", "LOCALAPPDATA"):
        value = os.environ.get(variable)
        if not value:
            continue
        production = pathlib.Path(os.path.abspath(value)) / "HydraSeat"
        try:
            common = os.path.commonpath([_norm(candidate), _norm(production)])
        except ValueError:
            continue
        if common in {_norm(candidate), _norm(production)}:
            raise HarnessError("controlled root overlaps an installed-product path")
    if base.exists() and is_reparse(base):
        raise HarnessError("controlled acceptance base must not be a reparse point")
    if candidate.exists() and is_reparse(candidate):
        raise HarnessError("controlled acceptance run root must not be a reparse point")
    return candidate


def _lexically_under(root: pathlib.Path, path: pathlib.Path) -> bool:
    try:
        return os.path.commonpath([_norm(root), _norm(path)]) == _norm(root)
    except ValueError:
        return False


def is_reparse(path: pathlib.Path) -> bool:
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    if stat.S_ISLNK(info.st_mode):
        return True
    attributes = getattr(info, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    return bool(reparse_flag and (attributes & reparse_flag))


def assert_plain_path(
    root: pathlib.Path,
    path: pathlib.Path,
    *,
    kind: str | None = None,
    allow_missing: bool = False,
) -> pathlib.Path:
    root = assert_safe_root(root)
    candidate = pathlib.Path(os.path.abspath(os.fspath(path)))
    if not _lexically_under(root, candidate):
        raise HarnessError("path escapes the controlled acceptance root")

    relative = candidate.relative_to(root)
    cursor = root
    if cursor.exists() and is_reparse(cursor):
        raise HarnessError("controlled root became a reparse point")
    for part in relative.parts:
        cursor = cursor / part
        if cursor.exists() and is_reparse(cursor):
            raise HarnessError(f"reparse/symlink path rejected: {cursor}")

    if not candidate.exists():
        if allow_missing:
            return candidate
        raise HarnessError(f"required controlled path is missing: {candidate}")
    if kind == "file" and not candidate.is_file():
        raise HarnessError(f"controlled path is not a normal file: {candidate}")
    if kind == "dir" and not candidate.is_dir():
        raise HarnessError(f"controlled path is not a normal directory: {candidate}")
    return candidate


def safe_remove_tree(root: pathlib.Path, path: pathlib.Path) -> None:
    checked = assert_plain_path(root, path, kind="dir")
    if checked == root:
        raise HarnessError("refusing to recursively remove the controlled run root")
    shutil.rmtree(checked)


def canonical_json_bytes(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def durable_write_bytes(path: pathlib.Path, payload: bytes) -> None:
    if len(payload) > MAX_OWNED_FILE_BYTES:
        raise HarnessError("controlled fixture payload exceeds owned-file bound")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".new")
    if temporary.exists():
        if is_reparse(temporary):
            raise HarnessError("staged write destination is a reparse point")
        temporary.unlink()
    with temporary.open("wb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def durable_write_json(path: pathlib.Path, value: Any) -> None:
    payload = canonical_json_bytes(value)
    if len(payload) <= 0 or len(payload) > MAX_JSON_BYTES:
        raise HarnessError("controlled journal/state JSON exceeds its bounded size")
    durable_write_bytes(path, payload)


def read_bounded_json(path: pathlib.Path, required_fields: set[str]) -> dict[str, Any]:
    if is_reparse(path) or not path.is_file():
        raise HarnessError(f"bounded JSON path is missing or unsafe: {path}")
    size = path.stat().st_size
    if size <= 0 or size > MAX_JSON_BYTES:
        raise HarnessError("bounded JSON file size is invalid")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise HarnessError(f"bounded JSON parse failed: {exc}") from exc
    if not isinstance(value, dict) or set(value) != required_fields:
        raise HarnessError("bounded JSON object has unknown or missing fields")
    return value


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    if is_reparse(path) or not path.is_file():
        raise HarnessError(f"hash input is missing or unsafe: {path}")
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(64 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > MAX_OWNED_FILE_BYTES:
                raise HarnessError("controlled owned file exceeds hash bound")
            digest.update(chunk)
    return digest.hexdigest()


def make_candidate(version: str, revision: int) -> Candidate:
    if not re.fullmatch(r"[A-Za-z0-9._+-]{1,64}", version) or revision <= 0:
        raise HarnessError("candidate version/revision is invalid")
    files: dict[str, bytes] = {}
    records: list[dict[str, Any]] = []
    for file_name in OWNED_FILES:
        payload = (
            "HydraSeat controlled installer acceptance fixture\n"
            f"version={version}\nrevision={revision}\nfile={file_name}\n"
        ).encode("utf-8")
        files[file_name] = payload
        records.append({"file_name": file_name, "sha256": sha256_bytes(payload)})
    state = {
        "schema_version": SCHEMA_VERSION,
        "evidence_class": CONTROLLED_EVIDENCE_CLASS,
        "release_version": version,
        "release_revision": revision,
        "owned_files": records,
    }
    return Candidate(version, revision, files, state)


def install_dir(root: pathlib.Path) -> pathlib.Path:
    return root / "install"


def data_dir(root: pathlib.Path) -> pathlib.Path:
    return root / "data"


def state_path(root: pathlib.Path) -> pathlib.Path:
    return data_dir(root) / "install-state.json"


def registration_path(root: pathlib.Path) -> pathlib.Path:
    return data_dir(root) / "registration.json"


def transaction_root(root: pathlib.Path) -> pathlib.Path:
    return data_dir(root) / "transactions"


def current_state(root: pathlib.Path) -> dict[str, Any] | None:
    root = assert_safe_root(root)
    install = install_dir(root)
    state_file = state_path(root)
    registration = registration_path(root)
    if not state_file.exists():
        for file_name in OWNED_FILES:
            if (install / file_name).exists():
                raise HarnessError("owned file exists without controlled install state")
        if registration.exists():
            raise HarnessError("registration exists without controlled install state")
        return None

    state = read_bounded_json(
        state_file,
        {"schema_version", "evidence_class", "release_version", "release_revision", "owned_files"},
    )
    if state["schema_version"] != SCHEMA_VERSION or state["evidence_class"] != CONTROLLED_EVIDENCE_CLASS:
        raise HarnessError("controlled installed state schema/evidence class is invalid")
    if not re.fullmatch(r"[A-Za-z0-9._+-]{1,64}", str(state["release_version"])):
        raise HarnessError("controlled installed version is invalid")
    if not isinstance(state["release_revision"], int) or state["release_revision"] <= 0:
        raise HarnessError("controlled installed revision is invalid")

    records = state["owned_files"]
    if not isinstance(records, list) or len(records) != len(OWNED_FILES):
        raise HarnessError("controlled installed state does not name the exact owned set")
    seen: set[str] = set()
    for record in records:
        if not isinstance(record, dict) or set(record) != {"file_name", "sha256"}:
            raise HarnessError("controlled owned-file state record is malformed")
        name = record["file_name"]
        digest = record["sha256"]
        if (
            name not in OWNED_FILES
            or name in seen
            or not isinstance(digest, str)
            or not HEX64_RE.fullmatch(digest)
        ):
            raise HarnessError("controlled owned-file state record is invalid or duplicated")
        seen.add(name)
        owned_path = assert_plain_path(root, install / name, kind="file")
        if sha256_file(owned_path) != digest:
            raise HarnessError("controlled installed owned-file hash does not match state")
    if seen != set(OWNED_FILES):
        raise HarnessError("controlled installed state is missing an owned file")

    registration_value = read_bounded_json(
        registration,
        {"schema_version", "evidence_class", "release_version", "release_revision"},
    )
    if (
        registration_value["schema_version"] != SCHEMA_VERSION
        or registration_value["evidence_class"] != CONTROLLED_EVIDENCE_CLASS
        or registration_value["release_version"] != state["release_version"]
        or registration_value["release_revision"] != state["release_revision"]
    ):
        raise HarnessError("controlled registration does not match installed state")
    return state


def state_identity(state: dict[str, Any] | None) -> str:
    if state is None:
        return "absent"
    return sha256_bytes(canonical_json_bytes(state))


def exact_expected_state(root: pathlib.Path, candidate: Candidate | None) -> None:
    actual = current_state(root)
    expected = None if candidate is None else candidate.state
    if actual != expected:
        raise HarnessError("committed controlled state is not the exact expected generation")


def deterministic_transaction_id(scenario: str) -> str:
    return hashlib.sha256(scenario.encode("utf-8")).hexdigest()[:32]


def journal_path(tx_root: pathlib.Path) -> pathlib.Path:
    return tx_root / "transaction-state.json"


def write_journal(
    tx_root: pathlib.Path,
    transaction_id: str,
    phase: str,
    action: str,
    previous_manifest_hash: str,
    candidate_manifest_hash: str,
) -> None:
    if not TX_ID_RE.fullmatch(transaction_id) or phase not in VALID_PHASES:
        raise HarnessError("controlled transaction journal arguments are invalid")
    durable_write_json(
        journal_path(tx_root),
        {
            "schema_version": SCHEMA_VERSION,
            "evidence_class": CONTROLLED_EVIDENCE_CLASS,
            "transaction_id": transaction_id,
            "phase": phase,
            "action": action,
            "previous_manifest_sha256": previous_manifest_hash,
            "candidate_manifest_sha256": candidate_manifest_hash,
        },
    )


def read_journal(tx_root: pathlib.Path) -> dict[str, Any]:
    value = read_bounded_json(
        journal_path(tx_root),
        {
            "schema_version", "evidence_class", "transaction_id", "phase", "action",
            "previous_manifest_sha256", "candidate_manifest_sha256",
        },
    )
    if value["schema_version"] != SCHEMA_VERSION or value["evidence_class"] != CONTROLLED_EVIDENCE_CLASS:
        raise HarnessError("controlled transaction journal schema/evidence class is invalid")
    transaction_id = value["transaction_id"]
    if not isinstance(transaction_id, str) or not TX_ID_RE.fullmatch(transaction_id):
        raise HarnessError("controlled transaction id is invalid")
    if value["phase"] not in VALID_PHASES:
        raise HarnessError("controlled transaction journal has an unknown phase")
    if value["action"] not in {"install", "repair", "update", "uninstall"}:
        raise HarnessError("controlled transaction journal action is invalid")
    for field in ("previous_manifest_sha256", "candidate_manifest_sha256"):
        text = value[field]
        if not isinstance(text, str) or (text and not HEX64_RE.fullmatch(text)):
            raise HarnessError("controlled transaction journal manifest hash is invalid")
    return value


def snapshot_manifest(
    root: pathlib.Path,
    backup: pathlib.Path,
    previous: dict[str, Any] | None,
) -> dict[str, Any]:
    files: list[dict[str, Any]] = []
    install = install_dir(root)
    for name in OWNED_FILES:
        source = install / name
        present = source.exists()
        digest = ""
        if present:
            checked = assert_plain_path(root, source, kind="file")
            payload = checked.read_bytes()
            digest = sha256_bytes(payload)
            durable_write_bytes(backup / name, payload)
        files.append({"file_name": name, "present": present, "sha256": digest})

    state_present = state_path(root).exists()
    registration_present = registration_path(root).exists()
    state_hash = ""
    registration_hash = ""
    if state_present:
        checked = assert_plain_path(root, state_path(root), kind="file")
        payload = checked.read_bytes()
        state_hash = sha256_bytes(payload)
        durable_write_bytes(backup / "install-state.json", payload)
    if registration_present:
        checked = assert_plain_path(root, registration_path(root), kind="file")
        payload = checked.read_bytes()
        registration_hash = sha256_bytes(payload)
        durable_write_bytes(backup / "registration.json", payload)

    return {
        "schema_version": SCHEMA_VERSION,
        "evidence_class": CONTROLLED_EVIDENCE_CLASS,
        "state": previous,
        "files": files,
        "state_file_present": state_present,
        "state_file_sha256": state_hash,
        "registration_present": registration_present,
        "registration_sha256": registration_hash,
    }


def candidate_manifest(candidate: Candidate | None, action: str) -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    if candidate is not None:
        for name in OWNED_FILES:
            records.append({"file_name": name, "sha256": sha256_bytes(candidate.files[name])})
    return {
        "schema_version": SCHEMA_VERSION,
        "evidence_class": CONTROLLED_EVIDENCE_CLASS,
        "action": action,
        "state": None if candidate is None else candidate.state,
        "files": records,
    }


def validate_previous_snapshot(
    root: pathlib.Path, tx_root: pathlib.Path, journal: dict[str, Any]
) -> dict[str, Any]:
    backup = assert_plain_path(root, tx_root / "backup", kind="dir")
    manifest_path = assert_plain_path(root, tx_root / "previous-manifest.json", kind="file")
    if sha256_file(manifest_path) != journal["previous_manifest_sha256"]:
        raise HarnessError("previous-state manifest hash changed")
    manifest = read_bounded_json(
        manifest_path,
        {
            "schema_version", "evidence_class", "state", "files",
            "state_file_present", "state_file_sha256",
            "registration_present", "registration_sha256",
        },
    )
    if manifest["schema_version"] != SCHEMA_VERSION or manifest["evidence_class"] != CONTROLLED_EVIDENCE_CLASS:
        raise HarnessError("previous-state manifest schema/evidence class is invalid")
    records = manifest["files"]
    if not isinstance(records, list) or len(records) != len(OWNED_FILES):
        raise HarnessError("previous-state manifest owned set is malformed")

    expected_entries: set[str] = set()
    seen: set[str] = set()
    for record in records:
        if not isinstance(record, dict) or set(record) != {"file_name", "present", "sha256"}:
            raise HarnessError("previous-state manifest file record is malformed")
        name = record["file_name"]
        present = record["present"]
        digest = record["sha256"]
        if name not in OWNED_FILES or name in seen or not isinstance(present, bool):
            raise HarnessError("previous-state manifest file record is invalid or duplicated")
        seen.add(name)
        if present:
            if not isinstance(digest, str) or not HEX64_RE.fullmatch(digest):
                raise HarnessError("previous owned-file hash is invalid")
            backup_file = assert_plain_path(root, backup / name, kind="file")
            if sha256_file(backup_file) != digest:
                raise HarnessError("altered previous-state owned-file hash detected")
            expected_entries.add(name)
        elif digest != "":
            raise HarnessError("absent previous owned file carries a hash")
    if seen != set(OWNED_FILES):
        raise HarnessError("previous-state manifest is missing an owned file")

    for present_key, hash_key, name in (
        ("state_file_present", "state_file_sha256", "install-state.json"),
        ("registration_present", "registration_sha256", "registration.json"),
    ):
        present = manifest[present_key]
        digest = manifest[hash_key]
        if not isinstance(present, bool):
            raise HarnessError("previous-state metadata presence flag is invalid")
        if present:
            if not isinstance(digest, str) or not HEX64_RE.fullmatch(digest):
                raise HarnessError("previous-state metadata hash is invalid")
            backup_file = assert_plain_path(root, backup / name, kind="file")
            if sha256_file(backup_file) != digest:
                raise HarnessError("altered previous-state metadata hash detected")
            expected_entries.add(name)
        elif digest != "":
            raise HarnessError("absent previous-state metadata carries a hash")

    actual_entries = {entry.name for entry in backup.iterdir()}
    if actual_entries != expected_entries:
        raise HarnessError("previous-state backup contains a missing or unexpected owned entry")
    return manifest


def validate_candidate_manifest(
    root: pathlib.Path, tx_root: pathlib.Path, journal: dict[str, Any]
) -> dict[str, Any]:
    manifest_path = assert_plain_path(root, tx_root / "candidate-manifest.json", kind="file")
    if sha256_file(manifest_path) != journal["candidate_manifest_sha256"]:
        raise HarnessError("candidate manifest hash changed")
    manifest = read_bounded_json(
        manifest_path,
        {"schema_version", "evidence_class", "action", "state", "files"},
    )
    if (
        manifest["schema_version"] != SCHEMA_VERSION
        or manifest["evidence_class"] != CONTROLLED_EVIDENCE_CLASS
        or manifest["action"] != journal["action"]
    ):
        raise HarnessError("candidate manifest identity is invalid")
    return manifest


def validate_stage(root: pathlib.Path, tx_root: pathlib.Path, candidate_value: dict[str, Any]) -> None:
    stage = assert_plain_path(root, tx_root / "stage", kind="dir")
    records = candidate_value["files"]
    if not isinstance(records, list):
        raise HarnessError("candidate stage manifest files are malformed")
    expected: dict[str, str] = {}
    for record in records:
        if not isinstance(record, dict) or set(record) != {"file_name", "sha256"}:
            raise HarnessError("candidate stage manifest file record is malformed")
        name = record["file_name"]
        digest = record["sha256"]
        if (
            name not in OWNED_FILES
            or name in expected
            or not isinstance(digest, str)
            or not HEX64_RE.fullmatch(digest)
        ):
            raise HarnessError("candidate stage manifest file record is invalid or duplicated")
        expected[name] = digest
    if candidate_value["state"] is None:
        if expected:
            raise HarnessError("uninstall candidate unexpectedly carries staged files")
    elif set(expected) != set(OWNED_FILES):
        raise HarnessError("candidate stage manifest does not contain the exact owned set")

    entries = list(stage.iterdir())
    if {entry.name for entry in entries} != set(expected):
        raise HarnessError("stage contains a missing or unexpected owned entry")
    for entry in entries:
        checked = assert_plain_path(root, entry, kind="file")
        if sha256_file(checked) != expected[entry.name]:
            raise HarnessError("altered staged hash detected")


def write_candidate_state(root: pathlib.Path, candidate: Candidate) -> None:
    install_dir(root).mkdir(parents=True, exist_ok=True)
    data_dir(root).mkdir(parents=True, exist_ok=True)
    durable_write_json(state_path(root), candidate.state)
    durable_write_json(
        registration_path(root),
        {
            "schema_version": SCHEMA_VERSION,
            "evidence_class": CONTROLLED_EVIDENCE_CLASS,
            "release_version": candidate.version,
            "release_revision": candidate.revision,
        },
    )


def verify_manifest_state(root: pathlib.Path, manifest: dict[str, Any]) -> None:
    expected_state = manifest["state"]
    actual_state = current_state(root)
    if actual_state != expected_state:
        raise HarnessError("recovery verification found a mixed or unexpected committed state")


def restore_previous(
    root: pathlib.Path,
    tx_root: pathlib.Path,
    journal: dict[str, Any],
    *,
    fault: str | None = None,
) -> None:
    previous = validate_previous_snapshot(root, tx_root, journal)
    assert_plain_path(root, tx_root / "stage", kind="dir")
    write_journal(
        tx_root,
        journal["transaction_id"],
        "RollingBack",
        journal["action"],
        journal["previous_manifest_sha256"],
        journal["candidate_manifest_sha256"],
    )

    install_dir(root).mkdir(parents=True, exist_ok=True)
    backup = tx_root / "backup"
    records = previous["files"]
    midpoint = max(1, len(records) // 2)
    for index, record in enumerate(records):
        destination = install_dir(root) / record["file_name"]
        if record["present"]:
            source = assert_plain_path(root, backup / record["file_name"], kind="file")
            durable_write_bytes(destination, source.read_bytes())
        elif destination.exists():
            assert_plain_path(root, destination, kind="file").unlink()
        if fault == "during-rollback" and index + 1 == midpoint:
            controlled_interrupt("during-rollback")

    if previous["state_file_present"]:
        source = assert_plain_path(root, backup / "install-state.json", kind="file")
        durable_write_bytes(state_path(root), source.read_bytes())
    elif state_path(root).exists():
        assert_plain_path(root, state_path(root), kind="file").unlink()

    if previous["registration_present"]:
        source = assert_plain_path(root, backup / "registration.json", kind="file")
        durable_write_bytes(registration_path(root), source.read_bytes())
    elif registration_path(root).exists():
        assert_plain_path(root, registration_path(root), kind="file").unlink()

    verify_manifest_state(root, previous)
    write_journal(
        tx_root,
        journal["transaction_id"],
        "RolledBack",
        journal["action"],
        journal["previous_manifest_sha256"],
        journal["candidate_manifest_sha256"],
    )
    if fault == "after-rollback-before-journal-cleanup":
        controlled_interrupt("after-rollback-before-journal-cleanup")
    safe_remove_tree(root, tx_root)


def enumerate_transactions(root: pathlib.Path) -> list[pathlib.Path]:
    tx_parent = transaction_root(root)
    if not tx_parent.exists():
        return []
    parent = assert_plain_path(root, tx_parent, kind="dir")
    entries = list(parent.iterdir())
    if len(entries) > MAX_TRANSACTIONS:
        raise HarnessError("controlled transaction set exceeds the bounded maximum")
    for entry in entries:
        assert_plain_path(root, entry, kind="dir")
        if not TX_ID_RE.fullmatch(entry.name):
            raise HarnessError("controlled transaction root contains an unknown entry")
    return sorted(entries, key=lambda path: path.name)


def recover_interrupted(root: pathlib.Path) -> int:
    root = assert_safe_root(root)
    entries = enumerate_transactions(root)
    journals: list[tuple[pathlib.Path, dict[str, Any]]] = []
    seen_ids: set[str] = set()
    for entry in entries:
        journal = read_journal(entry)
        transaction_id = journal["transaction_id"]
        if transaction_id in seen_ids:
            raise HarnessError("duplicate controlled transaction id")
        seen_ids.add(transaction_id)
        journals.append((entry, journal))
    for entry, journal in journals:
        if journal["transaction_id"] != entry.name:
            raise HarnessError("transaction directory and journal identity disagree")

    recovered = 0
    for tx_root, journal in journals:
        phase = journal["phase"]
        if phase == "Snapshotting":
            safe_remove_tree(root, tx_root)
            recovered += 1
            continue

        assert_plain_path(root, tx_root / "backup", kind="dir")
        assert_plain_path(root, tx_root / "stage", kind="dir")
        previous = validate_previous_snapshot(root, tx_root, journal)
        candidate_value = validate_candidate_manifest(root, tx_root, journal)

        if phase in {"Staged", "StagedVerified", "PreCommit", "Committing", "Committed"}:
            validate_stage(root, tx_root, candidate_value)

        if phase == "Committed":
            verify_manifest_state(root, candidate_value)
            safe_remove_tree(root, tx_root)
        elif phase == "RolledBack":
            verify_manifest_state(root, previous)
            safe_remove_tree(root, tx_root)
        else:
            restore_previous(root, tx_root, journal)
        recovered += 1
    return recovered


class MutationLock:
    def __init__(self, root: pathlib.Path) -> None:
        self.root = assert_safe_root(root)
        self.path = data_dir(self.root) / "controlled-mutation.lock"
        self.stream: Any = None
        self.locked = False

    def __enter__(self) -> "MutationLock":
        data_dir(self.root).mkdir(parents=True, exist_ok=True)
        if self.path.exists() and is_reparse(self.path):
            raise HarnessError("controlled mutation lock path is a reparse point")
        self.stream = self.path.open("a+b")
        self.stream.seek(0, os.SEEK_END)
        if self.stream.tell() == 0:
            self.stream.write(b"0")
            self.stream.flush()
            os.fsync(self.stream.fileno())
        self.stream.seek(0)
        try:
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(self.stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except (OSError, BlockingIOError) as exc:
            self.stream.close()
            self.stream = None
            raise HarnessError(
                "another controlled installer mutation owns the authoritative lock"
            ) from exc
        self.locked = True
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        if self.stream is None:
            return
        try:
            if self.locked:
                if os.name == "nt":
                    import msvcrt

                    self.stream.seek(0)
                    msvcrt.locking(self.stream.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl

                    fcntl.flock(self.stream.fileno(), fcntl.LOCK_UN)
        finally:
            self.stream.close()
            self.stream = None
            self.locked = False


def controlled_interrupt(fault: str) -> None:
    print(
        json.dumps(
            {
                "evidence_class": CONTROLLED_EVIDENCE_CLASS,
                "status": "Interrupted",
                "fault_point": fault,
                "evidence_note": "controlled process-interruption evidence",
            },
            sort_keys=True,
        ),
        flush=True,
    )
    os._exit(CONTROLLED_INTERRUPT_EXIT)


def _write_manifest_files(
    tx_root: pathlib.Path,
    previous_value: dict[str, Any],
    candidate_value: dict[str, Any],
) -> tuple[str, str]:
    previous_path = tx_root / "previous-manifest.json"
    candidate_path = tx_root / "candidate-manifest.json"
    durable_write_json(previous_path, previous_value)
    durable_write_json(candidate_path, candidate_value)
    return sha256_file(previous_path), sha256_file(candidate_path)


def run_mutation(
    root: pathlib.Path,
    *,
    action: str,
    scenario: str,
    version: str | None,
    revision: int | None,
    expected_previous_identity: str | None,
    fault: str | None,
    force_verify_failure: bool,
    hold_lock_stdin: bool,
) -> dict[str, Any]:
    root = assert_safe_root(root)
    if action not in {"install", "repair", "update", "uninstall"}:
        raise HarnessError("controlled mutation action is invalid")
    if not scenario or len(scenario) > 128:
        raise HarnessError("controlled scenario identity is invalid or unbounded")
    if fault is not None and fault not in FAULT_POINTS:
        raise HarnessError("controlled fault point is invalid")
    if action == "uninstall":
        candidate = None
    else:
        if version is None or revision is None:
            raise HarnessError("controlled install/repair/update requires a candidate identity")
        candidate = make_candidate(version, revision)

    with MutationLock(root):
        if hold_lock_stdin:
            print("CONTROLLED_LOCK_ACQUIRED", flush=True)
            if sys.stdin.readline() == "":
                raise HarnessError("controlled lock holder input closed before release")

        recover_interrupted(root)
        previous = current_state(root)
        if expected_previous_identity is not None and state_identity(previous) != expected_previous_identity:
            raise HarnessError("expected-previous CAS rejected a stale controlled writer")
        if action == "install" and previous is not None:
            raise HarnessError("controlled install requires an empty previous state")
        if action in {"repair", "update", "uninstall"} and previous is None:
            raise HarnessError("controlled repair/update/uninstall requires a previous committed state")
        if action == "repair" and candidate is not None:
            if (
                candidate.version != previous["release_version"]
                or candidate.revision != previous["release_revision"]
            ):
                raise HarnessError("controlled repair cannot change release identity")

        if fault == "before-snapshot":
            controlled_interrupt("before-snapshot")

        tx_parent = transaction_root(root)
        tx_parent.mkdir(parents=True, exist_ok=True)
        assert_plain_path(root, tx_parent, kind="dir")
        transaction_id = deterministic_transaction_id(scenario)
        tx_root = tx_parent / transaction_id
        if tx_root.exists():
            raise HarnessError("controlled deterministic transaction id already exists")
        backup = tx_root / "backup"
        stage = tx_root / "stage"
        backup.mkdir(parents=True)
        stage.mkdir()
        assert_plain_path(root, tx_root, kind="dir")
        assert_plain_path(root, backup, kind="dir")
        assert_plain_path(root, stage, kind="dir")
        write_journal(tx_root, transaction_id, "Snapshotting", action, "", "")

        previous_value = snapshot_manifest(root, backup, previous)
        candidate_value = candidate_manifest(candidate, action)
        previous_hash, candidate_hash = _write_manifest_files(
            tx_root, previous_value, candidate_value
        )
        write_journal(
            tx_root, transaction_id, "Prepared", action, previous_hash, candidate_hash
        )
        if fault == "after-snapshot-before-staging":
            controlled_interrupt("after-snapshot-before-staging")

        write_journal(
            tx_root, transaction_id, "Staging", action, previous_hash, candidate_hash
        )
        if candidate is not None:
            midpoint = max(1, len(OWNED_FILES) // 2)
            for index, name in enumerate(OWNED_FILES):
                durable_write_bytes(stage / name, candidate.files[name])
                if fault == "during-staging" and index + 1 == midpoint:
                    controlled_interrupt("during-staging")
        write_journal(tx_root, transaction_id, "Staged", action, previous_hash, candidate_hash)
        if fault == "after-staging-before-verification":
            controlled_interrupt("after-staging-before-verification")

        validate_stage(root, tx_root, candidate_value)
        write_journal(
            tx_root, transaction_id, "StagedVerified", action, previous_hash, candidate_hash
        )
        if fault == "after-staged-verification":
            controlled_interrupt("after-staged-verification")

        write_journal(tx_root, transaction_id, "PreCommit", action, previous_hash, candidate_hash)
        if fault == "before-commit":
            controlled_interrupt("before-commit")

        write_journal(tx_root, transaction_id, "Committing", action, previous_hash, candidate_hash)
        try:
            midpoint = max(1, len(OWNED_FILES) // 2)
            if candidate is None:
                for index, name in enumerate(OWNED_FILES):
                    destination = install_dir(root) / name
                    if destination.exists():
                        assert_plain_path(root, destination, kind="file").unlink()
                    if fault == "during-commit" and index + 1 == midpoint:
                        controlled_interrupt("during-commit")
                for metadata_path in (state_path(root), registration_path(root)):
                    if metadata_path.exists():
                        assert_plain_path(root, metadata_path, kind="file").unlink()
            else:
                install_dir(root).mkdir(parents=True, exist_ok=True)
                for index, name in enumerate(OWNED_FILES):
                    source = assert_plain_path(root, stage / name, kind="file")
                    durable_write_bytes(install_dir(root) / name, source.read_bytes())
                    if fault == "during-commit" and index + 1 == midpoint:
                        controlled_interrupt("during-commit")
                write_candidate_state(root, candidate)

            if force_verify_failure:
                target = install_dir(root) / OWNED_FILES[0]
                if target.exists():
                    durable_write_bytes(target, b"controlled verification failure\n")
            verify_manifest_state(root, candidate_value)
        except HarnessError:
            latest = read_journal(tx_root)
            restore_previous(root, tx_root, latest, fault=fault)
            raise

        write_journal(tx_root, transaction_id, "Committed", action, previous_hash, candidate_hash)
        if fault == "after-commit-before-cleanup":
            controlled_interrupt("after-commit-before-cleanup")
        safe_remove_tree(root, tx_root)

        return {
            "evidence_class": CONTROLLED_EVIDENCE_CLASS,
            "status": "Applied" if candidate is not None else "Removed",
            "action": action,
            "resulting_state_identity": state_identity(current_state(root)),
        }


def initialize_root(root: pathlib.Path) -> None:
    root = assert_safe_root(root)
    root.mkdir(parents=False, exist_ok=False)
    install_dir(root).mkdir()
    data_dir(root).mkdir()
    durable_write_bytes(
        install_dir(root) / "unrelated-user-file.txt", b"user sentinel install\n"
    )
    durable_write_bytes(
        data_dir(root) / "unrelated-data-file.txt", b"user sentinel data\n"
    )


def assert_sentinels(root: pathlib.Path) -> None:
    install_sentinel = assert_plain_path(
        root, install_dir(root) / "unrelated-user-file.txt", kind="file"
    )
    data_sentinel = assert_plain_path(
        root, data_dir(root) / "unrelated-data-file.txt", kind="file"
    )
    if install_sentinel.read_bytes() != b"user sentinel install\n":
        raise HarnessError("unrelated install sentinel changed")
    if data_sentinel.read_bytes() != b"user sentinel data\n":
        raise HarnessError("unrelated data sentinel changed")


def safe_cleanup_run(root: pathlib.Path) -> None:
    root = assert_safe_root(root)
    if not root.exists():
        return
    if is_reparse(root):
        raise HarnessError("refusing cleanup of a reparse-point run root")
    shutil.rmtree(root)


def child_command(root: pathlib.Path, *arguments: str) -> list[str]:
    return [
        sys.executable,
        os.path.abspath(__file__),
        "--child",
        "--root",
        os.fspath(root),
        *arguments,
    ]


def run_child(
    root: pathlib.Path,
    *arguments: str,
    expected: Iterable[int] = (0,),
) -> subprocess.CompletedProcess[str]:
    process = subprocess.run(
        child_command(root, *arguments),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if process.returncode not in set(expected):
        raise HarnessError(
            f"controlled child returned {process.returncode}; "
            f"stdout={process.stdout!r}; stderr={process.stderr!r}"
        )
    return process


def child_mutation_args(
    *,
    action: str,
    scenario: str,
    candidate: Candidate | None = None,
    expected_identity: str | None = None,
    fault: str | None = None,
    force_verify_failure: bool = False,
    hold_lock_stdin: bool = False,
) -> list[str]:
    arguments = ["mutate", "--action", action, "--scenario", scenario]
    if candidate is not None:
        arguments += ["--version", candidate.version, "--revision", str(candidate.revision)]
    if expected_identity is not None:
        arguments += ["--expected-previous-identity", expected_identity]
    if fault is not None:
        arguments += ["--fault", fault]
    if force_verify_failure:
        arguments.append("--force-verify-failure")
    if hold_lock_stdin:
        arguments.append("--hold-lock-stdin")
    return arguments


def assert_no_transactions(root: pathlib.Path) -> None:
    if enumerate_transactions(root):
        raise HarnessError("successful controlled scenario left transaction residue")


def new_run_root(label: str, counter: int) -> pathlib.Path:
    safe_label = re.sub(r"[^A-Za-z0-9._-]", "-", label)[:48]
    return controlled_base() / f"run-{os.getpid()}-{counter:03d}-{safe_label}"


def install_initial(root: pathlib.Path, candidate: Candidate, scenario: str) -> None:
    run_child(
        root,
        *child_mutation_args(action="install", scenario=scenario, candidate=candidate),
    )
    exact_expected_state(root, candidate)
    assert_sentinels(root)
    assert_no_transactions(root)


def self_test_normal(counter: int, keep: bool) -> dict[str, Any]:
    root = new_run_root("normal-lifecycle", counter)
    initialize_root(root)
    first = make_candidate("1.0.0", 100)
    second = make_candidate("1.1.0", 110)
    try:
        install_initial(root, first, "normal-install")
        run_child(
            root,
            *child_mutation_args(
                action="repair", scenario="normal-repair", candidate=first
            ),
        )
        exact_expected_state(root, first)
        expected = state_identity(current_state(root))
        run_child(
            root,
            *child_mutation_args(
                action="update",
                scenario="normal-update",
                candidate=second,
                expected_identity=expected,
            ),
        )
        exact_expected_state(root, second)
        run_child(
            root,
            *child_mutation_args(action="uninstall", scenario="normal-uninstall"),
        )
        exact_expected_state(root, None)
        assert_sentinels(root)
        assert_no_transactions(root)
        return {"name": "normal-install-repair-update-uninstall", "status": "Pass"}
    finally:
        if not keep:
            safe_cleanup_run(root)


def self_test_interruptions(
    start_counter: int, keep: bool
) -> tuple[list[dict[str, Any]], int]:
    results: list[dict[str, Any]] = []
    counter = start_counter
    first = make_candidate("1.0.0", 100)
    second = make_candidate("1.1.0", 110)
    rollback_faults = {"during-rollback", "after-rollback-before-journal-cleanup"}
    for fault in FAULT_POINTS:
        root = new_run_root(f"interrupt-{fault}", counter)
        counter += 1
        initialize_root(root)
        try:
            install_initial(root, first, f"seed-{fault}")
            expected_identity = state_identity(current_state(root))
            run_child(
                root,
                *child_mutation_args(
                    action="update",
                    scenario=f"fault-{fault}",
                    candidate=second,
                    expected_identity=expected_identity,
                    fault=fault,
                    force_verify_failure=fault in rollback_faults,
                ),
                expected=(CONTROLLED_INTERRUPT_EXIT,),
            )

            # Recovery is always a new interpreter process. This is deliberately
            # controlled process-interruption evidence, not machine power loss.
            run_child(root, "recover")
            expected_candidate = (
                second if fault == "after-commit-before-cleanup" else first
            )
            exact_expected_state(root, expected_candidate)
            assert_sentinels(root)
            assert_no_transactions(root)

            # Recovery of a terminal state must be idempotent in another fresh
            # invocation and must not rewrite the committed generation.
            before = state_identity(current_state(root))
            run_child(root, "recover")
            if state_identity(current_state(root)) != before:
                raise HarnessError("idempotent recovery changed a terminal state")
            results.append(
                {"name": fault, "status": "Pass", "restart_recovery": True}
            )
        finally:
            if not keep:
                safe_cleanup_run(root)
    return results, counter


def self_test_cas(counter: int, keep: bool) -> dict[str, Any]:
    root = new_run_root("stale-cas", counter)
    initialize_root(root)
    first = make_candidate("1.0.0", 100)
    second = make_candidate("1.1.0", 110)
    third = make_candidate("1.2.0", 120)
    try:
        install_initial(root, first, "cas-seed")
        preview_identity = state_identity(current_state(root))
        run_child(
            root,
            *child_mutation_args(
                action="update",
                scenario="cas-other-writer",
                candidate=second,
                expected_identity=preview_identity,
            ),
        )
        exact_expected_state(root, second)
        rejected = run_child(
            root,
            *child_mutation_args(
                action="update",
                scenario="cas-stale-writer",
                candidate=third,
                expected_identity=preview_identity,
            ),
            expected=(CONTROLLED_REJECT_EXIT,),
        )
        if "expected-previous CAS rejected" not in rejected.stderr:
            raise HarnessError("stale CAS scenario did not report the expected rejection")
        exact_expected_state(root, second)
        assert_no_transactions(root)
        assert_sentinels(root)
        return {"name": "expected-previous-cas", "status": "Pass"}
    finally:
        if not keep:
            safe_cleanup_run(root)


def self_test_concurrency(counter: int, keep: bool) -> dict[str, Any]:
    root = new_run_root("concurrent-writers", counter)
    initialize_root(root)
    first = make_candidate("1.0.0", 100)
    second = make_candidate("1.1.0", 110)
    third = make_candidate("1.2.0", 120)
    winner: subprocess.Popen[str] | None = None
    try:
        install_initial(root, first, "concurrency-seed")
        expected_identity = state_identity(current_state(root))
        winner = subprocess.Popen(
            child_command(
                root,
                *child_mutation_args(
                    action="update",
                    scenario="concurrency-winner",
                    candidate=second,
                    expected_identity=expected_identity,
                    hold_lock_stdin=True,
                ),
            ),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if winner.stdout is None or winner.stdin is None:
            raise HarnessError("controlled writer streams are unavailable")
        marker = winner.stdout.readline().strip()
        if marker != "CONTROLLED_LOCK_ACQUIRED":
            raise HarnessError(
                f"controlled writer did not acquire lock deterministically: {marker!r}"
            )

        contender = run_child(
            root,
            *child_mutation_args(
                action="update",
                scenario="concurrency-contender",
                candidate=third,
                expected_identity=expected_identity,
            ),
            expected=(CONTROLLED_REJECT_EXIT,),
        )
        if "authoritative lock" not in contender.stderr:
            raise HarnessError("concurrent writer was not rejected by mutation ownership")

        winner.stdin.write("\n")
        winner.stdin.flush()
        winner.stdin.close()
        winner.stdin = None
        winner_output = winner.stdout.read()
        winner_error = winner.stderr.read() if winner.stderr is not None else ""
        return_code = winner.wait(timeout=20)
        if return_code != 0:
            raise HarnessError(
                f"controlled mutation winner failed: rc={return_code} "
                f"out={winner_output!r} err={winner_error!r}"
            )
        exact_expected_state(root, second)
        assert_no_transactions(root)
        assert_sentinels(root)
        return {"name": "concurrent-writers", "status": "Pass", "winner_count": 1}
    finally:
        if winner is not None and winner.poll() is None:
            if winner.stdin is not None:
                try:
                    winner.stdin.write("\n")
                    winner.stdin.flush()
                except OSError:
                    pass
            try:
                winner.wait(timeout=5)
            except subprocess.TimeoutExpired:
                winner.kill()
                winner.wait(timeout=5)
        if not keep:
            safe_cleanup_run(root)


def only_transaction(root: pathlib.Path) -> pathlib.Path:
    entries = enumerate_transactions(root)
    if len(entries) != 1:
        raise HarnessError("corruption fixture expected exactly one interrupted transaction")
    return entries[0]


def make_prepared_interruption(
    root: pathlib.Path,
    scenario: str,
    *,
    staged: bool = False,
) -> tuple[Candidate, Candidate, pathlib.Path]:
    first = make_candidate("1.0.0", 100)
    second = make_candidate("1.1.0", 110)
    install_initial(root, first, scenario + "-seed")
    expected_identity = state_identity(current_state(root))
    fault = (
        "after-staging-before-verification"
        if staged
        else "after-snapshot-before-staging"
    )
    run_child(
        root,
        *child_mutation_args(
            action="update",
            scenario=scenario,
            candidate=second,
            expected_identity=expected_identity,
            fault=fault,
        ),
        expected=(CONTROLLED_INTERRUPT_EXIT,),
    )
    return first, second, only_transaction(root)


def self_test_corruptions(
    start_counter: int, keep: bool
) -> tuple[list[dict[str, Any]], int]:
    cases = (
        "malformed-journal",
        "truncated-journal",
        "unknown-phase",
        "duplicate-transaction-id",
        "missing-backup",
        "missing-stage",
        "altered-staged-hash",
        "altered-previous-state-hash",
        "unexpected-owned-file",
        "reparse-symlink-escape",
        "oversized-transaction-set",
    )
    results: list[dict[str, Any]] = []
    counter = start_counter
    for case in cases:
        root = new_run_root(f"corrupt-{case}", counter)
        counter += 1
        initialize_root(root)
        try:
            staged = case in {"altered-staged-hash", "unexpected-owned-file"}
            first, _second, tx_root = make_prepared_interruption(
                root, f"corrupt-{case}", staged=staged
            )
            before_identity = state_identity(current_state(root))

            if case == "malformed-journal":
                durable_write_bytes(journal_path(tx_root), b"{not-json\n")
            elif case == "truncated-journal":
                durable_write_bytes(journal_path(tx_root), b'{"schema_version":1')
            elif case == "unknown-phase":
                journal = json.loads(journal_path(tx_root).read_text(encoding="utf-8"))
                journal["phase"] = "UnknownControlledPhase"
                durable_write_json(journal_path(tx_root), journal)
            elif case == "duplicate-transaction-id":
                duplicate_name = hashlib.sha256(b"duplicate-entry").hexdigest()[:32]
                duplicate = transaction_root(root) / duplicate_name
                shutil.copytree(tx_root, duplicate)
            elif case == "missing-backup":
                safe_remove_tree(root, tx_root / "backup")
            elif case == "missing-stage":
                safe_remove_tree(root, tx_root / "stage")
            elif case == "altered-staged-hash":
                durable_write_bytes(
                    tx_root / "stage" / OWNED_FILES[0], b"altered staged bytes\n"
                )
            elif case == "altered-previous-state-hash":
                durable_write_bytes(
                    tx_root / "backup" / OWNED_FILES[0], b"altered previous bytes\n"
                )
            elif case == "unexpected-owned-file":
                durable_write_bytes(
                    tx_root / "stage" / "unexpected-owned.bin", b"unexpected\n"
                )
            elif case == "reparse-symlink-escape":
                safe_remove_tree(root, tx_root / "stage")
                target = root / "controlled-escape-target"
                target.mkdir()
                durable_write_bytes(target / "must-survive.txt", b"escape sentinel\n")
                try:
                    os.symlink(target, tx_root / "stage", target_is_directory=True)
                except (OSError, NotImplementedError) as exc:
                    raise HarnessError(
                        "controlled reparse/symlink fixture could not be created"
                    ) from exc
            elif case == "oversized-transaction-set":
                for index in range(MAX_TRANSACTIONS):
                    name = hashlib.sha256(
                        f"extra-{index}".encode("ascii")
                    ).hexdigest()[:32]
                    destination = transaction_root(root) / name
                    if destination.exists():
                        continue
                    shutil.copytree(tx_root, destination)
            else:
                raise HarnessError("unknown corruption case")

            rejected = run_child(root, "recover", expected=(CONTROLLED_REJECT_EXIT,))
            if not rejected.stderr.strip():
                raise HarnessError(
                    f"corruption case {case} did not emit a controlled rejection"
                )
            if state_identity(current_state(root)) != before_identity:
                raise HarnessError(
                    f"corruption case {case} changed committed state before rejection"
                )
            exact_expected_state(root, first)
            assert_sentinels(root)
            if case == "reparse-symlink-escape":
                sentinel = root / "controlled-escape-target" / "must-survive.txt"
                if not sentinel.is_file() or sentinel.read_bytes() != b"escape sentinel\n":
                    raise HarnessError(
                        "reparse rejection modified the controlled escape sentinel"
                    )
            results.append(
                {"name": case, "status": "Pass", "fail_closed": True}
            )
        finally:
            if not keep:
                safe_cleanup_run(root)
    return results, counter


def self_test_root_enforcement() -> dict[str, Any]:
    rejected = 0
    for candidate in (
        controlled_base(),
        controlled_base().parent / "HydraSeatInstallerAcceptance-unsafe",
        controlled_base() / "not-a-run-root",
    ):
        try:
            assert_safe_root(candidate)
        except HarnessError:
            rejected += 1
    if rejected != 3:
        raise HarnessError("controlled root enforcement accepted an unsafe root shape")
    return {"name": "safe-root-enforcement", "status": "Pass"}


def run_self_test(keep: bool) -> int:
    base = controlled_base()
    base.mkdir(parents=True, exist_ok=True)
    if is_reparse(base):
        raise HarnessError("controlled acceptance base is a reparse point")

    results: list[dict[str, Any]] = [self_test_root_enforcement()]
    counter = 1
    results.append(self_test_normal(counter, keep))
    counter += 1
    interruption_results, counter = self_test_interruptions(counter, keep)
    results.extend(interruption_results)
    results.append(self_test_cas(counter, keep))
    counter += 1
    results.append(self_test_concurrency(counter, keep))
    counter += 1
    corruption_results, counter = self_test_corruptions(counter, keep)
    results.extend(corruption_results)

    summary = {
        "schema_version": SCHEMA_VERSION,
        "evidence_class": CONTROLLED_EVIDENCE_CLASS,
        "evidence_note": "controlled process-interruption evidence",
        "root_policy": os.fspath(base / "run-*"),
        "scenario_count": len(results),
        "passed": sum(1 for result in results if result["status"] == "Pass"),
        "scenarios": results,
    }
    print(json.dumps(summary, sort_keys=True))
    return 0


def child_main(args: argparse.Namespace) -> int:
    root = assert_safe_root(pathlib.Path(args.root))
    try:
        if args.child_action == "recover":
            with MutationLock(root):
                count = recover_interrupted(root)
            print(
                json.dumps(
                    {
                        "evidence_class": CONTROLLED_EVIDENCE_CLASS,
                        "status": "Recovered",
                        "transaction_count": count,
                    },
                    sort_keys=True,
                )
            )
            return 0
        if args.child_action == "mutate":
            result = run_mutation(
                root,
                action=args.action,
                scenario=args.scenario,
                version=args.version,
                revision=args.revision,
                expected_previous_identity=args.expected_previous_identity,
                fault=args.fault,
                force_verify_failure=args.force_verify_failure,
                hold_lock_stdin=args.hold_lock_stdin,
            )
            print(json.dumps(result, sort_keys=True))
            return 0
        raise HarnessError("unknown controlled child action")
    except HarnessError as exc:
        print(
            json.dumps(
                {
                    "evidence_class": CONTROLLED_EVIDENCE_CLASS,
                    "status": "Rejected",
                    "message": str(exc),
                },
                sort_keys=True,
            ),
            file=sys.stderr,
        )
        return CONTROLLED_REJECT_EXIT


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run Controlled installer/update interruption and restart-recovery acceptance."
        )
    )
    parser.add_argument(
        "--self-test", action="store_true", help="run the complete Controlled campaign"
    )
    parser.add_argument(
        "--keep-root", action="store_true", help="retain Controlled temp roots for inspection"
    )
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--root", help=argparse.SUPPRESS)
    parser.add_argument(
        "child_action", nargs="?", choices=("mutate", "recover"), help=argparse.SUPPRESS
    )
    parser.add_argument(
        "--action", choices=("install", "repair", "update", "uninstall"), help=argparse.SUPPRESS
    )
    parser.add_argument("--scenario", help=argparse.SUPPRESS)
    parser.add_argument("--version", help=argparse.SUPPRESS)
    parser.add_argument("--revision", type=int, help=argparse.SUPPRESS)
    parser.add_argument("--expected-previous-identity", help=argparse.SUPPRESS)
    parser.add_argument("--fault", choices=FAULT_POINTS, help=argparse.SUPPRESS)
    parser.add_argument("--force-verify-failure", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--hold-lock-stdin", action="store_true", help=argparse.SUPPRESS)
    return parser


def main(argv: list[str]) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.child:
            if not args.root or not args.child_action:
                raise HarnessError(
                    "controlled child requires an explicit safe root and action"
                )
            return child_main(args)
        if args.self_test:
            return run_self_test(args.keep_root)
        build_parser().print_help()
        return 2
    except HarnessError as exc:
        print(
            json.dumps(
                {
                    "evidence_class": CONTROLLED_EVIDENCE_CLASS,
                    "status": "Rejected",
                    "message": str(exc),
                },
                sort_keys=True,
            ),
            file=sys.stderr,
        )
        return CONTROLLED_REJECT_EXIT


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
