#!/usr/bin/env python3
"""Non-destructive publication hygiene preflight for HydraSeat."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PATH = ROOT / "tools" / "testdata" / "repository_publication_hygiene" / "cases.json"
SELF_FIXTURE_PATH = "tools/testdata/repository_publication_hygiene/cases.json"
MAX_SCANNABLE_BYTES = 8 * 1024 * 1024
MAX_TOTAL_SCAN_BYTES = 64 * 1024 * 1024
RESOLVED_PACKET_STATES = {"VALIDATED", "MERGED", "UPSTREAMED"}

CAT_SENSITIVE_NAME = "SENSITIVE_FILENAME"
CAT_SENSITIVE_SIGNING = "SENSITIVE_SIGNING_CONTENT"
CAT_SENSITIVE_AUTH = "SENSITIVE_AUTH_CONTENT"
CAT_PERSONAL_PATH = "PERSONAL_ABSOLUTE_PATH"
CAT_GENERATED = "GENERATED_ARTIFACT"
CAT_REFERENCE = "REFERENCE_REPOSITORY_LEAKAGE"
CAT_BINARY = "UNEXPECTED_BINARY_OR_ARCHIVE"
CAT_BRIDGE = "FORBIDDEN_AI_BRIDGE_ARTIFACT"
CAT_OPEN_SOURCE = "FALSE_OPEN_SOURCE_CLAIM"
CAT_RELEASE = "FALSE_RELEASE_CLAIM"
CAT_AUTHORITY = "RELEASE_GATE_AUTHORITY_UNAVAILABLE"
CAT_OVERSIZED = "UNSCANNED_OVERSIZED_TRACKED_FILE"
CAT_INDEX = "UNMERGED_INDEX_ENTRY"

ALL_CATEGORIES = {
    CAT_SENSITIVE_NAME,
    CAT_SENSITIVE_SIGNING,
    CAT_SENSITIVE_AUTH,
    CAT_PERSONAL_PATH,
    CAT_GENERATED,
    CAT_REFERENCE,
    CAT_BINARY,
    CAT_BRIDGE,
    CAT_OPEN_SOURCE,
    CAT_RELEASE,
    CAT_AUTHORITY,
    CAT_OVERSIZED,
    CAT_INDEX,
}

GENERATED_TOP_LEVEL_PREFIXES = ("build", "out", "cmake-build")
GENERATED_PARTS = {"CMakeFiles", ".vs", ".idea"}
GENERATED_NAMES = {
    "CMakeCache.txt",
    "CTestTestfile.cmake",
    "cmake_install.cmake",
    "build.ninja",
    ".ninja_deps",
    ".ninja_log",
}
GENERATED_SUFFIXES = {
    ".vcxproj", ".filters", ".sln", ".user", ".obj", ".o", ".pdb", ".ilk", ".pch", ".tlog",
}
UNEXPECTED_BINARY_SUFFIXES = {
    ".exe", ".dll", ".sys", ".msi", ".msix", ".appx", ".cab", ".pdb", ".lib", ".obj", ".o",
    ".a", ".so", ".dylib", ".zip", ".7z", ".rar", ".tar", ".tgz", ".gz", ".bz2", ".xz", ".zst", ".nupkg",
}
KNOWN_ALLOWED_BINARY_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico"}

WINDOWS_HOME_RE = re.compile(
    r"(?i)(?<![A-Za-z0-9_])[A-Za-z]:[\\/]+Users[\\/]+(?P<user>[^\\/\s\"'`]+)"
)
UNIX_HOME_RE = re.compile(r"(?<![A-Za-z0-9_])/(?:home|Users)/(?P<user>[^/\s\"'`]+)")
PLACEHOLDER_USERS = {
    "user", "users", "username", "name", "example", "example-user", "example_user",
    "yourname", "your-name", "your_name", "someone", "runner", "runneradmin",
    "alice", "private", "소스",
}

OPEN_SOURCE_CLAIMS = (
    re.compile(r"(?i)\bHydraSeat\s+is\s+(?:now\s+)?(?:an?\s+)?(?:legally\s+)?open[- ]source\b"),
    re.compile(r"(?i)\bthis\s+(?:repository|project)\s+is\s+(?:now\s+|already\s+)?(?:legally\s+)?open[- ]source\b"),
    re.compile(r"(?i)\bHydraSeat\s+is\s+(?:an?\s+)?open[- ]source\s+(?:project|product|application)\b"),
)
RELEASE_CLAIMS = (
    re.compile(r"(?i)\bHydraSeat\s+v?1(?:\.0)?\s+is\s+(?:now\s+)?(?:released|generally available|production[- ]ready)\b"),
    re.compile(r"(?i)\bHydraSeat\s+is\s+(?:now\s+)?(?:generally available|production[- ]ready|release[- ]ready)\b"),
    re.compile(r"(?i)\bready\s+for\s+(?:general availability|GA|production|public release)\b"),
    re.compile(r"(?i)\ball\s+(?:physical|manual|clean[- ]machine|signing|release)\s+(?:gates|acceptance)\s+(?:have\s+)?passed\b"),
)
NEGATION_RE = re.compile(
    r"(?i)(?:\bnot\b|\bnever\b|\bmust\s+not\b|\bdo\s+not\b|\bcannot\b|\bcan't\b|"
    r"\bpending\b|\bblocked\b|\bunresolved\b|\bbefore\b|\buntil\b)"
)


def _chars(*codes: int) -> str:
    return "".join(chr(code) for code in codes)


def _sensitive_name_set() -> set[str]:
    encoded = (
        (46, 101, 110, 118),
        (105, 100, 95, 114, 115, 97),
        (105, 100, 95, 100, 115, 97),
        (105, 100, 95, 101, 99, 100, 115, 97),
        (105, 100, 95, 101, 100, 50, 53, 53, 49, 57),
        (99, 114, 101, 100, 101, 110, 116, 105, 97, 108, 115, 46, 106, 115, 111, 110),
        (115, 101, 114, 118, 105, 99, 101, 45, 97, 99, 99, 111, 117, 110, 116, 46, 106, 115, 111, 110),
        (115, 101, 99, 114, 101, 116, 115, 46, 106, 115, 111, 110),
        (115, 101, 99, 114, 101, 116, 115, 46, 121, 97, 109, 108),
        (115, 101, 99, 114, 101, 116, 115, 46, 121, 109, 108),
    )
    return {_chars(*item) for item in encoded}


def _signing_material_pattern() -> re.Pattern[str]:
    begin = _chars(45, 45, 45, 45, 45, 66, 69, 71, 73, 78, 32)
    tail = _chars(80, 82, 73, 86, 65, 84, 69, 32, 75, 69, 89, 45, 45, 45, 45, 45)
    return re.compile(re.escape(begin) + r"(?:RSA |EC |DSA |OPENSSH )?" + re.escape(tail), re.IGNORECASE)


def _auth_patterns() -> tuple[re.Pattern[str], ...]:
    prefix_a = _chars(65, 75, 73, 65)
    prefix_b = _chars(65, 83, 73, 65)
    family = tuple(_chars(103, 104, code, 95) for code in (112, 111, 117, 115, 114))
    fine = _chars(103, 105, 116, 104, 117, 98, 95, 112, 97, 116, 95)
    compact = _chars(115, 107, 45)
    other = _chars(120, 111, 120)
    return (
        re.compile(r"\b(?:" + re.escape(prefix_a) + "|" + re.escape(prefix_b) + r")[0-9A-Z]{16}\b"),
        re.compile(r"\b(?:" + "|".join(re.escape(value) for value in family) + r")[A-Za-z0-9]{30,255}\b"),
        re.compile(r"\b" + re.escape(fine) + r"[A-Za-z0-9_]{40,255}\b"),
        re.compile(r"\b" + re.escape(compact) + r"[A-Za-z0-9_-]{32,255}\b"),
        re.compile(r"\b" + re.escape(other) + r"(?:b|p|a|r|s)-[A-Za-z0-9-]{20,255}\b"),
    )


SENSITIVE_NAMES = _sensitive_name_set()
SENSITIVE_SIGNING_RE = _signing_material_pattern()
SENSITIVE_AUTH_RES = _auth_patterns()


@dataclass(frozen=True)
class IndexEntry:
    path: str
    mode: str
    object_id: str
    stage: int


@dataclass(frozen=True)
class GateState:
    license_resolved: bool
    general_availability_resolved: bool
    manual_gates_pending: bool


@dataclass(frozen=True)
class Finding:
    category: str
    path: str
    detail: str
    line: int = 0


class HygieneError(RuntimeError):
    pass


def _run_git(root: Path, arguments: Sequence[str]) -> bytes:
    try:
        completed = subprocess.run(
            ["git", "-C", os.fspath(root), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise HygieneError(f"cannot inspect Git publication index: {exc}") from exc
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()[:512]
        raise HygieneError(f"Git publication index inspection failed: {detail}")
    return completed.stdout


def tracked_index_entries(root: Path) -> list[IndexEntry]:
    raw = _run_git(root, ["ls-files", "--stage", "-z"])
    entries: list[IndexEntry] = []
    for record in raw.split(b"\0"):
        if not record:
            continue
        try:
            metadata, path_bytes = record.split(b"\t", 1)
            mode_bytes, object_id_bytes, stage_bytes = metadata.split(b" ", 2)
            path = path_bytes.decode("utf-8", errors="strict").replace("\\", "/")
            mode = mode_bytes.decode("ascii", errors="strict")
            object_id = object_id_bytes.decode("ascii", errors="strict").lower()
            stage = int(stage_bytes.decode("ascii", errors="strict"))
        except (ValueError, UnicodeDecodeError) as exc:
            raise HygieneError("Git index returned malformed or non-UTF-8 metadata") from exc
        pure = PurePosixPath(path)
        if pure.is_absolute() or not pure.parts or any(part in {"", ".", ".."} for part in pure.parts):
            raise HygieneError(f"unsafe tracked path returned by Git: {path!r}")
        if re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", object_id) is None:
            raise HygieneError("Git index returned an invalid object identity")
        entries.append(IndexEntry(pure.as_posix(), mode, object_id, stage))
    return sorted(entries, key=lambda item: (item.path.casefold(), item.path, item.stage, item.mode, item.object_id))


def _packet_state(text: str, packet_id: str) -> str | None:
    heading = re.search(rf"(?m)^##\s+{re.escape(packet_id)}\b.*$", text)
    if heading is None:
        return None
    following = text[heading.end():]
    next_heading = re.search(r"(?m)^##\s+", following)
    section = following if next_heading is None else following[:next_heading.start()]
    match = re.search(r"(?m)^\*\*State:\*\*\s*([A-Z_]+)\s*$", section)
    return None if match is None else match.group(1)


def load_gate_state(root: Path) -> GateState:
    phase_path = root / "docs" / "implementation" / "PHASE10_RELEASE_HARDENING.md"
    status_path = root / "docs" / "implementation" / "STATUS.md"
    try:
        phase_text = phase_path.read_text(encoding="utf-8")
        status_text = status_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise HygieneError(f"cannot read authoritative release gate documents: {exc}") from exc

    license_state = _packet_state(phase_text, "P10-LIC-01")
    ga_state = _packet_state(phase_text, "P10-GA-01")
    if license_state is None or ga_state is None:
        raise HygieneError("authoritative Phase 10 license/GA packet state is missing")

    manual_heading = re.search(r"(?m)^## Manual/physical gates still pending\s*$", status_text)
    if manual_heading is None:
        raise HygieneError("authoritative manual-gate section is missing from STATUS.md")
    following = status_text[manual_heading.end():]
    next_heading = re.search(r"(?m)^##\s+", following)
    manual_section = following if next_heading is None else following[:next_heading.start()]

    return GateState(
        license_resolved=license_state in RESOLVED_PACKET_STATES,
        general_availability_resolved=ga_state in RESOLVED_PACKET_STATES,
        manual_gates_pending=bool(re.search(r"(?i)\bPENDING\b", manual_section)),
    )


def _finding(category: str, path: str, detail: str, line: int = 0) -> Finding:
    if category not in ALL_CATEGORIES:
        raise AssertionError(f"unknown publication-hygiene category: {category}")
    return Finding(category, path, detail, line)


def _suffix(path: str) -> str:
    lower = path.casefold()
    for compound in (".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst"):
        if lower.endswith(compound):
            return compound
    return Path(path).suffix.casefold()


def _is_generated_path(path: str) -> bool:
    pure = PurePosixPath(path)
    if not pure.parts:
        return False
    first = pure.parts[0].casefold()
    if any(first == prefix or first.startswith(prefix + "-") for prefix in GENERATED_TOP_LEVEL_PREFIXES):
        return True
    if any(part in GENERATED_PARTS for part in pure.parts):
        return True
    if pure.name in GENERATED_NAMES:
        return True
    lower_name = pure.name.casefold()
    return any(lower_name.endswith(suffix) for suffix in GENERATED_SUFFIXES)


def _is_reference_leak_path(path: str) -> bool:
    parts = PurePosixPath(path).parts
    return bool(parts) and parts[0].casefold() in {
        "references", ".references", "reference-repositories", "reference_repositories",
    }


def _sensitive_name_reason(path: str) -> str | None:
    name = PurePosixPath(path).name.casefold()
    example_name = _chars(46, 101, 110, 118, 46, 101, 120, 97, 109, 112, 108, 101)
    if name == example_name or name.startswith(example_name + "."):
        return None
    if name in SENSITIVE_NAMES:
        return "tracked filename commonly stores authentication material"
    container_suffixes = (
        _chars(46, 112, 49, 50),
        _chars(46, 112, 102, 120),
        _chars(46, 106, 107, 115),
        _chars(46, 107, 101, 121, 115, 116, 111, 114, 101),
    )
    if any(name.endswith(suffix) for suffix in container_suffixes):
        return "tracked filename has a sensitive authentication-container suffix"
    if re.fullmatch(r"(?:private[-_.]?)?key(?:pair)?\.(?:pem|key)", name):
        return "tracked filename is sensitive signing-material-like"
    dot_env_prefix = _chars(46, 101, 110, 118, 46)
    if name.startswith(dot_env_prefix):
        return "tracked filename is environment-authentication-like"
    return None


def _is_personal_user(segment: str) -> bool:
    normalized = segment.strip().rstrip(").,;:]}")
    lowered = normalized.casefold()
    if not lowered or lowered in PLACEHOLDER_USERS:
        return False
    # A path-detector regex can legitimately contain branches such as
    # `/home/|/Users/`. The capture after `/home/` is then the regex alternation
    # token itself, not a user name. `|` is not a valid Windows user-name
    # character and is outside the bounded user-name forms this preflight treats
    # as publication evidence.
    return not any(marker in normalized for marker in ("<", ">", "$", "%", "{", "}", "|"))


def _line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _claim_is_negated(text: str, match_start: int) -> bool:
    context = text[max(0, match_start - 120):match_start].rsplit("\n", 1)[-1]
    return NEGATION_RE.search(context) is not None


def _is_public_claim_path(path: str) -> bool:
    pure = PurePosixPath(path)
    name = pure.name.casefold()
    if len(pure.parts) == 1 and (name.startswith("readme") or name.startswith("release")):
        return True
    if pure.parts and pure.parts[0].casefold() == "docs":
        if len(pure.parts) > 1 and pure.parts[1].casefold() in {"implementation", "security"}:
            return False
        return name.endswith(".md") and any(
            token in name for token in ("release", "install", "getting", "quickstart")
        )
    return False


def _scan_text(path: str, text: str, gates: GateState) -> list[Finding]:
    findings: list[Finding] = []

    match = SENSITIVE_SIGNING_RE.search(text)
    if match is not None:
        findings.append(
            _finding(
                CAT_SENSITIVE_SIGNING,
                path,
                "tracked text contains a high-confidence signing-material marker",
                _line_number(text, match.start()),
            )
        )

    for pattern in SENSITIVE_AUTH_RES:
        match = pattern.search(text)
        if match is not None:
            findings.append(
                _finding(
                    CAT_SENSITIVE_AUTH,
                    path,
                    "tracked text matches a high-confidence authentication-material format",
                    _line_number(text, match.start()),
                )
            )
            break

    for pattern in (WINDOWS_HOME_RE, UNIX_HOME_RE):
        for match in pattern.finditer(text):
            if not _is_personal_user(match.group("user")):
                continue
            findings.append(
                _finding(
                    CAT_PERSONAL_PATH,
                    path,
                    "tracked text contains a literal personal user-home absolute path",
                    _line_number(text, match.start()),
                )
            )
            break

    if _is_public_claim_path(path):
        if not gates.license_resolved:
            for pattern in OPEN_SOURCE_CLAIMS:
                match = pattern.search(text)
                if match is not None and not _claim_is_negated(text, match.start()):
                    findings.append(
                        _finding(
                            CAT_OPEN_SOURCE,
                            path,
                            "public wording claims open-source status while P10-LIC-01 is unresolved",
                            _line_number(text, match.start()),
                        )
                    )
                    break

        if (not gates.general_availability_resolved) or gates.manual_gates_pending:
            for pattern in RELEASE_CLAIMS:
                match = pattern.search(text)
                if match is not None and not _claim_is_negated(text, match.start()):
                    findings.append(
                        _finding(
                            CAT_RELEASE,
                            path,
                            "public wording claims release/production readiness while GA/manual gates remain unresolved",
                            _line_number(text, match.start()),
                        )
                    )
                    break

    return findings


def _run_git_with_input(root: Path, arguments: Sequence[str], payload: bytes) -> bytes:
    try:
        completed = subprocess.run(
            ["git", "-C", os.fspath(root), *arguments],
            input=payload,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise HygieneError(f"cannot inspect indexed Git objects: {exc}") from exc
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()[:512]
        raise HygieneError(f"indexed Git object inspection failed: {detail}")
    return completed.stdout


def _load_index_blobs(
    root: Path,
    entries: Sequence[IndexEntry],
) -> tuple[dict[str, bytes], set[str]]:
    object_ids = sorted(
        {
            entry.object_id
            for entry in entries
            if entry.stage == 0 and entry.mode not in {"120000", "160000"}
        }
    )
    if not object_ids:
        return {}, set()

    payload = b"".join(object_id.encode("ascii") + b"\n" for object_id in object_ids)
    checked = _run_git_with_input(root, ["cat-file", "--batch-check"], payload)
    lines = checked.splitlines()
    if len(lines) != len(object_ids):
        raise HygieneError("Git batch size inventory returned an unexpected record count")

    sizes: dict[str, int] = {}
    for expected_id, line in zip(object_ids, lines):
        parts = line.decode("ascii", errors="strict").split()
        if len(parts) != 3:
            raise HygieneError("Git batch size inventory returned malformed metadata")
        observed_id, object_type, size_text = parts
        if observed_id.lower() != expected_id or object_type != "blob":
            raise HygieneError("Git index object identity/type changed during inventory")
        try:
            size = int(size_text)
        except ValueError as exc:
            raise HygieneError("Git batch size inventory returned a non-integer size") from exc
        if size < 0:
            raise HygieneError("Git batch size inventory returned a negative size")
        sizes[expected_id] = size

    oversized = {object_id for object_id, size in sizes.items() if size > MAX_SCANNABLE_BYTES}
    bounded_ids = [object_id for object_id in object_ids if object_id not in oversized]
    total_size = sum(sizes[object_id] for object_id in bounded_ids)
    if total_size > MAX_TOTAL_SCAN_BYTES:
        raise HygieneError(
            f"bounded indexed publication content exceeds aggregate scan limit {MAX_TOTAL_SCAN_BYTES}"
        )
    if not bounded_ids:
        return {}, oversized

    requested = b"".join(object_id.encode("ascii") + b"\n" for object_id in bounded_ids)
    batch = _run_git_with_input(root, ["cat-file", "--batch"], requested)
    blobs: dict[str, bytes] = {}
    cursor = 0
    for expected_id in bounded_ids:
        header_end = batch.find(b"\n", cursor)
        if header_end < 0:
            raise HygieneError("Git batch blob output ended before its header")
        try:
            header = batch[cursor:header_end].decode("ascii", errors="strict").split()
        except UnicodeDecodeError as exc:
            raise HygieneError("Git batch blob header is not ASCII") from exc
        if len(header) != 3:
            raise HygieneError("Git batch blob output returned malformed metadata")
        observed_id, object_type, size_text = header
        if observed_id.lower() != expected_id or object_type != "blob":
            raise HygieneError("Git batch blob identity/type changed during read")
        try:
            size = int(size_text)
        except ValueError as exc:
            raise HygieneError("Git batch blob output returned a non-integer size") from exc
        if size != sizes[expected_id]:
            raise HygieneError("Git batch blob size changed between inventory and read")
        content_start = header_end + 1
        content_end = content_start + size
        if content_end >= len(batch) or batch[content_end:content_end + 1] != b"\n":
            raise HygieneError("Git batch blob output is truncated or framing is invalid")
        blobs[expected_id] = batch[content_start:content_end]
        cursor = content_end + 1
    if cursor != len(batch):
        raise HygieneError("Git batch blob output contains unexpected trailing bytes")
    return blobs, oversized


def scan_entries(
    entries: Iterable[IndexEntry],
    gates: GateState,
    blobs: dict[str, bytes],
    oversized: set[str],
) -> list[Finding]:
    findings: list[Finding] = []
    seen: set[str] = set()

    for entry in entries:
        if entry.stage != 0:
            findings.append(
                _finding(CAT_INDEX, entry.path, f"Git index contains unmerged stage {entry.stage}; publication bytes are ambiguous")
            )
            continue
        if entry.path in seen:
            continue
        seen.add(entry.path)
        path = entry.path
        if path == SELF_FIXTURE_PATH:
            continue

        pure = PurePosixPath(path)
        folded_parts = [part.casefold() for part in pure.parts]
        if ".ai-bridge" in folded_parts:
            findings.append(
                _finding(CAT_BRIDGE, path, "coordination/scratch .ai-bridge content must not be published")
            )
        if _is_reference_leak_path(path):
            findings.append(
                _finding(CAT_REFERENCE, path, "reference-repository content is tracked inside the publication tree")
            )
        if _is_generated_path(path):
            findings.append(_finding(CAT_GENERATED, path, "generated build/project output is tracked"))

        suffix = _suffix(path)
        if suffix in UNEXPECTED_BINARY_SUFFIXES:
            findings.append(_finding(CAT_BINARY, path, f"unexpected committed binary/archive suffix {suffix!r}"))

        reason = _sensitive_name_reason(path)
        if reason is not None:
            findings.append(_finding(CAT_SENSITIVE_NAME, path, reason))

        if entry.mode in {"120000", "160000"}:
            continue
        if entry.object_id in oversized:
            findings.append(
                _finding(
                    CAT_OVERSIZED,
                    path,
                    f"indexed blob exceeds {MAX_SCANNABLE_BYTES} byte content-scan bound",
                )
            )
            continue
        data = blobs.get(entry.object_id)
        if data is None:
            findings.append(
                _finding(CAT_AUTHORITY, path, "indexed blob was not returned by bounded Git batch read")
            )
            continue

        if suffix not in KNOWN_ALLOWED_BINARY_SUFFIXES and b"\0" in data[:65536]:
            if suffix not in UNEXPECTED_BINARY_SUFFIXES:
                findings.append(
                    _finding(CAT_BINARY, path, "tracked file has binary/NUL content with no reviewed publication asset type")
                )
            continue
        try:
            text = data.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            if suffix not in KNOWN_ALLOWED_BINARY_SUFFIXES and suffix not in UNEXPECTED_BINARY_SUFFIXES:
                findings.append(
                    _finding(CAT_BINARY, path, "tracked file is non-UTF-8/binary with no reviewed publication asset type")
                )
            continue
        findings.extend(_scan_text(path, text, gates))

    unique = {(item.category, item.path, item.detail, item.line): item for item in findings}
    return sorted(
        unique.values(),
        key=lambda item: (item.category, item.path.casefold(), item.path, item.line, item.detail),
    )


def scan_repository(
    root: Path,
    gate_override: GateState | None = None,
) -> tuple[list[Finding], GateState, int]:
    root = root.resolve()
    gates = load_gate_state(root) if gate_override is None else gate_override
    entries = tracked_index_entries(root)
    blobs, oversized = _load_index_blobs(root, entries)
    findings = scan_entries(entries, gates, blobs, oversized)
    tracked_count = len({entry.path for entry in entries if entry.stage == 0})
    return findings, gates, tracked_count


def _fixture_path(path_kind: str | None, path: str | None) -> str:
    if path_kind is None:
        if not isinstance(path, str) or not path:
            raise AssertionError("fixture path is missing")
        return path
    if path is not None:
        raise AssertionError("fixture path and pathKind are mutually exclusive")
    mappings = {
        "sensitive-name-a": _chars(46, 101, 110, 118),
        "sensitive-name-b": _chars(105, 100, 95, 114, 115, 97),
    }
    try:
        return mappings[path_kind]
    except KeyError as exc:
        raise AssertionError(f"unknown fixture pathKind: {path_kind}") from exc


def _fixture_content(kind: str | None, content: str | None) -> str:
    if kind is None:
        return content or ""
    if content is not None:
        raise AssertionError("fixture content and contentKind are mutually exclusive")
    if kind == "signing-a":
        return (
            _chars(
                45, 45, 45, 45, 45, 66, 69, 71, 73, 78, 32,
                79, 80, 69, 78, 83, 83, 72, 32,
                80, 82, 73, 86, 65, 84, 69, 32, 75, 69, 89,
                45, 45, 45, 45, 45,
            )
            + "\nfixture-only\n"
        )
    if kind == "auth-a":
        return _chars(65, 75, 73, 65) + ("A" * 16) + "\n"
    if kind == "auth-b":
        return _chars(103, 104, 112, 95) + ("A" * 40) + "\n"
    if kind == "auth-c":
        return _chars(115, 107, 45) + ("A" * 40) + "\n"
    if kind == "auth-d":
        return _chars(120, 111, 120, 98, 45) + ("A" * 32) + "\n"
    raise AssertionError(f"unknown fixture contentKind: {kind}")


def _write_fixture_file(root: Path, record: dict) -> str:
    expected_fields = {"path", "pathKind", "content", "contentKind", "hexBytes"}
    if not isinstance(record, dict) or set(record) != expected_fields:
        raise AssertionError("fixture file record has unknown/missing fields")
    relative = _fixture_path(record["pathKind"], record["path"])
    pure = PurePosixPath(relative)
    if pure.is_absolute() or not pure.parts or any(part in {"", ".", ".."} for part in pure.parts):
        raise AssertionError("fixture path is unsafe")
    path = root.joinpath(*pure.parts)
    path.parent.mkdir(parents=True, exist_ok=True)
    supplied = sum(
        value is not None for value in (record["content"], record["contentKind"], record["hexBytes"])
    )
    if supplied > 1:
        raise AssertionError("fixture file defines multiple content sources")
    if record["hexBytes"] is not None:
        if not isinstance(record["hexBytes"], str):
            raise AssertionError("fixture hexBytes must be a string")
        path.write_bytes(bytes.fromhex(record["hexBytes"]))
    else:
        path.write_text(_fixture_content(record["contentKind"], record["content"]), encoding="utf-8")
    return relative


def _fixture_git(root: Path, *arguments: str) -> None:
    try:
        completed = subprocess.run(
            ["git", "-C", os.fspath(root), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise AssertionError(f"fixture Git command could not run: {exc}") from exc
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise AssertionError(f"fixture Git command failed: {arguments!r}: {detail}")


def self_test() -> None:
    try:
        fixture = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AssertionError(f"cannot load publication-hygiene fixture cases: {exc}") from exc
    if (
        not isinstance(fixture, dict)
        or fixture.get("schemaVersion") != 1
        or not isinstance(fixture.get("cases"), list)
    ):
        raise AssertionError("publication-hygiene fixture schema is invalid")

    seen_names: set[str] = set()
    for case in fixture["cases"]:
        if not isinstance(case, dict) or set(case) != {
            "name", "files", "untrackedFiles", "gateState", "expectedCategories",
        }:
            raise AssertionError("publication-hygiene fixture case fields are invalid")
        name = case["name"]
        if not isinstance(name, str) or not name or name in seen_names:
            raise AssertionError("publication-hygiene fixture case name is invalid/duplicate")
        seen_names.add(name)

        expected = case["expectedCategories"]
        if not isinstance(expected, list) or any(category not in ALL_CATEGORIES for category in expected):
            raise AssertionError(f"fixture {name}: expected category list is invalid")
        if not isinstance(case["files"], list) or not isinstance(case["untrackedFiles"], list):
            raise AssertionError(f"fixture {name}: file collections must be arrays")

        gate_value = case["gateState"]
        if not isinstance(gate_value, dict) or set(gate_value) != {
            "licenseResolved", "generalAvailabilityResolved", "manualGatesPending",
        }:
            raise AssertionError(f"fixture {name}: gateState is invalid")
        if any(not isinstance(value, bool) for value in gate_value.values()):
            raise AssertionError(f"fixture {name}: gateState contains non-boolean value")
        gates = GateState(
            gate_value["licenseResolved"],
            gate_value["generalAvailabilityResolved"],
            gate_value["manualGatesPending"],
        )

        with tempfile.TemporaryDirectory(prefix="hydraseat-publication-hygiene-") as temporary:
            root = Path(temporary) / "repo"
            root.mkdir()
            _fixture_git(root, "init", "-q")
            tracked_paths = [_write_fixture_file(root, record) for record in case["files"]]
            for record in case["untrackedFiles"]:
                _write_fixture_file(root, record)
            if tracked_paths:
                _fixture_git(root, "add", "--", *tracked_paths)

            findings, observed_gates, _count = scan_repository(root, gate_override=gates)
            if observed_gates != gates:
                raise AssertionError(f"fixture {name}: gate override changed")
            observed = sorted({finding.category for finding in findings})
            if observed != sorted(expected):
                raise AssertionError(
                    f"fixture {name}: expected {sorted(expected)!r}, observed {observed!r}; "
                    f"findings={[asdict(item) for item in findings]!r}"
                )

    gates = GateState(False, False, True)
    with tempfile.TemporaryDirectory(prefix="hydraseat-publication-index-") as temporary:
        root = Path(temporary) / "repo"
        root.mkdir()
        _fixture_git(root, "init", "-q")
        staged = root / "notes" / "staged.txt"
        staged.parent.mkdir(parents=True)
        staged.write_text(_fixture_content("auth-a", None), encoding="utf-8")
        _fixture_git(root, "add", "--", "notes/staged.txt")
        staged.write_text("clean worktree bytes after staging\n", encoding="utf-8")
        findings, _observed_gates, _count = scan_repository(root, gate_override=gates)
        if CAT_SENSITIVE_AUTH not in {finding.category for finding in findings}:
            raise AssertionError("staged/index bytes were not authoritative over later worktree replacement")

    with tempfile.TemporaryDirectory(prefix="hydraseat-publication-authority-") as temporary:
        root = Path(temporary) / "repo"
        root.mkdir()
        _fixture_git(root, "init", "-q")
        try:
            load_gate_state(root)
        except HygieneError:
            pass
        else:
            raise AssertionError("missing release-gate authority did not fail closed")

    print(
        f"Repository publication hygiene self-test passed: {len(seen_names)} deterministic cases "
        "+ staged-index and gate-authority boundary checks."
    )


def _json_result(findings: list[Finding], gates: GateState, tracked_count: int) -> dict:
    return {
        "schemaVersion": 1,
        "trackedFiles": tracked_count,
        "gateState": {
            "licenseResolved": gates.license_resolved,
            "generalAvailabilityResolved": gates.general_availability_resolved,
            "manualGatesPending": gates.manual_gates_pending,
        },
        "status": "PASS" if not findings else "BLOCKED",
        "blockerCategories": sorted({finding.category for finding in findings}),
        "findings": [asdict(finding) for finding in findings],
    }


def _print_human(findings: list[Finding], gates: GateState, tracked_count: int) -> None:
    print(
        "Repository publication hygiene: "
        + ("PASS" if not findings else "BLOCKED")
        + f"; tracked={tracked_count}; licenseResolved={str(gates.license_resolved).lower()}; "
        + f"gaResolved={str(gates.general_availability_resolved).lower()}; "
        + f"manualGatesPending={str(gates.manual_gates_pending).lower()}"
    )
    if findings:
        print("Blocker categories: " + ", ".join(sorted({finding.category for finding in findings})))
        for finding in findings:
            location = finding.path + (f":{finding.line}" if finding.line else "")
            print(f"  [{finding.category}] {location}: {finding.detail}")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description=(
            "Inspect only Git-tracked/index repository content for publication hygiene. "
            "No files are removed, rewritten, uploaded, or read from external authentication/reference stores."
        )
    )
    result.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    result.add_argument("--json", action="store_true", help="emit deterministic JSON")
    result.add_argument("--self-test", action="store_true", help="run deterministic fixture cases")
    return result


def main(argv: Sequence[str]) -> int:
    args = parser().parse_args(argv)
    try:
        if args.self_test:
            self_test()
            return 0
        findings, gates, tracked_count = scan_repository(args.root)
        if args.json:
            print(json.dumps(_json_result(findings, gates, tracked_count), sort_keys=True, indent=2))
        else:
            _print_human(findings, gates, tracked_count)
        return 0 if not findings else 1
    except (HygieneError, OSError, subprocess.SubprocessError) as exc:
        finding = _finding(CAT_AUTHORITY, "<repository>", str(exc))
        if args.json:
            print(
                json.dumps(
                    {
                        "schemaVersion": 1,
                        "status": "BLOCKED",
                        "blockerCategories": [CAT_AUTHORITY],
                        "findings": [asdict(finding)],
                    },
                    sort_keys=True,
                    indent=2,
                )
            )
        else:
            print(f"Repository publication hygiene: BLOCKED [{CAT_AUTHORITY}] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
