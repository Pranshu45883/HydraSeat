#!/usr/bin/env python3
"""Deterministic HydraSeat release artifact manifest validation and self-test.

This module owns artifact inventory/checksum/SBOM/provenance integrity only. It
never signs artifacts and never turns controlled/developer evidence into release
qualification. Production signature authority remains the existing P8 signing
and installer validation pipeline.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Callable


ROOT = Path(__file__).resolve().parents[1]
POLICY_PATH = ROOT / "config" / "release-artifact-preflight.json"
MANIFEST_NAME = "release-artifact-manifest.json"
CHECKSUMS_NAME = "SHA256SUMS"
SBOM_NAME = "release-sbom.json"
PROVENANCE_NAME = "release-provenance.json"
MAX_JSON_BYTES = 1024 * 1024
MAX_CACHE_BYTES = 4 * 1024 * 1024
MAX_GIT_OUTPUT = 1024 * 1024
REPARSE_POINT_ATTRIBUTE = 0x400
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
TOKEN = re.compile(r"^[A-Za-z0-9._+@-]{1,128}$")
VERSION = re.compile(r"^[A-Za-z0-9._+-]{1,64}$")
SAFE_FILE = re.compile(r"^[A-Za-z0-9._+-]{1,128}$")
SAFE_TARGET = re.compile(r"^[A-Za-z0-9_]{1,96}$")
COMPONENT_ROLES = {
    "ApplicationUI",
    "AuthoritativeHost",
    "SeatUI",
    "RecoveryWatchdog",
    "RecoveryReset",
    "ProfileCLI",
    "CommunityValidator",
    "Installer",
    "ConfigurationData",
    "RuntimeComponent",
}
SIGNING_STATES = {
    "UnsignedControlled",
    "SigningExpectedAbsent",
    "SignedUnverified",
    "ProductionSignatureVerified",
}
QUALIFICATION_MODES = {"Controlled", "ReleaseCandidate"}
INPUT_MODES = {"BuildRoot", "PackageRoot"}
ALLOWED_PACKAGE_METADATA = {"signing-provenance.json", "signing-provenance.json.p7s"}
POLICY_KEYS = {
    "schemaVersion", "product", "architecture", "releaseSigningManifestPath",
    "releaseScopePath", "legal", "limits", "artifacts", "thirdPartyRedistributables",
}
POLICY_LEGAL_KEYS = {"projectLicenseState", "distributionNoticesState"}
POLICY_LIMIT_KEYS = {"maximumArtifactBytes", "maximumManifestBytes", "maximumPackagedFiles"}
POLICY_ARTIFACT_KEYS = {"id", "componentRole", "packagePath"}
SIGNING_MANIFEST_KEYS = {"schemaVersion", "releaseScope", "artifacts", "excludedArtifactClasses"}
MANIFEST_KEYS = {
    "schemaVersion", "kind", "product", "release", "source", "reviewedInputs",
    "legal", "artifacts", "thirdPartyRedistributables", "integrity",
}
RELEASE_KEYS = {"version", "revision", "architecture", "qualificationMode"}
SOURCE_KEYS = {
    "commitSha", "checkoutHeadSha", "treeState", "buildConfiguration",
    "inputMode", "buildRootSourceBound", "buildReproducibility",
}
REVIEWED_INPUT_KEYS = {
    "preflightPolicyPath", "preflightPolicySha256", "releaseSigningManifestPath",
    "releaseSigningManifestSha256", "releaseScopePath", "releaseScopeSha256",
}
LEGAL_KEYS = {"projectLicenseState", "distributionNoticesState"}
ARTIFACT_KEYS = {
    "id", "fileName", "kind", "componentRole", "architecture", "packagePath",
    "sourceLocator", "bytes", "sha256", "sourceCommitSha", "signingState",
    "signingExpected", "signerThumbprint", "signingProvenanceSha256",
    "provenanceState",
}
INTEGRITY_KEYS = {
    "artifactBytesVerified", "manifestDeterministic", "sourceProvenanceBound",
    "packageFileSetVerified", "buildReproducibilityProven", "artifactPreflightState",
    "artifactPreflightQualificationState", "qualificationBlockers",
}
PROVENANCE_KEYS = {
    "schemaVersion", "artifactManifest", "artifactManifestSha256", "product",
    "releaseVersion", "releaseRevision", "architecture", "sourceCommitSha",
    "sourceTreeState", "buildConfiguration", "inputMode", "releaseScopeSha256",
    "releaseSigningManifestSha256", "preflightPolicySha256", "artifactBytesVerified",
    "manifestDeterministic", "sourceProvenanceBound", "packageFileSetVerified",
    "buildReproducibility", "productionSignatureVerification", "artifactPreflightState",
    "artifactPreflightQualificationState", "qualificationBlockers",
}


class ArtifactManifestError(ValueError):
    pass


class IncompletePreflight(ArtifactManifestError):
    def __init__(self, report: dict):
        super().__init__("release artifact input set is incomplete")
        self.report = report


def _reject_constant(value: str):
    raise ArtifactManifestError(f"non-finite JSON constant is forbidden: {value}")


def _no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ArtifactManifestError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _canonical_json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _exact_keys(value: dict, expected: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        raise ArtifactManifestError(f"{label} has unknown or missing fields")


def _integer(value: object, label: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ArtifactManifestError(f"{label} is not a bounded integer")
    return value


def _is_reparse(path: Path) -> bool:
    try:
        stat = path.lstat()
    except FileNotFoundError:
        return False
    return path.is_symlink() or bool(getattr(stat, "st_file_attributes", 0) & REPARSE_POINT_ATTRIBUTE)


def _assert_directory(path: Path, label: str) -> None:
    if not path.is_dir() or _is_reparse(path):
        raise ArtifactManifestError(f"{label} must be a non-reparse directory")


def _assert_regular(path: Path, label: str, maximum: int) -> int:
    if not path.is_file() or _is_reparse(path):
        raise ArtifactManifestError(f"{label} must be a non-reparse regular file")
    size = path.stat().st_size
    if size <= 0 or size > maximum:
        raise ArtifactManifestError(f"{label} is empty or exceeds its bounded size")
    return size


def _read_bounded(path: Path, label: str, maximum: int) -> bytes:
    size = _assert_regular(path, label, maximum)
    with path.open("rb") as handle:
        data = handle.read(maximum + 1)
    if len(data) != size:
        raise ArtifactManifestError(f"{label} changed during bounded read")
    return data


def _load_json(path: Path, label: str, maximum: int = MAX_JSON_BYTES) -> tuple[dict, bytes]:
    data = _read_bounded(path, label, maximum)
    try:
        value = json.loads(
            data.decode("utf-8", errors="strict"),
            object_pairs_hook=_no_duplicates,
            parse_constant=_reject_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ArtifactManifestError(f"{label} is not strict bounded UTF-8 JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ArtifactManifestError(f"{label} root must be an object")
    return value, data


def _sha256_file(path: Path, label: str, maximum: int) -> tuple[str, int]:
    expected_size = _assert_regular(path, label, maximum)
    digest = hashlib.sha256()
    observed = 0
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            observed += len(chunk)
            if observed > maximum:
                raise ArtifactManifestError(f"{label} grew beyond its maximum during hashing")
            digest.update(chunk)
    if observed != expected_size:
        raise ArtifactManifestError(f"{label} changed while being hashed")
    return digest.hexdigest(), observed


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    _assert_directory(path.parent, f"output directory {path.parent.name}")
    temporary = Path(str(path) + ".new")
    if temporary.exists() or _is_reparse(temporary):
        if _is_reparse(temporary) or not temporary.is_file():
            raise ArtifactManifestError(f"unsafe stale staging path: {temporary.name}")
        temporary.unlink()
    with temporary.open("wb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def _safe_repo_relative(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or "\\" in value:
        raise ArtifactManifestError(f"{label} must be one portable repository-relative path")
    pure = PurePosixPath(value)
    if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
        raise ArtifactManifestError(f"{label} contains absolute path or traversal")
    if re.match(r"^[A-Za-z]:", value):
        raise ArtifactManifestError(f"{label} contains an absolute Windows path")
    return value


def _safe_package_path(value: object, label: str) -> str:
    value = _safe_repo_relative(value, label)
    pure = PurePosixPath(value)
    if len(pure.parts) != 2 or pure.parts[0] != "x64" or not SAFE_FILE.fullmatch(pure.parts[1]):
        raise ArtifactManifestError(f"{label} must be exactly x64/<portable-leaf>")
    return value


def _safe_token(value: object, label: str) -> str:
    if not isinstance(value, str) or not TOKEN.fullmatch(value):
        raise ArtifactManifestError(f"{label} is not a bounded token")
    return value


def _safe_version(value: object, label: str) -> str:
    if not isinstance(value, str) or not VERSION.fullmatch(value):
        raise ArtifactManifestError(f"{label} is invalid")
    return value


def _safe_hex(value: object, length: int, label: str) -> str:
    pattern = HEX40 if length == 40 else HEX64
    if not isinstance(value, str) or not pattern.fullmatch(value):
        raise ArtifactManifestError(f"{label} must be exactly {length} lowercase hexadecimal characters")
    return value


def _resolve_under(root: Path, relative: str, label: str) -> Path:
    relative = _safe_repo_relative(relative, label)
    candidate = root.joinpath(*PurePosixPath(relative).parts)
    try:
        candidate_abs = candidate.resolve(strict=False)
        root_abs = root.resolve(strict=True)
    except OSError as exc:
        raise ArtifactManifestError(f"cannot resolve {label}: {exc}") from exc
    try:
        candidate_abs.relative_to(root_abs)
    except ValueError as exc:
        raise ArtifactManifestError(f"{label} escapes its authoritative root") from exc
    return candidate


def _validate_policy(policy: dict, repo_root: Path) -> tuple[dict, dict]:
    _exact_keys(policy, POLICY_KEYS, "artifact preflight policy")
    if policy["schemaVersion"] != 1 or policy["product"] != "HydraSeat" or policy["architecture"] != "x64":
        raise ArtifactManifestError("artifact preflight policy product/schema/architecture is not reviewed v1 x64")
    _exact_keys(policy["legal"], POLICY_LEGAL_KEYS, "artifact preflight legal policy")
    if policy["legal"]["projectLicenseState"] not in {"Unresolved", "Resolved"} or \
       policy["legal"]["distributionNoticesState"] not in {"Unresolved", "Resolved"}:
        raise ArtifactManifestError("artifact preflight legal state is unknown")
    _exact_keys(policy["limits"], POLICY_LIMIT_KEYS, "artifact preflight limits")
    max_artifact = _integer(policy["limits"]["maximumArtifactBytes"], "maximumArtifactBytes", 1, 8 * 1024**3)
    max_manifest = _integer(policy["limits"]["maximumManifestBytes"], "maximumManifestBytes", 4096, MAX_JSON_BYTES)
    max_files = _integer(policy["limits"]["maximumPackagedFiles"], "maximumPackagedFiles", 1, 64)
    _ = max_artifact, max_manifest
    signing_path = _safe_repo_relative(policy["releaseSigningManifestPath"], "release signing manifest path")
    scope_path = _safe_repo_relative(policy["releaseScopePath"], "release scope path")
    if signing_path != "config/release-signing-manifest.json" or scope_path != "config/release-scope-v1.json":
        raise ArtifactManifestError("artifact preflight policy must bind the reviewed release config files")
    artifacts = policy["artifacts"]
    if not isinstance(artifacts, list) or not artifacts or len(artifacts) > max_files:
        raise ArtifactManifestError("artifact preflight policy artifact count is invalid")
    seen_ids: set[str] = set()
    seen_paths: set[str] = set()
    seen_casefold: set[str] = set()
    roles_by_id: dict[str, tuple[str, str]] = {}
    for item in artifacts:
        _exact_keys(item, POLICY_ARTIFACT_KEYS, "artifact preflight policy item")
        artifact_id = _safe_token(item["id"], "artifact policy id")
        role = item["componentRole"]
        package_path = _safe_package_path(item["packagePath"], "artifact package path")
        folded = package_path.casefold()
        if artifact_id in seen_ids:
            raise ArtifactManifestError("artifact preflight policy contains duplicate artifact id")
        if package_path in seen_paths:
            raise ArtifactManifestError("artifact preflight policy contains duplicate package path")
        if folded in seen_casefold:
            raise ArtifactManifestError("artifact preflight policy contains Windows case-fold filename collision")
        if role not in COMPONENT_ROLES:
            raise ArtifactManifestError(f"artifact preflight policy contains unknown component role: {role!r}")
        seen_ids.add(artifact_id)
        seen_paths.add(package_path)
        seen_casefold.add(folded)
        roles_by_id[artifact_id] = (role, package_path)
    third_party = policy["thirdPartyRedistributables"]
    if not isinstance(third_party, list):
        raise ArtifactManifestError("thirdPartyRedistributables must be an array")
    if third_party:
        raise ArtifactManifestError(
            "current reviewed signing scope declares no third-party redistributables; update release policy explicitly before shipping one"
        )
    signing_doc, _ = _load_json(repo_root / signing_path, "reviewed release signing manifest")
    _exact_keys(signing_doc, SIGNING_MANIFEST_KEYS, "reviewed release signing manifest")
    if signing_doc["schemaVersion"] != 1:
        raise ArtifactManifestError("unsupported release signing manifest schema")
    signing_artifacts = signing_doc["artifacts"]
    if not isinstance(signing_artifacts, list) or len(signing_artifacts) != len(artifacts):
        raise ArtifactManifestError("preflight policy must match the exact signing artifact set")
    seen_signing_ids: set[str] = set()
    seen_file_names: set[str] = set()
    seen_file_casefold: set[str] = set()
    for signing in signing_artifacts:
        if not isinstance(signing, dict):
            raise ArtifactManifestError("release signing artifact must be an object")
        artifact_id = _safe_token(signing.get("id"), "release signing artifact id")
        if artifact_id in seen_signing_ids or artifact_id not in roles_by_id:
            raise ArtifactManifestError("release signing manifest contains duplicate/unreviewed artifact id")
        seen_signing_ids.add(artifact_id)
        kind = signing.get("kind")
        file_name = signing.get("fileName")
        if not isinstance(file_name, str) or not SAFE_FILE.fullmatch(file_name):
            raise ArtifactManifestError("release signing artifact fileName is unsafe")
        folded = file_name.casefold()
        if file_name in seen_file_names or folded in seen_file_casefold:
            raise ArtifactManifestError("release signing manifest contains duplicate/case-fold filename collision")
        seen_file_names.add(file_name)
        seen_file_casefold.add(folded)
        if signing.get("architectures") != ["x64"]:
            raise ArtifactManifestError("release signing artifacts must target exact x64 host architecture")
        role, package_path = roles_by_id[artifact_id]
        _ = role
        if PurePosixPath(package_path).name != file_name:
            raise ArtifactManifestError("preflight packagePath basename differs from signing manifest fileName")
        if kind == "cmake-executable":
            target = signing.get("target")
            if not isinstance(target, str) or not SAFE_TARGET.fullmatch(target):
                raise ArtifactManifestError("release executable has invalid CMake target")
            if set(signing) != {"id", "kind", "target", "fileName", "architectures"}:
                raise ArtifactManifestError("release executable signing record has unknown/missing fields")
        elif kind == "powershell-script":
            source_path = signing.get("sourcePath")
            if set(signing) != {"id", "kind", "sourcePath", "fileName", "architectures"} or \
               source_path != "tools/install_hydraseat.ps1":
                raise ArtifactManifestError("only the reviewed installer PowerShell source may enter signing scope")
        else:
            raise ArtifactManifestError("release signing manifest contains unsupported artifact kind")
    if seen_signing_ids != seen_ids:
        raise ArtifactManifestError("preflight/signing artifact identity sets differ")
    scope_doc, _ = _load_json(repo_root / scope_path, "reviewed release scope")
    if scope_doc.get("schemaVersion") != 1 or scope_doc.get("productVersion") != "v1" or \
       scope_doc.get("hostPlatform", {}).get("hostArchitecture") != "x64":
        raise ArtifactManifestError("release scope does not bind the reviewed v1 x64 host")
    return signing_doc, scope_doc


def load_reviewed_inputs(repo_root: Path = ROOT, policy_path: Path | None = None) -> dict:
    repo_root = repo_root.resolve()
    if policy_path is None:
        policy_path = repo_root / "config" / "release-artifact-preflight.json"
    policy_doc, policy_bytes = _load_json(policy_path, "artifact preflight policy")
    signing_doc, scope_doc = _validate_policy(policy_doc, repo_root)
    signing_bytes = _read_bounded(repo_root / policy_doc["releaseSigningManifestPath"], "release signing manifest", MAX_JSON_BYTES)
    scope_bytes = _read_bounded(repo_root / policy_doc["releaseScopePath"], "release scope", MAX_JSON_BYTES)
    return {
        "policy": policy_doc,
        "policyBytes": policy_bytes,
        "signing": signing_doc,
        "scope": scope_doc,
        "policySha256": _sha256(policy_bytes),
        "signingSha256": _sha256(signing_bytes),
        "scopeSha256": _sha256(scope_bytes),
    }


def _host_path_from_cache(value: str) -> Path:
    # Windows CMake caches remain authoritative when this offline validator is
    # invoked from WSL. Normalize only a drive-root path; never shell-resolve it.
    match = re.fullmatch(r"([A-Za-z]):[\\/](.*)", value)
    if match and os.name != "nt" and Path("/mnt").is_dir():
        return Path("/mnt") / match.group(1).lower() / match.group(2).replace("\\", "/")
    return Path(value)


def _read_cmake_cache(build_root: Path, repo_root: Path, configuration: str) -> dict:
    _assert_directory(build_root, "CMake build root")
    cache_path = build_root / "CMakeCache.txt"
    data = _read_bounded(cache_path, "CMake cache", MAX_CACHE_BYTES)
    try:
        text = data.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise ArtifactManifestError("CMake cache is not UTF-8 text") from exc
    values: dict[str, str] = {}
    for line in text.splitlines():
        if not line or line.startswith("#") or line.startswith("//") or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        left, value = line.split("=", 1)
        key = left.split(":", 1)[0]
        if key in values:
            raise ArtifactManifestError(f"CMake cache contains duplicate authority key: {key}")
        values[key] = value.strip()
    home = values.get("CMAKE_HOME_DIRECTORY")
    if not home:
        raise ArtifactManifestError("CMake cache does not contain CMAKE_HOME_DIRECTORY")
    try:
        configured = _host_path_from_cache(home).resolve()
    except OSError as exc:
        raise ArtifactManifestError("CMAKE_HOME_DIRECTORY cannot be resolved") from exc
    if os.path.normcase(str(configured)) != os.path.normcase(str(repo_root.resolve())):
        raise ArtifactManifestError("build root was not configured from the reviewed repository checkout")
    if configuration != "Release":
        raise ArtifactManifestError("release artifact preflight requires Release configuration")
    multi = bool(values.get("CMAKE_CONFIGURATION_TYPES"))
    if not multi and values.get("CMAKE_BUILD_TYPE") != "Release":
        raise ArtifactManifestError("single-configuration build root is not configured as Release")
    platform = values.get("CMAKE_GENERATOR_PLATFORM", "")
    if platform and platform.lower() not in {"x64", "amd64"}:
        raise ArtifactManifestError("CMake generator platform is not x64")
    return {"multiConfig": multi, "generator": values.get("CMAKE_GENERATOR", "unknown")[:128]}


def _run_git(repo_root: Path, args: list[str]) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(repo_root), *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ArtifactManifestError(f"cannot execute git source-provenance check: {exc}") from exc
    if completed.returncode != 0:
        raise ArtifactManifestError(f"git source-provenance check failed: {completed.stderr.strip()[:512]}")
    output = completed.stdout
    if len(output.encode("utf-8", errors="ignore")) > MAX_GIT_OUTPUT:
        raise ArtifactManifestError("git provenance output exceeds bound")
    return output.strip()


def source_state(repo_root: Path, expected_commit: str | None, allow_unassigned: bool = False) -> dict:
    head = _run_git(repo_root, ["rev-parse", "HEAD"]).lower()
    _safe_hex(head, 40, "repository HEAD")
    if expected_commit is not None:
        expected_commit = expected_commit.lower()
        _safe_hex(expected_commit, 40, "expected source commit")
        if head != expected_commit:
            raise ArtifactManifestError("expected source revision does not match checked-out repository HEAD")
    elif not allow_unassigned:
        raise ArtifactManifestError("exact source commit is required for canonical manifest generation")
    dirty = bool(_run_git(repo_root, ["status", "--porcelain"]))
    return {"commitSha": expected_commit or head, "headSha": head, "treeState": "DirtyControlled" if dirty else "Clean"}


def _pe_observation(path: Path, maximum: int) -> tuple[str, bool]:
    size = _assert_regular(path, f"release executable {path.name}", maximum)
    with path.open("rb") as handle:
        prefix = handle.read(min(size, 4096))
        if len(prefix) < 64 or prefix[:2] != b"MZ":
            raise ArtifactManifestError(f"{path.name} is not a PE executable")
        pe_offset = struct.unpack_from("<I", prefix, 0x3C)[0]
        required = pe_offset + 24 + 152
        if required > len(prefix):
            handle.seek(0)
            prefix = handle.read(min(size, max(required, 4096)))
        if required > len(prefix) or prefix[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise ArtifactManifestError(f"{path.name} has malformed PE headers")
        machine = struct.unpack_from("<H", prefix, pe_offset + 4)[0]
        if machine != 0x8664:
            raise ArtifactManifestError(f"{path.name} is not AMD64/x64")
        optional = pe_offset + 24
        magic = struct.unpack_from("<H", prefix, optional)[0]
        if magic != 0x20B:
            raise ArtifactManifestError(f"{path.name} does not contain a PE32+ x64 optional header")
        security = optional + 112 + 4 * 8
        cert_offset, cert_size = struct.unpack_from("<II", prefix, security)
        signed_material = cert_offset != 0 and cert_size != 0
        if signed_material and (cert_offset + cert_size > size or cert_offset < required):
            raise ArtifactManifestError(f"{path.name} has malformed Authenticode certificate table bounds")
        return "x64", signed_material


def _script_signature_observation(path: Path, maximum: int) -> bool:
    data = _read_bounded(path, f"release script {path.name}", maximum)
    begin = b"# SIG # Begin signature block" in data
    end = b"# SIG # End signature block" in data
    if begin != end:
        raise ArtifactManifestError("PowerShell script contains a partial/malformed Authenticode signature block")
    return begin and end


def _artifact_source_map(
    repo_root: Path,
    inputs: dict,
    input_mode: str,
    build_root: Path | None,
    package_root: Path | None,
    configuration: str,
) -> tuple[list[dict], bool, dict]:
    if input_mode not in INPUT_MODES:
        raise ArtifactManifestError("input mode is invalid")
    policy = inputs["policy"]
    signing_by_id = {item["id"]: item for item in inputs["signing"]["artifacts"]}
    cache_info: dict = {"multiConfig": False, "generator": "package"}
    package_verified = input_mode == "PackageRoot"
    if input_mode == "BuildRoot":
        if build_root is None or package_root is not None:
            raise ArtifactManifestError("BuildRoot mode requires only --build-root")
        build_root = build_root.resolve()
        cache_info = _read_cmake_cache(build_root, repo_root, configuration)
    else:
        if package_root is None or build_root is not None:
            raise ArtifactManifestError("PackageRoot mode requires only --package-root")
        package_root = package_root.resolve()
        _assert_directory(package_root, "release package root")
        _validate_package_file_set(package_root, policy)
    records: list[dict] = []
    for declared in sorted(policy["artifacts"], key=lambda item: item["id"]):
        signing = signing_by_id[declared["id"]]
        file_name = signing["fileName"]
        if input_mode == "PackageRoot":
            source_locator = declared["packagePath"]
            source = _resolve_under(package_root, source_locator, "package artifact path")
        elif signing["kind"] == "cmake-executable":
            relative = f"{configuration}/{file_name}" if cache_info["multiConfig"] else file_name
            source_locator = relative.replace("\\", "/")
            source = _resolve_under(build_root, source_locator, "build artifact path")
        else:
            source_locator = signing["sourcePath"]
            source = _resolve_under(repo_root, source_locator, "repository artifact source")
        records.append({
            "declared": declared,
            "signing": signing,
            "path": source,
            "sourceLocator": source_locator,
        })
    return records, package_verified, cache_info


def _walk_regular_files(root: Path, maximum_files: int) -> set[str]:
    _assert_directory(root, "package root")
    _integer(maximum_files, "maximum packaged files", 1, 64)
    result: set[str] = set()
    folded: set[str] = set()
    root_entries = 0
    for child in root.iterdir():
        root_entries += 1
        if root_entries > maximum_files + len(ALLOWED_PACKAGE_METADATA) + 1:
            raise ArtifactManifestError("package root contains an unbounded number of entries")
        if child.name == "x64":
            if not child.is_dir() or _is_reparse(child):
                raise ArtifactManifestError("package x64 root must be one non-reparse directory")
            x64_entries = 0
            for leaf in child.iterdir():
                x64_entries += 1
                if x64_entries > maximum_files:
                    raise ArtifactManifestError("package x64 root exceeds the reviewed file-count bound")
                if _is_reparse(leaf) or not leaf.is_file():
                    raise ArtifactManifestError("package contains a reparse/symlink/non-regular x64 entry")
                relative = f"x64/{leaf.name}"
                _safe_package_path(relative, "packaged x64 file path")
                key = relative.casefold()
                if key in folded:
                    raise ArtifactManifestError("package contains Windows case-fold path collision")
                folded.add(key)
                result.add(relative)
            continue
        if child.is_dir() or _is_reparse(child) or not child.is_file():
            raise ArtifactManifestError("package contains an unexpected/reparse directory or non-regular root entry")
        relative = _safe_repo_relative(child.name, "packaged root metadata path")
        key = relative.casefold()
        if key in folded:
            raise ArtifactManifestError("package contains Windows case-fold path collision")
        folded.add(key)
        result.add(relative)
    if len(result) > maximum_files + len(ALLOWED_PACKAGE_METADATA):
        raise ArtifactManifestError("package file inventory exceeds the reviewed bounded count")
    return result


def _validate_package_file_set(package_root: Path, policy: dict) -> None:
    actual = _walk_regular_files(package_root, policy["limits"]["maximumPackagedFiles"])
    required = {item["packagePath"] for item in policy["artifacts"]}
    provenance_present = "signing-provenance.json" in actual
    signature_present = "signing-provenance.json.p7s" in actual
    if provenance_present != signature_present:
        raise ArtifactManifestError("package contains an incomplete signing provenance pair")
    allowed = set(required)
    if provenance_present:
        allowed |= ALLOWED_PACKAGE_METADATA
    missing = sorted(required - actual)
    extra = sorted(actual - allowed)
    if missing:
        raise ArtifactManifestError("release package is missing allowlisted file(s): " + ", ".join(missing))
    if extra:
        raise ArtifactManifestError("release package contains unexpected file(s): " + ", ".join(extra))


def inspect_release_inputs(
    repo_root: Path,
    input_mode: str,
    build_root: Path | None,
    package_root: Path | None,
    configuration: str,
    expected_commit: str | None,
    release_version: str | None,
    release_revision: int | None,
    policy_path: Path | None = None,
) -> dict:
    inputs = load_reviewed_inputs(repo_root, policy_path)
    source = source_state(repo_root, expected_commit, allow_unassigned=True)
    policy = inputs["policy"]
    signing_by_id = {item["id"]: item for item in inputs["signing"]["artifacts"]}
    report = {
        "schemaVersion": 1,
        "mode": "ControlledPreflightInspection",
        "sourceCommitSha": source["headSha"],
        "sourceTreeState": source["treeState"],
        "releaseVersion": release_version if release_version is not None else "UNASSIGNED",
        "releaseRevision": release_revision if release_revision is not None else None,
        "architecture": "x64",
        "inputMode": input_mode,
        "expected": [],
        "found": [],
        "missing": [],
        "unexpectedPackagedFiles": [],
        "unrelatedBuildOutputCount": 0,
        "signingStates": {},
        "manifestState": "NOT_GENERATED",
        "sbomState": "NOT_GENERATED",
        "qualificationState": "ControlledUnqualified",
        "qualificationBlockers": [],
    }
    try:
        records, _, cache = _artifact_source_map(
            repo_root, inputs, input_mode, build_root, package_root, configuration)
        _ = cache
    except ArtifactManifestError as exc:
        if input_mode == "PackageRoot" and package_root is not None and package_root.exists() and package_root.is_dir():
            actual = _walk_regular_files(package_root, policy["limits"]["maximumPackagedFiles"])
            allowed = {item["packagePath"] for item in policy["artifacts"]} | ALLOWED_PACKAGE_METADATA
            report["unexpectedPackagedFiles"] = sorted(actual - allowed)
        report["qualificationBlockers"].append(str(exc))
        # Build mode may still report individual missing files even if no complete map exists.
        records = []
        if input_mode == "BuildRoot" and build_root is not None:
            try:
                cache = _read_cmake_cache(build_root.resolve(), repo_root.resolve(), configuration)
                for declared in sorted(policy["artifacts"], key=lambda item: item["id"]):
                    signing = signing_by_id[declared["id"]]
                    locator = signing.get("sourcePath") if signing["kind"] == "powershell-script" else (
                        f"{configuration}/{signing['fileName']}" if cache["multiConfig"] else signing["fileName"]
                    )
                    root = repo_root if signing["kind"] == "powershell-script" else build_root
                    records.append({"declared": declared, "signing": signing, "path": root / locator, "sourceLocator": locator})
            except ArtifactManifestError:
                pass
    max_artifact = policy["limits"]["maximumArtifactBytes"]
    allowed_build_names: set[str] = set()
    for record in records:
        artifact_id = record["declared"]["id"]
        report["expected"].append(artifact_id)
        if record["signing"]["kind"] == "cmake-executable":
            allowed_build_names.add(record["signing"]["fileName"].casefold())
        try:
            _assert_regular(record["path"], f"expected release artifact {artifact_id}", max_artifact)
            if record["signing"]["kind"] == "cmake-executable":
                _, signed = _pe_observation(record["path"], max_artifact)
            else:
                signed = _script_signature_observation(record["path"], max_artifact)
            state = "SignedUnverified" if signed else "UnsignedControlled"
            report["found"].append(artifact_id)
            report["signingStates"][artifact_id] = state
        except ArtifactManifestError:
            report["missing"].append(artifact_id)
    if input_mode == "BuildRoot" and build_root is not None:
        try:
            cache = _read_cmake_cache(build_root.resolve(), repo_root.resolve(), configuration)
            output_dir = build_root.resolve() / configuration if cache["multiConfig"] else build_root.resolve()
            if output_dir.is_dir() and not _is_reparse(output_dir):
                count = 0
                for child in output_dir.iterdir():
                    if child.is_file() and not _is_reparse(child) and child.name.casefold() not in allowed_build_names:
                        count += 1
                report["unrelatedBuildOutputCount"] = count
        except ArtifactManifestError:
            pass
    blockers = report["qualificationBlockers"]
    for blocker in (
        "controlled-preflight-only",
        "production-signatures-not-independently-verified",
    ):
        if blocker not in blockers:
            blockers.append(blocker)
    if source["treeState"] != "Clean":
        blockers.append("source-tree-dirty")
    if release_revision is None:
        blockers.append("release-revision-unassigned")
    if release_version is None:
        blockers.append("release-version-unassigned")
    if policy["legal"]["projectLicenseState"] != "Resolved":
        blockers.append("project-license-unresolved")
    if policy["legal"]["distributionNoticesState"] != "Resolved":
        blockers.append("distribution-notices-unresolved")
    if report["missing"]:
        blockers.append("expected-release-artifacts-missing")
    if report["unexpectedPackagedFiles"]:
        blockers.append("unexpected-packaged-artifacts")
    report["qualificationBlockers"] = sorted(set(blockers))
    return report


def _wsl_to_windows(path: Path) -> str:
    text = str(path.resolve())
    match = re.match(r"^/mnt/([A-Za-z])/(.*)$", text)
    if match:
        return match.group(1).upper() + ":\\" + match.group(2).replace("/", "\\")
    return text


def _production_signature_evidence(package_root: Path, policy: dict, actual_hashes: dict[str, str]) -> dict:
    _ = package_root, policy, actual_hashes
    raise ArtifactManifestError(
        "production signature verification is a separate manual/deployment gate; "
        "controlled artifact preflight refuses package-contained self-verification"
    )


def generate_release_bundle(
    repo_root: Path,
    output_dir: Path,
    input_mode: str,
    build_root: Path | None,
    package_root: Path | None,
    configuration: str,
    release_version: str,
    release_revision: int,
    commit_sha: str,
    qualification_mode: str,
    verify_production_signatures: bool = False,
    policy_path: Path | None = None,
) -> dict:
    repo_root = repo_root.resolve()
    release_version = _safe_version(release_version, "release version")
    _integer(release_revision, "release revision", 1, (1 << 63) - 1)
    commit_sha = _safe_hex(commit_sha.lower(), 40, "release source commit")
    if qualification_mode not in QUALIFICATION_MODES:
        raise ArtifactManifestError("qualification mode is invalid")
    if verify_production_signatures and input_mode != "PackageRoot":
        raise ArtifactManifestError("production signature verification requires PackageRoot mode")
    inputs = load_reviewed_inputs(repo_root, policy_path)
    source = source_state(repo_root, commit_sha)
    records, package_verified, cache_info = _artifact_source_map(
        repo_root, inputs, input_mode, build_root, package_root, configuration)
    _ = cache_info
    max_artifact = inputs["policy"]["limits"]["maximumArtifactBytes"]
    observations: list[dict] = []
    actual_hashes: dict[str, str] = {}
    signed_material_by_id: dict[str, bool] = {}
    for record in records:
        artifact_id = record["declared"]["id"]
        digest, size = _sha256_file(record["path"], f"release artifact {artifact_id}", max_artifact)
        if record["signing"]["kind"] == "cmake-executable":
            architecture, signed_material = _pe_observation(record["path"], max_artifact)
        else:
            architecture = "x64"
            signed_material = _script_signature_observation(record["path"], max_artifact)
        actual_hashes[artifact_id] = digest
        signed_material_by_id[artifact_id] = signed_material
        observations.append({
            "record": record,
            "sha256": digest,
            "bytes": size,
            "architecture": architecture,
        })
    production_signature = None
    if verify_production_signatures:
        production_signature = _production_signature_evidence(package_root.resolve(), inputs["policy"], actual_hashes)
    artifact_rows: list[dict] = []
    for observed in observations:
        record = observed["record"]
        artifact_id = record["declared"]["id"]
        signed_material = signed_material_by_id[artifact_id]
        if production_signature is not None:
            signing_state = "ProductionSignatureVerified"
            signer_thumbprint = production_signature["signerThumbprint"]
            signing_provenance = production_signature["provenanceSha256"]
            provenance_state = "P8SigningPipelineVerified"
        elif signed_material:
            signing_state = "SignedUnverified"
            signer_thumbprint = None
            signing_provenance = None
            provenance_state = "SignatureMaterialObservedWithoutIndependentVerification"
        elif qualification_mode == "Controlled":
            signing_state = "UnsignedControlled"
            signer_thumbprint = None
            signing_provenance = None
            provenance_state = "SourceBoundControlled"
        else:
            signing_state = "SigningExpectedAbsent"
            signer_thumbprint = None
            signing_provenance = None
            provenance_state = "SourceBoundSigningPending"
        artifact_rows.append({
            "id": artifact_id,
            "fileName": record["signing"]["fileName"],
            "kind": record["signing"]["kind"],
            "componentRole": record["declared"]["componentRole"],
            "architecture": observed["architecture"],
            "packagePath": record["declared"]["packagePath"],
            "sourceLocator": record["sourceLocator"].replace("\\", "/"),
            "bytes": observed["bytes"],
            "sha256": observed["sha256"],
            "sourceCommitSha": commit_sha,
            "signingState": signing_state,
            "signingExpected": True,
            "signerThumbprint": signer_thumbprint,
            "signingProvenanceSha256": signing_provenance,
            "provenanceState": provenance_state,
        })
    blockers: list[str] = []
    if qualification_mode == "Controlled":
        blockers.append("controlled-preflight-only")
    if source["treeState"] != "Clean":
        blockers.append("source-tree-dirty")
    if any(row["signingState"] != "ProductionSignatureVerified" for row in artifact_rows):
        blockers.append("production-signatures-not-independently-verified")
    if inputs["policy"]["legal"]["projectLicenseState"] != "Resolved":
        blockers.append("project-license-unresolved")
    if inputs["policy"]["legal"]["distributionNoticesState"] != "Resolved":
        blockers.append("distribution-notices-unresolved")
    preflight_state = "Verified"
    qualification_state = "ArtifactPreflightReady" if not blockers else (
        "ControlledUnqualified" if qualification_mode == "Controlled" else "Blocked"
    )
    manifest = {
        "schemaVersion": 1,
        "kind": "HydraSeatReleaseArtifactManifest",
        "product": "HydraSeat",
        "release": {
            "version": release_version,
            "revision": release_revision,
            "architecture": "x64",
            "qualificationMode": qualification_mode,
        },
        "source": {
            "commitSha": commit_sha,
            "checkoutHeadSha": source["headSha"],
            "treeState": source["treeState"],
            "buildConfiguration": configuration,
            "inputMode": input_mode,
            "buildRootSourceBound": True,
            "buildReproducibility": "NotProven",
        },
        "reviewedInputs": {
            "preflightPolicyPath": "config/release-artifact-preflight.json",
            "preflightPolicySha256": inputs["policySha256"],
            "releaseSigningManifestPath": inputs["policy"]["releaseSigningManifestPath"],
            "releaseSigningManifestSha256": inputs["signingSha256"],
            "releaseScopePath": inputs["policy"]["releaseScopePath"],
            "releaseScopeSha256": inputs["scopeSha256"],
        },
        "legal": copy.deepcopy(inputs["policy"]["legal"]),
        "artifacts": sorted(artifact_rows, key=lambda item: item["id"]),
        "thirdPartyRedistributables": [],
        "integrity": {
            "artifactBytesVerified": True,
            "manifestDeterministic": True,
            "sourceProvenanceBound": True,
            "packageFileSetVerified": package_verified,
            "buildReproducibilityProven": False,
            "artifactPreflightState": preflight_state,
            "artifactPreflightQualificationState": qualification_state,
            "qualificationBlockers": sorted(blockers),
        },
    }
    manifest_bytes = _canonical_json_bytes(manifest)
    if len(manifest_bytes) > inputs["policy"]["limits"]["maximumManifestBytes"]:
        raise ArtifactManifestError("canonical artifact manifest exceeds reviewed maximum")
    manifest_sha = _sha256(manifest_bytes)
    checksums = _checksum_bytes(manifest)
    sbom = _sbom_document(manifest, manifest_sha)
    provenance = _provenance_document(
        manifest,
        manifest_sha,
        "P8InstallerContractVerified" if production_signature is not None else "NotPerformed",
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    _assert_directory(output_dir, "artifact preflight output directory")
    _atomic_write(output_dir / MANIFEST_NAME, manifest_bytes)
    _atomic_write(output_dir / CHECKSUMS_NAME, checksums)
    _atomic_write(output_dir / SBOM_NAME, _canonical_json_bytes(sbom))
    _atomic_write(output_dir / PROVENANCE_NAME, _canonical_json_bytes(provenance))
    validate_release_bundle(
        repo_root=repo_root,
        manifest_path=output_dir / MANIFEST_NAME,
        build_root=build_root,
        package_root=package_root,
        expected_commit_sha=commit_sha,
        expected_release_version=release_version,
        expected_release_revision=release_revision,
        expected_architecture="x64",
        policy_path=policy_path,
    )
    return manifest


def _checksum_bytes(manifest: dict) -> bytes:
    lines = [f"{item['sha256']}  {item['packagePath']}" for item in sorted(manifest["artifacts"], key=lambda row: row["packagePath"].casefold())]
    return ("\n".join(lines) + "\n").encode("ascii")


def _sbom_document(manifest: dict, manifest_sha: str) -> dict:
    components = []
    for item in sorted(manifest["artifacts"], key=lambda row: row["id"]):
        components.append({
            "type": "file",
            "name": item["fileName"],
            "version": manifest["release"]["version"],
            "hashes": [{"alg": "SHA-256", "content": item["sha256"]}],
            "properties": [
                {"name": "hydraseat:artifactId", "value": item["id"]},
                {"name": "hydraseat:componentRole", "value": item["componentRole"]},
                {"name": "hydraseat:architecture", "value": item["architecture"]},
                {"name": "hydraseat:kind", "value": item["kind"]},
                {"name": "hydraseat:sourceCommitSha", "value": item["sourceCommitSha"]},
            ],
        })
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "name": "HydraSeat",
                "version": manifest["release"]["version"],
                "properties": [
                    {"name": "hydraseat:releaseRevision", "value": str(manifest["release"]["revision"])},
                    {"name": "hydraseat:architecture", "value": manifest["release"]["architecture"]},
                    {"name": "hydraseat:sourceCommitSha", "value": manifest["source"]["commitSha"]},
                    {"name": "hydraseat:artifactManifestSha256", "value": manifest_sha},
                    {"name": "hydraseat:projectLicenseState", "value": manifest["legal"]["projectLicenseState"]},
                ],
            }
        },
        "components": components,
        "properties": [
            {"name": "hydraseat:thirdPartyRedistributables", "value": "0"},
            {"name": "hydraseat:distributionNoticesState", "value": manifest["legal"]["distributionNoticesState"]},
        ],
    }


def _provenance_document(manifest: dict, manifest_sha: str, production_signature_verification: str) -> dict:
    return {
        "schemaVersion": 1,
        "artifactManifest": MANIFEST_NAME,
        "artifactManifestSha256": manifest_sha,
        "product": "HydraSeat",
        "releaseVersion": manifest["release"]["version"],
        "releaseRevision": manifest["release"]["revision"],
        "architecture": manifest["release"]["architecture"],
        "sourceCommitSha": manifest["source"]["commitSha"],
        "sourceTreeState": manifest["source"]["treeState"],
        "buildConfiguration": manifest["source"]["buildConfiguration"],
        "inputMode": manifest["source"]["inputMode"],
        "releaseScopeSha256": manifest["reviewedInputs"]["releaseScopeSha256"],
        "releaseSigningManifestSha256": manifest["reviewedInputs"]["releaseSigningManifestSha256"],
        "preflightPolicySha256": manifest["reviewedInputs"]["preflightPolicySha256"],
        "artifactBytesVerified": manifest["integrity"]["artifactBytesVerified"],
        "manifestDeterministic": manifest["integrity"]["manifestDeterministic"],
        "sourceProvenanceBound": manifest["integrity"]["sourceProvenanceBound"],
        "packageFileSetVerified": manifest["integrity"]["packageFileSetVerified"],
        "buildReproducibility": manifest["source"]["buildReproducibility"],
        "productionSignatureVerification": production_signature_verification,
        "artifactPreflightState": manifest["integrity"]["artifactPreflightState"],
        "artifactPreflightQualificationState": manifest["integrity"]["artifactPreflightQualificationState"],
        "qualificationBlockers": manifest["integrity"]["qualificationBlockers"],
    }


def _validate_manifest_structure(manifest: dict, inputs: dict) -> None:
    _exact_keys(manifest, MANIFEST_KEYS, "release artifact manifest")
    if manifest["schemaVersion"] != 1 or manifest["kind"] != "HydraSeatReleaseArtifactManifest" or manifest["product"] != "HydraSeat":
        raise ArtifactManifestError("release artifact manifest kind/schema/product is invalid")
    _exact_keys(manifest["release"], RELEASE_KEYS, "release identity")
    _safe_version(manifest["release"]["version"], "manifest release version")
    _integer(manifest["release"]["revision"], "manifest release revision", 1, (1 << 63) - 1)
    if manifest["release"]["architecture"] != "x64" or manifest["release"]["qualificationMode"] not in QUALIFICATION_MODES:
        raise ArtifactManifestError("manifest release architecture/qualification mode is invalid")
    _exact_keys(manifest["source"], SOURCE_KEYS, "manifest source provenance")
    _safe_hex(manifest["source"]["commitSha"], 40, "manifest source commit")
    _safe_hex(manifest["source"]["checkoutHeadSha"], 40, "manifest checkout head")
    if manifest["source"]["commitSha"] != manifest["source"]["checkoutHeadSha"]:
        raise ArtifactManifestError("manifest source commit differs from checked-out source identity")
    if manifest["source"]["treeState"] not in {"Clean", "DirtyControlled"} or \
       manifest["source"]["buildConfiguration"] != "Release" or \
       manifest["source"]["inputMode"] not in INPUT_MODES or \
       manifest["source"]["buildRootSourceBound"] is not True or \
       manifest["source"]["buildReproducibility"] != "NotProven":
        raise ArtifactManifestError("manifest source/build provenance is invalid")
    _exact_keys(manifest["reviewedInputs"], REVIEWED_INPUT_KEYS, "manifest reviewed inputs")
    reviewed = manifest["reviewedInputs"]
    if reviewed["preflightPolicyPath"] != "config/release-artifact-preflight.json" or \
       reviewed["releaseSigningManifestPath"] != "config/release-signing-manifest.json" or \
       reviewed["releaseScopePath"] != "config/release-scope-v1.json":
        raise ArtifactManifestError("manifest references unreviewed release configuration")
    if reviewed["preflightPolicySha256"] != inputs["policySha256"] or \
       reviewed["releaseSigningManifestSha256"] != inputs["signingSha256"] or \
       reviewed["releaseScopeSha256"] != inputs["scopeSha256"]:
        raise ArtifactManifestError("manifest reviewed configuration hashes do not match current exact files")
    _exact_keys(manifest["legal"], LEGAL_KEYS, "manifest legal state")
    if manifest["legal"] != inputs["policy"]["legal"]:
        raise ArtifactManifestError("manifest legal state differs from reviewed policy")
    if manifest["thirdPartyRedistributables"] != []:
        raise ArtifactManifestError("manifest invents undeclared third-party redistributables")
    artifacts = manifest["artifacts"]
    if not isinstance(artifacts, list) or len(artifacts) != len(inputs["policy"]["artifacts"]):
        raise ArtifactManifestError("manifest artifact count differs from reviewed allowlist")
    policy_by_id = {item["id"]: item for item in inputs["policy"]["artifacts"]}
    signing_by_id = {item["id"]: item for item in inputs["signing"]["artifacts"]}
    ids: set[str] = set()
    names: set[str] = set()
    folded: set[str] = set()
    prior_id = ""
    for item in artifacts:
        _exact_keys(item, ARTIFACT_KEYS, "manifest artifact")
        artifact_id = _safe_token(item["id"], "manifest artifact id")
        if artifact_id in ids or artifact_id not in policy_by_id:
            raise ArtifactManifestError("manifest contains duplicate/unreviewed artifact id")
        if prior_id and artifact_id <= prior_id:
            raise ArtifactManifestError("manifest artifact ordering is not deterministic by id")
        prior_id = artifact_id
        ids.add(artifact_id)
        expected_policy = policy_by_id[artifact_id]
        expected_signing = signing_by_id[artifact_id]
        name = item["fileName"]
        if name != expected_signing["fileName"] or item["kind"] != expected_signing["kind"] or \
           item["componentRole"] != expected_policy["componentRole"] or \
           item["packagePath"] != expected_policy["packagePath"] or item["architecture"] != "x64":
            raise ArtifactManifestError("manifest artifact identity/type/role/path/architecture differs from reviewed policy")
        if name in names or name.casefold() in folded:
            raise ArtifactManifestError("manifest contains duplicate/case-fold filename collision")
        names.add(name)
        folded.add(name.casefold())
        _safe_repo_relative(item["sourceLocator"], "manifest source locator")
        _integer(item["bytes"], "manifest artifact bytes", 1, inputs["policy"]["limits"]["maximumArtifactBytes"])
        _safe_hex(item["sha256"], 64, "manifest artifact hash")
        if item["sourceCommitSha"] != manifest["source"]["commitSha"]:
            raise ArtifactManifestError("manifest artifact source commit differs from release source")
        if item["signingState"] not in SIGNING_STATES or item["signingExpected"] is not True:
            raise ArtifactManifestError("manifest signing state is invalid")
        if item["signingState"] == "ProductionSignatureVerified":
            if not isinstance(item["signerThumbprint"], str) or not re.fullmatch(r"[A-F0-9]{40}", item["signerThumbprint"]) or \
               not isinstance(item["signingProvenanceSha256"], str) or not HEX64.fullmatch(item["signingProvenanceSha256"]) or \
               item["provenanceState"] != "P8SigningPipelineVerified":
                raise ArtifactManifestError("production signature claim lacks exact P8 verification provenance")
        else:
            if item["signerThumbprint"] is not None or item["signingProvenanceSha256"] is not None:
                raise ArtifactManifestError("unverified signing state carries forged signer/provenance authority")
    if ids != set(policy_by_id):
        raise ArtifactManifestError("manifest omits reviewed artifact id")
    _exact_keys(manifest["integrity"], INTEGRITY_KEYS, "manifest integrity state")
    integrity = manifest["integrity"]
    if integrity["artifactBytesVerified"] is not True or integrity["manifestDeterministic"] is not True or \
       integrity["sourceProvenanceBound"] is not True or integrity["buildReproducibilityProven"] is not False or \
       integrity["artifactPreflightState"] != "Verified" or not isinstance(integrity["packageFileSetVerified"], bool) or \
       integrity["artifactPreflightQualificationState"] not in {"ControlledUnqualified", "Blocked", "ArtifactPreflightReady"} or \
       not isinstance(integrity["qualificationBlockers"], list) or \
       integrity["qualificationBlockers"] != sorted(set(integrity["qualificationBlockers"])):
        raise ArtifactManifestError("manifest integrity/qualification state is invalid")
    blockers = set(integrity["qualificationBlockers"])
    if manifest["release"]["qualificationMode"] == "Controlled" and "controlled-preflight-only" not in blockers:
        raise ArtifactManifestError("controlled manifest omitted controlled-preflight qualification blocker")
    if manifest["source"]["treeState"] != "Clean" and "source-tree-dirty" not in blockers:
        raise ArtifactManifestError("dirty source manifest omitted qualification blocker")
    if any(item["signingState"] != "ProductionSignatureVerified" for item in artifacts) and \
       "production-signatures-not-independently-verified" not in blockers:
        raise ArtifactManifestError("manifest falsely qualifies without independent production signature verification")
    if manifest["legal"]["projectLicenseState"] != "Resolved" and "project-license-unresolved" not in blockers:
        raise ArtifactManifestError("manifest omitted unresolved project-license blocker")
    if manifest["legal"]["distributionNoticesState"] != "Resolved" and "distribution-notices-unresolved" not in blockers:
        raise ArtifactManifestError("manifest omitted unresolved distribution-notices blocker")
    if blockers and integrity["artifactPreflightQualificationState"] == "ArtifactPreflightReady":
        raise ArtifactManifestError("blocked artifact manifest claims ArtifactPreflightReady")


def _source_path_for_manifest_artifact(
    item: dict,
    manifest: dict,
    repo_root: Path,
    build_root: Path | None,
    package_root: Path | None,
) -> Path:
    mode = manifest["source"]["inputMode"]
    locator = item["sourceLocator"]
    if mode == "PackageRoot":
        if package_root is None or build_root is not None:
            raise ArtifactManifestError("PackageRoot manifest validation requires only --package-root")
        if locator != item["packagePath"]:
            raise ArtifactManifestError("PackageRoot manifest source locator differs from package path")
        return _resolve_under(package_root.resolve(), locator, "manifest package artifact")
    if build_root is None or package_root is not None:
        raise ArtifactManifestError("BuildRoot manifest validation requires only --build-root")
    if item["kind"] == "powershell-script":
        if locator != "tools/install_hydraseat.ps1":
            raise ArtifactManifestError("installer source locator differs from reviewed repository source")
        return _resolve_under(repo_root, locator, "manifest repository artifact")
    return _resolve_under(build_root.resolve(), locator, "manifest build artifact")


def validate_release_bundle(
    repo_root: Path,
    manifest_path: Path,
    build_root: Path | None,
    package_root: Path | None,
    expected_commit_sha: str,
    expected_release_version: str,
    expected_release_revision: int,
    expected_architecture: str,
    policy_path: Path | None = None,
) -> dict:
    repo_root = repo_root.resolve()
    inputs = load_reviewed_inputs(repo_root, policy_path)
    max_manifest = inputs["policy"]["limits"]["maximumManifestBytes"]
    manifest, manifest_bytes = _load_json(manifest_path, "release artifact manifest", max_manifest)
    _validate_manifest_structure(manifest, inputs)
    expected_commit_sha = _safe_hex(expected_commit_sha.lower(), 40, "expected validation commit")
    expected_release_version = _safe_version(expected_release_version, "expected validation release version")
    _integer(expected_release_revision, "expected validation release revision", 1, (1 << 63) - 1)
    if expected_architecture != "x64":
        raise ArtifactManifestError("expected validation architecture must be x64")
    if manifest["source"]["commitSha"] != expected_commit_sha:
        raise ArtifactManifestError("manifest source revision differs from expected exact source revision")
    if manifest["release"]["version"] != expected_release_version:
        raise ArtifactManifestError("manifest release version differs from expected release version")
    if manifest["release"]["revision"] != expected_release_revision:
        raise ArtifactManifestError("manifest release revision differs from expected release revision")
    if manifest["release"]["architecture"] != expected_architecture:
        raise ArtifactManifestError("manifest release architecture differs from expected architecture")
    current_source = source_state(repo_root, expected_commit_sha)
    if current_source["treeState"] != manifest["source"]["treeState"]:
        raise ArtifactManifestError("repository cleanliness changed after canonical manifest generation")
    if manifest["source"]["inputMode"] == "BuildRoot":
        _read_cmake_cache(build_root.resolve(), repo_root, "Release")
    else:
        _validate_package_file_set(package_root.resolve(), inputs["policy"])
    max_artifact = inputs["policy"]["limits"]["maximumArtifactBytes"]
    production_verified = all(item["signingState"] == "ProductionSignatureVerified" for item in manifest["artifacts"])
    signing_provenance_hash: str | None = None
    if production_verified:
        if manifest["source"]["inputMode"] != "PackageRoot":
            raise ArtifactManifestError("production signature verification cannot be claimed from build-root inputs")
        actual_hashes = {item["id"]: item["sha256"] for item in manifest["artifacts"]}
        evidence = _production_signature_evidence(package_root.resolve(), inputs["policy"], actual_hashes)
        signing_provenance_hash = evidence["provenanceSha256"]
    for item in manifest["artifacts"]:
        path = _source_path_for_manifest_artifact(item, manifest, repo_root, build_root, package_root)
        digest, size = _sha256_file(path, f"manifest-bound artifact {item['id']}", max_artifact)
        if digest != item["sha256"]:
            raise ArtifactManifestError(f"artifact bytes changed after manifest generation: {item['id']}")
        if size != item["bytes"]:
            raise ArtifactManifestError(f"artifact size changed after manifest generation: {item['id']}")
        if item["kind"] == "cmake-executable":
            architecture, signature_present = _pe_observation(path, max_artifact)
            if architecture != item["architecture"]:
                raise ArtifactManifestError(f"artifact architecture changed: {item['id']}")
        else:
            signature_present = _script_signature_observation(path, max_artifact)
        state = item["signingState"]
        if state in {"UnsignedControlled", "SigningExpectedAbsent"} and signature_present:
            raise ArtifactManifestError("manifest claims unsigned artifact but signature material is present")
        if state == "SignedUnverified" and not signature_present:
            raise ArtifactManifestError("manifest claims signed artifact but no signature material is present")
        if state == "ProductionSignatureVerified":
            if not signature_present or item["signingProvenanceSha256"] != signing_provenance_hash:
                raise ArtifactManifestError("production signature claim does not revalidate against exact P8 signing provenance")
    output_dir = manifest_path.parent
    checksum_bytes = _read_bounded(output_dir / CHECKSUMS_NAME, "release checksum file", MAX_JSON_BYTES)
    if checksum_bytes != _checksum_bytes(manifest):
        raise ArtifactManifestError("SHA256SUMS does not exactly describe manifest-bound artifact bytes")
    manifest_sha = _sha256(manifest_bytes)
    sbom, sbom_bytes = _load_json(output_dir / SBOM_NAME, "release SBOM", MAX_JSON_BYTES)
    expected_sbom = _sbom_document(manifest, manifest_sha)
    if sbom != expected_sbom or sbom_bytes != _canonical_json_bytes(expected_sbom):
        raise ArtifactManifestError("release SBOM is not the deterministic component inventory derived from manifest")
    provenance, provenance_bytes = _load_json(output_dir / PROVENANCE_NAME, "release provenance", MAX_JSON_BYTES)
    _exact_keys(provenance, PROVENANCE_KEYS, "release provenance")
    expected_provenance = _provenance_document(
        manifest,
        manifest_sha,
        "P8InstallerContractVerified" if production_verified else "NotPerformed",
    )
    if provenance != expected_provenance or provenance_bytes != _canonical_json_bytes(expected_provenance):
        raise ArtifactManifestError("release provenance is not the deterministic manifest-bound provenance record")
    return manifest


def _make_fake_pe(machine: int = 0x8664, signed: bool = False, payload: bytes = b"fixture") -> bytes:
    size = 1024
    data = bytearray(b"\0" * size)
    data[:2] = b"MZ"
    pe = 0x80
    struct.pack_into("<I", data, 0x3C, pe)
    data[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<H", data, pe + 4, machine)
    struct.pack_into("<H", data, pe + 20, 240)
    optional = pe + 24
    struct.pack_into("<H", data, optional, 0x20B if machine == 0x8664 else 0x10B)
    if machine == 0x8664:
        struct.pack_into("<I", data, optional + 108, 16)
        if signed:
            struct.pack_into("<II", data, optional + 112 + 4 * 8, 768, 128)
            data[768:896] = b"S" * 128
    data[512:512 + min(len(payload), 200)] = payload[:200]
    return bytes(data)


def _fixture_repo(root: Path) -> tuple[Path, Path, str]:
    repo = root / "repo"
    build = root / "build"
    (repo / "config").mkdir(parents=True)
    (repo / "tools").mkdir(parents=True)
    build.mkdir()
    signing = {
        "schemaVersion": 1,
        "releaseScope": "fixture reviewed scope",
        "artifacts": [],
        "excludedArtifactClasses": ["tests", "references"],
    }
    artifact_specs = [
        ("main-ui", "HydraSeat", "HydraSeat.exe", "ApplicationUI"),
        ("host", "hydra_host", "hydra_host.exe", "AuthoritativeHost"),
        ("seat-ui", "hydra_seat_ui", "hydra_seat_ui.exe", "SeatUI"),
        ("watchdog", "hydra_watchdog", "hydra_watchdog.exe", "RecoveryWatchdog"),
        ("reset", "hydra_reset", "hydra_reset.exe", "RecoveryReset"),
        ("profile-cli", "hydraseat_profilectl", "hydraseat_profilectl.exe", "ProfileCLI"),
        ("community-validator", "hydraseat_community_validate", "hydraseat_community_validate.exe", "CommunityValidator"),
    ]
    policy_artifacts = []
    release_dir = build / "Release"
    release_dir.mkdir()
    for index, (artifact_id, target, file_name, role) in enumerate(artifact_specs):
        signing["artifacts"].append({
            "id": artifact_id,
            "kind": "cmake-executable",
            "target": target,
            "fileName": file_name,
            "architectures": ["x64"],
        })
        policy_artifacts.append({"id": artifact_id, "componentRole": role, "packagePath": f"x64/{file_name}"})
        (release_dir / file_name).write_bytes(_make_fake_pe(payload=f"{artifact_id}-{index}".encode()))
    signing["artifacts"].append({
        "id": "installer-script",
        "kind": "powershell-script",
        "sourcePath": "tools/install_hydraseat.ps1",
        "fileName": "install_hydraseat.ps1",
        "architectures": ["x64"],
    })
    policy_artifacts.append({"id": "installer-script", "componentRole": "Installer", "packagePath": "x64/install_hydraseat.ps1"})
    installer = b"# deterministic unsigned installer fixture\r\nWrite-Output 'fixture'\r\n"
    (repo / "tools" / "install_hydraseat.ps1").write_bytes(installer)
    scope = {
        "schemaVersion": 1,
        "productVersion": "v1",
        "hostPlatform": {"hostArchitecture": "x64"},
    }
    policy = {
        "schemaVersion": 1,
        "product": "HydraSeat",
        "architecture": "x64",
        "releaseSigningManifestPath": "config/release-signing-manifest.json",
        "releaseScopePath": "config/release-scope-v1.json",
        "legal": {"projectLicenseState": "Unresolved", "distributionNoticesState": "Unresolved"},
        "limits": {"maximumArtifactBytes": 1024 * 1024, "maximumManifestBytes": MAX_JSON_BYTES, "maximumPackagedFiles": 16},
        "artifacts": policy_artifacts,
        "thirdPartyRedistributables": [],
    }
    (repo / "config" / "release-signing-manifest.json").write_bytes(_canonical_json_bytes(signing))
    (repo / "config" / "release-scope-v1.json").write_bytes(_canonical_json_bytes(scope))
    (repo / "config" / "release-artifact-preflight.json").write_bytes(_canonical_json_bytes(policy))
    cache = (
        f"CMAKE_HOME_DIRECTORY:INTERNAL={repo.as_posix()}\n"
        "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n"
        "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\n"
        "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64\n"
    )
    (build / "CMakeCache.txt").write_text(cache, encoding="utf-8")
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    subprocess.run(["git", "-C", str(repo), "config", "user.name", "HydraSeat Fixture"], check=True)
    subprocess.run(["git", "-C", str(repo), "config", "user.email", "fixture@invalid.example"], check=True)
    subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
    env = dict(os.environ)
    env.update({
        "GIT_AUTHOR_DATE": "2000-01-01T00:00:00+0000",
        "GIT_COMMITTER_DATE": "2000-01-01T00:00:00+0000",
    })
    subprocess.run(["git", "-C", str(repo), "commit", "-q", "-m", "fixture"], check=True, env=env)
    commit = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip().lower()
    return repo, build, commit


def _make_package(repo: Path, build: Path, package: Path) -> None:
    inputs = load_reviewed_inputs(repo)
    (package / "x64").mkdir(parents=True)
    for signing in inputs["signing"]["artifacts"]:
        destination = package / "x64" / signing["fileName"]
        if signing["kind"] == "cmake-executable":
            source = build / "Release" / signing["fileName"]
        else:
            source = repo / signing["sourcePath"]
        shutil.copyfile(source, destination)


def _expect_error(label: str, operation: Callable[[], object]) -> None:
    try:
        operation()
    except ArtifactManifestError:
        return
    raise AssertionError(f"self-test expected failure: {label}")


def _validate_fixture_bundle(repo: Path, build: Path, out: Path, commit: str) -> dict:
    return validate_release_bundle(
        repo, out / MANIFEST_NAME, build, None, commit, "0.1.0", 7, "x64"
    )


def self_test() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        repo, build, commit = _fixture_repo(base)
        out = base / "out"

        # 1-2 clean generation and deterministic repeated canonical output.
        first = generate_release_bundle(
            repo, out, "BuildRoot", build, None, "Release", "0.1.0", 7, commit, "Controlled"
        )
        first_bytes = (out / MANIFEST_NAME).read_bytes()
        first_checksums = (out / CHECKSUMS_NAME).read_bytes()
        first_sbom = (out / SBOM_NAME).read_bytes()
        first_provenance = (out / PROVENANCE_NAME).read_bytes()
        generate_release_bundle(
            repo, out, "BuildRoot", build, None, "Release", "0.1.0", 7, commit, "Controlled"
        )
        if (out / MANIFEST_NAME).read_bytes() != first_bytes or \
           (out / CHECKSUMS_NAME).read_bytes() != first_checksums or \
           (out / SBOM_NAME).read_bytes() != first_sbom or \
           (out / PROVENANCE_NAME).read_bytes() != first_provenance:
            raise AssertionError("same exact inputs did not produce identical canonical outputs")
        if first["integrity"]["artifactPreflightQualificationState"] != "ControlledUnqualified":
            raise AssertionError("unsigned controlled output was incorrectly qualified")

        # 3-4 changed bytes and changed size are both detected from actual reopened files.
        target = build / "Release" / "HydraSeat.exe"
        original = target.read_bytes()
        mutated = bytearray(original)
        mutated[600] ^= 1
        target.write_bytes(mutated)
        _expect_error("artifact byte changed", lambda: _validate_fixture_bundle(repo, build, out, commit))
        target.write_bytes(original + b"size-change")
        _expect_error("artifact size changed", lambda: _validate_fixture_bundle(repo, build, out, commit))
        target.write_bytes(original)

        # 5 removed file.
        removed = build / "Release" / "hydra_host.exe"
        removed_bytes = removed.read_bytes()
        removed.unlink()
        _expect_error("artifact removed", lambda: _validate_fixture_bundle(repo, build, out, commit))
        removed.write_bytes(removed_bytes)

        # Package-mode fixture for exact packaged-file checks.
        package = base / "package"
        _make_package(repo, build, package)
        package_out = base / "package-out"
        package_manifest = generate_release_bundle(
            repo, package_out, "PackageRoot", None, package, "Release", "0.1.0", 7, commit, "Controlled"
        )
        if package_manifest["integrity"]["packageFileSetVerified"] is not True:
            raise AssertionError("package mode did not verify exact packaged file set")

        # Controlled preflight must never execute a package-contained verifier and
        # promote its caller-controlled success into production signing authority.
        _expect_error("package-contained production signature self-verification", lambda: generate_release_bundle(
            repo, base / "self-verify-out", "PackageRoot", None, package,
            "Release", "0.1.0", 7, commit, "ReleaseCandidate", verify_production_signatures=True))

        directory_package = base / "directory-package"
        _make_package(repo, build, directory_package)
        directory_leaf = directory_package / "x64" / "hydra_host.exe"
        directory_leaf.unlink()
        directory_leaf.mkdir()
        _expect_error("directory where release leaf expected", lambda: generate_release_bundle(
            repo, base / "directory-out", "PackageRoot", None, directory_package,
            "Release", "0.1.0", 7, commit, "Controlled"))

        bounded_package = base / "bounded-package"
        _make_package(repo, build, bounded_package)
        for index in range(9):
            (bounded_package / "x64" / f"extra-{index}.tmp").write_bytes(b"bounded-extra")
        _expect_error("unbounded package entry count", lambda: generate_release_bundle(
            repo, base / "bounded-out", "PackageRoot", None, bounded_package,
            "Release", "0.1.0", 7, commit, "Controlled"))

        # 6 unexpected package extra; 20 generated project files cannot enter payload; 21 reference source cannot enter.
        extra = package / "x64" / "unexpected.tmp"
        extra.write_bytes(b"extra")
        _expect_error("unexpected packaged file", lambda: generate_release_bundle(
            repo, base / "extra-out", "PackageRoot", None, package, "Release", "0.1.0", 7, commit, "Controlled"))
        extra.unlink()
        vcx = package / "x64" / "HydraSeat.vcxproj"
        vcx.write_text("generated project metadata", encoding="utf-8")
        _expect_error("generated vcxproj packaged", lambda: generate_release_bundle(
            repo, base / "vcx-out", "PackageRoot", None, package, "Release", "0.1.0", 7, commit, "Controlled"))
        vcx.unlink()
        reference = package / "reference-source.txt"
        reference.write_text("research reference must not ship", encoding="utf-8")
        _expect_error("research/reference source packaged", lambda: generate_release_bundle(
            repo, base / "reference-out", "PackageRoot", None, package, "Release", "0.1.0", 7, commit, "Controlled"))
        reference.unlink()
        (build / "Release" / "HydraSeat.vcxproj").write_text("ignored build metadata", encoding="utf-8")
        regenerated = generate_release_bundle(
            repo, base / "ignore-build-metadata", "BuildRoot", build, None, "Release", "0.1.0", 7, commit, "Controlled"
        )
        if any(item["fileName"].endswith(".vcxproj") for item in regenerated["artifacts"]):
            raise AssertionError("generated project metadata entered allowlisted build payload")

        # 7-9 duplicate id/file/case-fold collision in reviewed policy.
        policy_path = repo / "config" / "release-artifact-preflight.json"
        original_policy_bytes = policy_path.read_bytes()
        original_policy = json.loads(original_policy_bytes)
        for label, mutation in (
            ("duplicate artifact id", lambda p: p["artifacts"][1].__setitem__("id", p["artifacts"][0]["id"])),
            ("duplicate filename", lambda p: p["artifacts"][1].__setitem__("packagePath", p["artifacts"][0]["packagePath"])),
            ("case-fold filename collision", lambda p: p["artifacts"][1].__setitem__("packagePath", "x64/HYDRASEAT.EXE")),
        ):
            changed = copy.deepcopy(original_policy)
            mutation(changed)
            policy_path.write_bytes(_canonical_json_bytes(changed))
            _expect_error(label, lambda: load_reviewed_inputs(repo))
        policy_path.write_bytes(original_policy_bytes)

        # 10 wrong PE architecture.
        wrong_arch = build / "Release" / "hydra_seat_ui.exe"
        right_arch_bytes = wrong_arch.read_bytes()
        wrong_arch.write_bytes(_make_fake_pe(machine=0x14C))
        _expect_error("wrong architecture", lambda: generate_release_bundle(
            repo, base / "wrong-arch", "BuildRoot", build, None, "Release", "0.1.0", 7, commit, "Controlled"))
        wrong_arch.write_bytes(right_arch_bytes)

        # 11 wrong source revision.
        _expect_error("wrong source revision", lambda: generate_release_bundle(
            repo, base / "wrong-source", "BuildRoot", build, None, "Release", "0.1.0", 7, "f" * 40, "Controlled"))

        # 12 wrong release revision at validation boundary.
        _expect_error("wrong release revision", lambda: validate_release_bundle(
            repo, out / MANIFEST_NAME, build, None, commit, "0.1.0", 8, "x64"))

        # 13-14 absolute and traversal injection into policy.
        for label, injected in (("absolute path", "C:/outside/HydraSeat.exe"), ("dot-dot traversal", "x64/../HydraSeat.exe")):
            changed = copy.deepcopy(original_policy)
            changed["artifacts"][0]["packagePath"] = injected
            policy_path.write_bytes(_canonical_json_bytes(changed))
            _expect_error(label, lambda: load_reviewed_inputs(repo))
        policy_path.write_bytes(original_policy_bytes)

        # 15 symlink/reparse package escape.
        link_package = base / "link-package"
        _make_package(repo, build, link_package)
        victim = link_package / "x64" / "HydraSeat.exe"
        victim.unlink()
        external = base / "external.exe"
        external.write_bytes(_make_fake_pe())
        try:
            victim.symlink_to(external)
        except OSError as exc:
            raise AssertionError(f"self-test environment cannot exercise symlink escape rejection: {exc}") from exc
        _expect_error("symlink escape", lambda: generate_release_bundle(
            repo, base / "link-out", "PackageRoot", None, link_package, "Release", "0.1.0", 7, commit, "Controlled"))

        # 16 unknown component role.
        changed = copy.deepcopy(original_policy)
        changed["artifacts"][0]["componentRole"] = "MysteryRole"
        policy_path.write_bytes(_canonical_json_bytes(changed))
        _expect_error("unknown component role", lambda: load_reviewed_inputs(repo))
        policy_path.write_bytes(original_policy_bytes)

        # 17 malformed manifest.
        malformed = base / "malformed"
        shutil.copytree(out, malformed)
        (malformed / MANIFEST_NAME).write_bytes(b"{not-json\n")
        _expect_error("malformed manifest", lambda: validate_release_bundle(
            repo, malformed / MANIFEST_NAME, build, None, commit, "0.1.0", 7, "x64"))

        # 18 oversized manifest.
        oversized = base / "oversized"
        shutil.copytree(out, oversized)
        (oversized / MANIFEST_NAME).write_bytes(b"x" * (MAX_JSON_BYTES + 1))
        _expect_error("oversized manifest", lambda: validate_release_bundle(
            repo, oversized / MANIFEST_NAME, build, None, commit, "0.1.0", 7, "x64"))

        # 19 caller/manifest metadata cannot forge ProductionSignatureVerified.
        forged = base / "forged"
        shutil.copytree(out, forged)
        forged_manifest = json.loads((forged / MANIFEST_NAME).read_text(encoding="utf-8"))
        forged_manifest["artifacts"][0].update({
            "signingState": "ProductionSignatureVerified",
            "signerThumbprint": "A" * 40,
            "signingProvenanceSha256": "a" * 64,
            "provenanceState": "P8SigningPipelineVerified",
        })
        (forged / MANIFEST_NAME).write_bytes(_canonical_json_bytes(forged_manifest))
        _expect_error("forged signing metadata", lambda: validate_release_bundle(
            repo, forged / MANIFEST_NAME, build, None, commit, "0.1.0", 7, "x64"))

        # 22 controlled unsigned output is explicit and remains non-qualified.
        verified = _validate_fixture_bundle(repo, build, out, commit)
        if any(item["signingState"] != "UnsignedControlled" for item in verified["artifacts"]) or \
           verified["integrity"]["artifactPreflightQualificationState"] != "ControlledUnqualified" or \
           "production-signatures-not-independently-verified" not in verified["integrity"]["qualificationBlockers"]:
            raise AssertionError("unsigned controlled output acquired signed/release-qualified status")

    print("Release artifact manifest self-test passed: all 23 required tamper/path/signing/allowlist cases.")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Validate exact HydraSeat release artifact manifest/checksum/SBOM/provenance bytes.")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--package-root", type=Path)
    parser.add_argument("--expected-commit-sha")
    parser.add_argument("--expected-release-version")
    parser.add_argument("--expected-release-revision", type=int)
    parser.add_argument("--expected-architecture", default="x64")
    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        if args.self_test:
            self_test()
            if args.manifest is None:
                return 0
        required = (
            args.manifest,
            args.expected_commit_sha,
            args.expected_release_version,
            args.expected_release_revision,
        )
        if any(value is None for value in required):
            if args.self_test:
                return 0
            raise ArtifactManifestError(
                "--manifest, --expected-commit-sha, --expected-release-version, and --expected-release-revision are required"
            )
        manifest = validate_release_bundle(
            ROOT,
            args.manifest,
            args.build_root,
            args.package_root,
            args.expected_commit_sha,
            args.expected_release_version,
            args.expected_release_revision,
            args.expected_architecture,
        )
        print(
            "Release artifact manifest valid: "
            f"{manifest['release']['version']} rev {manifest['release']['revision']} "
            f"{manifest['release']['architecture']} {manifest['integrity']['artifactPreflightQualificationState']}"
        )
        return 0
    except (ArtifactManifestError, OSError, subprocess.SubprocessError) as exc:
        print(f"release artifact manifest validation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
