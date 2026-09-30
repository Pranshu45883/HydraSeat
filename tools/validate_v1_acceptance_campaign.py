#!/usr/bin/env python3
"""Strict offline validator for HydraSeat v1 acceptance campaign evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tempfile
import time
from pathlib import Path

MAX_BYTES = 1024 * 1024
MAX_NOTE = 8192
MAX_EVIDENCE = 16
MAX_AGE = 7 * 24 * 60 * 60
STAGES = [
    "Preflight", "Phase3Physical", "DisplayReconnect", "DifferentGames",
    "SameTitle", "SeatIndependence", "ReturnToWindows", "InstallRepairUninstall",
    "RebootStartup", "UpdateRollback", "Offline", "FaultRecovery", "PerformanceSoak",
]
EVIDENCE_CLASS_BY_STAGE = {
    "Preflight": "Controlled", "Phase3Physical": "Physical", "DisplayReconnect": "Physical",
    "DifferentGames": "RealGame", "SameTitle": "RealGame", "SeatIndependence": "RealGame",
    "ReturnToWindows": "RealGame", "InstallRepairUninstall": "CleanMachineInstall",
    "RebootStartup": "CleanMachineInstall", "UpdateRollback": "CleanMachineInstall",
    "Offline": "Controlled", "FaultRecovery": "Manual", "PerformanceSoak": "RealGame",
}
PHYSICAL = {stage for stage, evidence_class in EVIDENCE_CLASS_BY_STAGE.items()
            if evidence_class != "Controlled"}
MANUAL = set(STAGES) - {"Preflight", "Offline"}
STATES = {"Pending", "Running", "AwaitingManualReview", "Passed", "Failed", "RecoveryRequired"}
ORIGINS = {"Synthetic", "ControlledProcess", "Physical"}
EVIDENCE_CLASSES = {"Synthetic", "Controlled", "Physical", "Manual", "RealGame",
                    "CleanMachineInstall", "SigningDeployment"}
VERDICTS = {"Pending", "Pass", "Fail"}
TOKEN = re.compile(r"^[A-Za-z0-9._:@+\-]{1,128}$")
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")


class ValidationError(ValueError):
    pass


def exact_keys(value: dict, expected: set[str], label: str) -> None:
    if set(value) != expected:
        raise ValidationError(f"{label} has unknown or missing fields")


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def integer(value, label: str, minimum: int = 0, maximum: int = (1 << 64) - 1) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValidationError(f"{label} is not a bounded integer")
    return value


def validate_document(document: dict, now: int | None = None) -> None:
    now = int(time.time()) if now is None else now
    if not isinstance(document, dict):
        raise ValidationError("campaign root must be an object")
    exact_keys(document, {"schema_version", "identity", "created_unix", "updated_unix", "stages"}, "campaign")
    if document["schema_version"] != 2:
        raise ValidationError("unsupported campaign schema")
    identity = document["identity"]
    if not isinstance(identity, dict):
        raise ValidationError("identity must be an object")
    exact_keys(identity, {"campaign_id", "rc_commit_sha", "release_artifact_sha256",
                          "release_artifact_name", "release_revision", "architecture",
                          "profile_sha256", "install_state_sha256", "windows_build",
                          "topology_fingerprint_sha256", "seat_ids", "scenario_identity",
                          "session_run_id"}, "identity")
    for key in ("campaign_id", "release_artifact_name", "windows_build", "scenario_identity",
                "session_run_id"):
        if not isinstance(identity[key], str) or not TOKEN.fullmatch(identity[key]):
            raise ValidationError(f"invalid identity field: {key}")
    if not isinstance(identity["rc_commit_sha"], str) or not HEX40.fullmatch(identity["rc_commit_sha"]):
        raise ValidationError("invalid RC commit")
    integer(identity["release_revision"], "release_revision", 1)
    if identity["architecture"] not in {"x64", "x86", "arm64"}:
        raise ValidationError("invalid release architecture")
    for key in ("release_artifact_sha256", "profile_sha256", "install_state_sha256",
                "topology_fingerprint_sha256"):
        if not isinstance(identity[key], str) or not HEX64.fullmatch(identity[key]):
            raise ValidationError(f"invalid identity hash: {key}")
    if identity["seat_ids"] not in ([1, 2], [2, 1]):
        raise ValidationError("campaign requires exactly unique Seat 1 and Seat 2")
    if identity["session_run_id"] != identity["campaign_id"]:
        raise ValidationError("campaign/session run identity must be one exact token")
    created = integer(document["created_unix"], "created_unix", 1)
    updated = integer(document["updated_unix"], "updated_unix", created)
    if updated > now + 300:
        raise ValidationError("campaign update time is implausibly in the future")
    stages = document["stages"]
    if not isinstance(stages, list) or len(stages) != len(STAGES):
        raise ValidationError("campaign must contain all 13 stages exactly once")
    evidence_ids: set[str] = set()
    evidence_artifacts: set[str] = set()
    test_names: set[str] = set()
    all_prior_passed = True
    for index, record in enumerate(stages):
        if not isinstance(record, dict):
            raise ValidationError("stage must be an object")
        exact_keys(record, {"stage", "state", "attempt", "started_unix", "completed_unix",
                            "evidence", "diagnostic"}, f"stage[{index}]")
        if record["stage"] != STAGES[index]:
            raise ValidationError("stages must be unique and in canonical order")
        state = record["state"]
        if state not in STATES:
            raise ValidationError("unknown stage state")
        attempt = integer(record["attempt"], "attempt", 0, (1 << 32) - 1)
        started = integer(record["started_unix"], "started_unix")
        completed = integer(record["completed_unix"], "completed_unix")
        if not isinstance(record["diagnostic"], str) or len(record["diagnostic"].encode()) > MAX_NOTE:
            raise ValidationError("stage diagnostic is oversized")
        if state == "Pending" and (attempt or started or completed or record["evidence"]):
            raise ValidationError("pending stage carries execution state")
        if state != "Pending" and (attempt == 0 or started == 0):
            raise ValidationError("started/imported stage lacks attempt or timestamp")
        # startStage() remains strictly sequential. Terminal/awaiting states may also
        # be imported later from already-produced exact RC receipts, so only a live
        # Running/RecoveryRequired stage requires every earlier stage to have passed.
        if state in {"Running", "RecoveryRequired"} and not all_prior_passed:
            raise ValidationError("active stage lacks passed prerequisites")
        if state in {"Passed", "Failed", "RecoveryRequired"} and completed < started:
            raise ValidationError("terminal stage completion time is invalid")
        evidence = record["evidence"]
        if not isinstance(evidence, list) or len(evidence) > MAX_EVIDENCE:
            raise ValidationError("stage evidence count is invalid")
        if state in {"Running", "RecoveryRequired"} and evidence:
            raise ValidationError("running/recovery stage retains completed evidence")
        for item in evidence:
            if not isinstance(item, dict):
                raise ValidationError("evidence must be an object")
            exact_keys(item, {"schema_version", "evidence_id", "stage", "origin", "created_unix",
                              "content_sha256", "evidence_artifact_name", "test_name",
                              "rc_commit_sha", "release_artifact_sha256", "release_revision",
                              "architecture", "profile_sha256", "install_state_sha256",
                              "scenario_identity", "automated_passed", "human_verdict", "note",
                              "campaign_schema_version", "campaign_id", "session_run_id",
                              "release_artifact_name", "windows_build", "topology_fingerprint_sha256",
                              "evidence_class"}, "evidence")
            if item["schema_version"] != 2 or item["campaign_schema_version"] != 2 or \
               item["stage"] != record["stage"]:
                raise ValidationError("evidence campaign/schema/stage mismatch")
            for key in ("evidence_id", "evidence_artifact_name", "test_name"):
                if not isinstance(item[key], str) or not TOKEN.fullmatch(item[key]):
                    raise ValidationError(f"invalid evidence identity: {key}")
            if item["evidence_id"] in evidence_ids or \
               item["evidence_artifact_name"] in evidence_artifacts or \
               item["test_name"] in test_names:
                raise ValidationError("duplicate evidence ID, artifact, or test identity")
            evidence_ids.add(item["evidence_id"])
            evidence_artifacts.add(item["evidence_artifact_name"])
            test_names.add(item["test_name"])
            if item["origin"] not in ORIGINS or item["evidence_class"] not in EVIDENCE_CLASSES or \
               item["human_verdict"] not in VERDICTS:
                raise ValidationError("invalid evidence origin/class/verdict")
            expected_class = EVIDENCE_CLASS_BY_STAGE[record["stage"]]
            expected_origin = "ControlledProcess" if expected_class == "Controlled" else "Physical"
            if item["evidence_class"] != expected_class or item["origin"] != expected_origin:
                raise ValidationError("evidence class/origin cannot satisfy this campaign stage")
            when = integer(item["created_unix"], "evidence created_unix", 1)
            if when > now + 300 or now - min(now, when) > MAX_AGE:
                raise ValidationError("stale/future evidence")
            if not isinstance(item["content_sha256"], str) or not HEX64.fullmatch(item["content_sha256"]):
                raise ValidationError("invalid evidence hash")
            integer(item["release_revision"], "evidence release_revision", 1)
            if item["architecture"] not in {"x64", "x86", "arm64"}:
                raise ValidationError("invalid evidence architecture")
            for key in ("profile_sha256", "install_state_sha256", "topology_fingerprint_sha256"):
                if not isinstance(item[key], str) or not HEX64.fullmatch(item[key]):
                    raise ValidationError(f"invalid evidence binding hash: {key}")
            for key in ("campaign_id", "session_run_id", "release_artifact_name", "windows_build"):
                if not isinstance(item[key], str) or not TOKEN.fullmatch(item[key]):
                    raise ValidationError(f"invalid evidence binding identity: {key}")
            if item["campaign_id"] != identity["campaign_id"] or \
               item["session_run_id"] != identity["session_run_id"] or \
               item["rc_commit_sha"] != identity["rc_commit_sha"] or \
               item["release_artifact_sha256"] != identity["release_artifact_sha256"] or \
               item["release_artifact_name"] != identity["release_artifact_name"] or \
               item["release_revision"] != identity["release_revision"] or \
               item["architecture"] != identity["architecture"] or \
               item["profile_sha256"] != identity["profile_sha256"] or \
               item["install_state_sha256"] != identity["install_state_sha256"] or \
               item["windows_build"] != identity["windows_build"] or \
               item["topology_fingerprint_sha256"] != identity["topology_fingerprint_sha256"] or \
               item["scenario_identity"] != identity["scenario_identity"]:
                raise ValidationError("evidence is bound to another campaign/session RC/build/profile/scenario/input artifact set")
            if type(item["automated_passed"]) is not bool:
                raise ValidationError("automated_passed must be boolean")
            if not isinstance(item["note"], str) or len(item["note"].encode()) > MAX_NOTE:
                raise ValidationError("evidence note is oversized")
            if record["stage"] in PHYSICAL and item["origin"] != "Physical":
                raise ValidationError("non-physical evidence is attached to a physical stage")
        if state == "AwaitingManualReview":
            if record["stage"] not in MANUAL or not evidence or \
               not all(item["automated_passed"] and item["human_verdict"] == "Pending" for item in evidence):
                raise ValidationError("manual-review stage lacks passing PENDING evidence")
        if state == "Passed" and (not evidence or not all(item["automated_passed"] for item in evidence)):
            raise ValidationError("passed stage lacks passing automated evidence")
        if state == "Passed" and record["stage"] in MANUAL and \
           not all(item["human_verdict"] == "Pass" for item in evidence):
            raise ValidationError("passed manual stage still has PENDING/failed human verdict")
        if state == "Passed" and record["stage"] in PHYSICAL and \
           not any(item["origin"] == "Physical" for item in evidence):
            raise ValidationError("passed physical stage lacks physical evidence")
        all_prior_passed = all_prior_passed and state == "Passed"


def sha256_bounded_file(path: Path, label: str) -> str:
    if path.is_symlink() or not path.is_file():
        raise ValidationError(f"{label} is missing, not regular, or is a symlink")
    size = path.stat().st_size
    if size <= 0 or size > MAX_BYTES:
        raise ValidationError(f"{label} is empty or oversized")
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(65536):
            digest.update(chunk)
    return digest.hexdigest()


def validate_artifact_binding(path: Path, item: dict, identity: dict, now: int) -> None:
    try:
        with path.open("r", encoding="utf-8") as handle:
            document = json.load(handle, object_pairs_hook=no_duplicates)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"evidence artifact is not strict bounded JSON: {path.name}: {exc}") from exc
    if not isinstance(document, dict):
        raise ValidationError(f"evidence artifact root is not an object: {path.name}")

    stage = item["stage"]
    if stage == "Phase3Physical":
        try:
            if document["schema_version"] != 1 or document["state"] != "MANUAL_PASS" or \
               document["manual_verdict"] != "PASS" or \
               document["environment"]["architecture"] != identity["architecture"] or \
               document["environment"]["windows_build"] != identity["windows_build"] or \
               document["profile"]["sha256"] != identity["profile_sha256"] or \
               document["session_id"] != identity["session_run_id"]:
                raise ValidationError("Phase 3 artifact is foreign, non-physical, or bound to another session")
        except (KeyError, TypeError) as exc:
            raise ValidationError("Phase 3 artifact is partial or malformed") from exc
        return

    binding = document.get("release_binding")
    if binding is None:
        if stage != "Preflight":
            raise ValidationError(f"{stage} evidence artifact lacks exact release_binding")
        required = {"schema_version", "commit_sha", "release_revision", "architecture",
                    "install_state_sha256", "all_owned_files_verified"}
        if not required.issubset(document) or document["schema_version"] != 1 or \
           document["commit_sha"] != identity["rc_commit_sha"] or \
           document["release_revision"] != identity["release_revision"] or \
           document["architecture"] != identity["architecture"] or \
           document["install_state_sha256"] != identity["install_state_sha256"] or \
           document["all_owned_files_verified"] is not True:
            raise ValidationError("generated Preflight probe is not bound to the exact installed RC")
        return

    if not isinstance(binding, dict):
        raise ValidationError("evidence release_binding must be an object")
    expected_keys = {
        "schema_version", "campaign_schema_version", "campaign_id", "session_run_id",
        "stage", "test_name", "origin", "evidence_class", "created_unix", "rc_commit_sha",
        "release_artifact_sha256", "release_artifact_name", "release_revision", "architecture",
        "profile_sha256", "install_state_sha256", "windows_build", "topology_fingerprint_sha256",
        "scenario_identity", "automated_passed",
    }
    exact_keys(binding, expected_keys, "evidence release_binding")
    expected_class = EVIDENCE_CLASS_BY_STAGE[stage]
    expected_origin = "ControlledProcess" if expected_class == "Controlled" else "Physical"
    if binding["schema_version"] != 2 or binding["campaign_schema_version"] != 2 or \
       binding["campaign_id"] != identity["campaign_id"] or \
       binding["session_run_id"] != identity["session_run_id"] or \
       binding["stage"] != stage or binding["test_name"] != item["test_name"] or \
       binding["origin"] != expected_origin or binding["evidence_class"] != expected_class or \
       binding["rc_commit_sha"] != identity["rc_commit_sha"] or \
       binding["release_artifact_sha256"] != identity["release_artifact_sha256"] or \
       binding["release_artifact_name"] != identity["release_artifact_name"] or \
       binding["release_revision"] != identity["release_revision"] or \
       binding["architecture"] != identity["architecture"] or \
       binding["profile_sha256"] != identity["profile_sha256"] or \
       binding["install_state_sha256"] != identity["install_state_sha256"] or \
       binding["windows_build"] != identity["windows_build"] or \
       binding["topology_fingerprint_sha256"] != identity["topology_fingerprint_sha256"] or \
       binding["scenario_identity"] != identity["scenario_identity"] or \
       binding["automated_passed"] is not item["automated_passed"]:
        raise ValidationError("evidence artifact release_binding is foreign or inconsistent with campaign")
    created = integer(binding["created_unix"], "artifact binding created_unix", 1)
    if created != item["created_unix"] or created > now + 300 or now - min(now, created) > MAX_AGE:
        raise ValidationError("evidence artifact release_binding timestamp is stale or mismatched")


def validate_evidence_artifacts(campaign_path: Path, document: dict, now: int) -> None:
    artifacts_root = campaign_path.parent / "artifacts"
    for record in document["stages"]:
        for item in record["evidence"]:
            name = item["evidence_artifact_name"]
            if Path(name).name != name or name in {".", ".."}:
                raise ValidationError("evidence artifact name is not a bounded leaf name")
            artifact = artifacts_root / name
            actual = sha256_bounded_file(artifact, f"evidence artifact {name}")
            if actual != item["content_sha256"]:
                raise ValidationError(f"evidence artifact hash mismatch: {name}")
            validate_artifact_binding(artifact, item, document["identity"], now)


def validate_file(path: Path, now: int | None = None) -> None:
    if path.is_symlink() or not path.is_file():
        raise ValidationError("campaign file is missing, not regular, or is a symlink")
    staged = Path(str(path) + ".new")
    if staged.exists() or staged.is_symlink():
        raise ValidationError("stale campaign staging file exists")
    size = path.stat().st_size
    if size <= 0 or size > MAX_BYTES:
        raise ValidationError("campaign file is empty or oversized")
    with path.open("r", encoding="utf-8") as handle:
        document = json.load(handle, object_pairs_hook=no_duplicates)
    effective_now = int(time.time()) if now is None else now
    validate_document(document, effective_now)
    identity = document["identity"]
    if path.parent.name != identity["session_run_id"] or \
       identity["campaign_id"] != identity["session_run_id"]:
        raise ValidationError("campaign file is outside its exact session/run directory")
    validate_evidence_artifacts(path, document, effective_now)


def self_test() -> None:
    now = 2_000_000_000
    identity = {
        "campaign_id": "selftest", "rc_commit_sha": "a" * 40,
        "release_artifact_sha256": "b" * 64, "release_artifact_name": "HydraSeat-x64.zip",
        "release_revision": 7, "architecture": "x64", "profile_sha256": "c" * 64,
        "install_state_sha256": "d" * 64, "windows_build": "10.0.26100",
        "topology_fingerprint_sha256": "e" * 64, "seat_ids": [1, 2],
        "scenario_identity": "selftest-scenario", "session_run_id": "selftest",
    }
    stages = []
    for name in STAGES:
        stages.append({"stage": name, "state": "Pending", "attempt": 0,
                       "started_unix": 0, "completed_unix": 0,
                       "evidence": [], "diagnostic": ""})
    valid = {"schema_version": 2, "identity": identity, "created_unix": now,
             "updated_unix": now, "stages": stages}
    validate_document(valid, now)
    bad = json.loads(json.dumps(valid))
    bad["identity"]["seat_ids"] = [1, 3]
    try:
        validate_document(bad, now)
        raise AssertionError("third Seat was accepted")
    except ValidationError:
        pass
    bad = json.loads(json.dumps(valid))
    bad["stages"][0].update({"state": "Passed", "attempt": 1,
                              "started_unix": now, "completed_unix": now,
                              "evidence": []})
    try:
        validate_document(bad, now)
        raise AssertionError("evidence-free pass was accepted")
    except ValidationError:
        pass

    probe_document = {
        "schema_version": 1,
        "release_revision": identity["release_revision"],
        "commit_sha": identity["rc_commit_sha"],
        "architecture": identity["architecture"],
        "install_state_sha256": identity["install_state_sha256"],
        "all_owned_files_verified": True,
    }
    evidence_bytes = (json.dumps(probe_document, separators=(",", ":"), sort_keys=True) + "\n").encode()
    artifact_name = "Preflight-selftest.json"
    evidence_item = {
        "schema_version": 2, "evidence_id": "selftest-preflight",
        "stage": "Preflight", "origin": "ControlledProcess", "created_unix": now,
        "content_sha256": hashlib.sha256(evidence_bytes).hexdigest(),
        "evidence_artifact_name": artifact_name, "test_name": "selftest-preflight-test",
        "rc_commit_sha": identity["rc_commit_sha"],
        "release_artifact_sha256": identity["release_artifact_sha256"],
        "release_revision": identity["release_revision"],
        "architecture": identity["architecture"], "profile_sha256": identity["profile_sha256"],
        "install_state_sha256": identity["install_state_sha256"],
        "scenario_identity": identity["scenario_identity"],
        "automated_passed": True, "human_verdict": "Pending", "note": "selftest",
        "campaign_schema_version": 2, "campaign_id": identity["campaign_id"],
        "session_run_id": identity["session_run_id"],
        "release_artifact_name": identity["release_artifact_name"],
        "windows_build": identity["windows_build"],
        "topology_fingerprint_sha256": identity["topology_fingerprint_sha256"],
        "evidence_class": "Controlled",
    }
    file_valid = json.loads(json.dumps(valid))
    file_valid["stages"][0].update({
        "state": "Passed", "attempt": 1, "started_unix": now,
        "completed_unix": now, "diagnostic": "", "evidence": [evidence_item],
    })
    validate_document(file_valid, now)

    # The runner still starts stages sequentially, but the RC orchestrator may later
    # ingest already-produced terminal receipts in any deterministic order. Terminal
    # evidence is therefore representable without pretending that a live execution
    # skipped its prerequisites.
    out_of_order = json.loads(json.dumps(valid))
    offline_evidence = json.loads(json.dumps(evidence_item))
    offline_evidence.update({
        "evidence_id": "selftest-offline",
        "stage": "Offline",
        "evidence_artifact_name": "Offline-selftest.json",
        "test_name": "selftest-offline-test",
    })
    out_of_order["stages"][10].update({
        "state": "Passed", "attempt": 1, "started_unix": now,
        "completed_unix": now, "diagnostic": "", "evidence": [offline_evidence],
    })
    validate_document(out_of_order, now)

    active_out_of_order = json.loads(json.dumps(valid))
    active_out_of_order["stages"][10].update({
        "state": "Running", "attempt": 1, "started_unix": now,
        "completed_unix": 0, "diagnostic": "", "evidence": [],
    })
    try:
        validate_document(active_out_of_order, now)
        raise AssertionError("live out-of-order stage execution was accepted")
    except ValidationError:
        pass

    for label, mutate in (
        ("foreign session", lambda item: item.__setitem__("session_run_id", "foreign-session")),
        ("wrong profile", lambda item: item.__setitem__("profile_sha256", "f" * 64)),
        ("controlled labelled physical", lambda item: item.__setitem__("evidence_class", "Physical")),
        ("stale evidence", lambda item: item.__setitem__("created_unix", now - MAX_AGE - 1)),
    ):
        bad_binding = json.loads(json.dumps(file_valid))
        mutate(bad_binding["stages"][0]["evidence"][0])
        try:
            validate_document(bad_binding, now)
            raise AssertionError(f"{label} was accepted")
        except ValidationError:
            pass

    with tempfile.TemporaryDirectory() as root_text:
        root = Path(root_text) / identity["session_run_id"]
        root.mkdir()
        artifacts = root / "artifacts"
        artifacts.mkdir()
        artifact = artifacts / artifact_name
        artifact.write_bytes(evidence_bytes)
        campaign = root / "v1-acceptance-campaign.json"
        campaign.write_text(json.dumps(file_valid, separators=(",", ":")), encoding="utf-8")
        validate_file(campaign, now)

        foreign_root = root.parent / "foreign-session"
        foreign_root.mkdir()
        foreign_campaign = foreign_root / campaign.name
        foreign_campaign.write_bytes(campaign.read_bytes())
        try:
            validate_file(foreign_campaign, now)
            raise AssertionError("foreign campaign session directory was accepted")
        except ValidationError:
            pass

        artifact.write_bytes(b"altered-evidence\n")
        try:
            validate_file(campaign, now)
            raise AssertionError("altered evidence artifact was accepted")
        except ValidationError:
            pass
        artifact.unlink()
        try:
            validate_file(campaign, now)
            raise AssertionError("missing evidence artifact was accepted")
        except ValidationError:
            pass
        artifact.write_bytes(evidence_bytes)
        Path(str(campaign) + ".new").write_text("stale-sensitive-staging", encoding="utf-8")
        try:
            validate_file(campaign, now)
            raise AssertionError("stale campaign staging file was accepted")
        except ValidationError:
            pass

    runner = Path(__file__).with_name("run_v1_acceptance_campaign.ps1").read_text(encoding="utf-8")
    for forbidden in ("Invoke-Expression", "cmd.exe", "powershell.exe -Command"):
        if forbidden.lower() in runner.lower():
            raise AssertionError(f"runner contains forbidden general execution surface: {forbidden}")
    for expected in STAGES:
        if expected not in runner:
            raise AssertionError(f"runner omits fixed stage: {expected}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            self_test()
            print("V1 acceptance campaign validator self-test passed.")
        if args.campaign:
            validate_file(args.campaign)
            print(f"V1 acceptance campaign valid: {args.campaign}")
        if not args.self_test and not args.campaign:
            parser.error("campaign or --self-test is required")
    except (OSError, json.JSONDecodeError, ValidationError, AssertionError) as error:
        print(f"validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
