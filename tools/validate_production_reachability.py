#!/usr/bin/env python3
"""Validate bounded HydraSeat production build reachability from tracked CMake authority.

This is intentionally not a C++ linker or arbitrary source-code symbol resolver.  It
parses only the CMake constructs used by HydraSeat, then checks an explicit manifest
of production components/factories whose implementations must be reachable from
known production targets.  Generated build projects are never build authority.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Iterator, Sequence

ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PATH = ROOT / "tools" / "testdata" / "production_reachability" / "cases.json"

TARGET_KINDS = {"STATIC", "SHARED", "MODULE", "OBJECT", "INTERFACE", "UNKNOWN"}
EXECUTABLE_OPTIONS = {"WIN32", "MACOSX_BUNDLE", "EXCLUDE_FROM_ALL", "IMPORTED", "GLOBAL"}
LINK_KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE", "LINK_PUBLIC", "LINK_PRIVATE", "debug", "optimized", "general"}
SOURCE_SCOPE_KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE"}
VAR_TOKEN_RE = re.compile(r"^\$\{([A-Za-z_][A-Za-z0-9_]*)\}$")
COMMAND_NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
INTERNAL_TARGET_RE = re.compile(r"^(?:hydra_|hydraseat_|HydraSeat$)")
GENERATED_TOP_LEVEL_PREFIXES = (
    "build",
    "out",
    "cmake-build",
)
GENERATED_EXACT_PARTS = {
    ".git",
    ".vs",
    ".idea",
    ".ai-bridge",
    "CMakeFiles",
}


@dataclass
class Target:
    name: str
    kind: str
    sources: set[str] = field(default_factory=set)
    links: list[str] = field(default_factory=list)
    definition: str = ""


@dataclass(frozen=True)
class ProductionComponent:
    name: str
    implementation_sources: tuple[str, ...] = ()
    required_by_targets: tuple[str, ...] = ()
    allowed_owner_targets: tuple[str, ...] = ()
    required_owner_dependencies: tuple[str, ...] = ()
    required_owner_link_tokens: tuple[str, ...] = ()
    activate_if_paths_exist: tuple[str, ...] = ()
    header_only_headers: tuple[str, ...] = ()
    unique_production_owner: bool = True
    suggested_owner: str = ""
    suggested_insertion: str = ""

    @property
    def header_only(self) -> bool:
        return bool(self.header_only_headers) and not self.implementation_sources


@dataclass(frozen=True)
class ReachabilityError:
    code: str
    component: str
    message: str
    target: str = ""
    source: str = ""
    required_library: str = ""
    current_missing_edge: str = ""
    suggested_insertion: str = ""


@dataclass
class ValidationResult:
    errors: list[ReachabilityError] = field(default_factory=list)
    checked_components: list[str] = field(default_factory=list)
    skipped_components: list[str] = field(default_factory=list)

    def add(self, code: str, component: ProductionComponent, message: str, **fields: str) -> None:
        self.errors.append(
            ReachabilityError(code=code, component=component.name, message=message, **fields)
        )


@dataclass
class BuildGraph:
    targets: dict[str, Target]

    def owners(self, source: str) -> list[str]:
        normalized = normalize_source_token(source)
        return sorted(
            target.name for target in self.targets.values() if normalized in target.sources
        )

    def unknown_internal_dependencies(self, target_name: str) -> set[str]:
        if target_name not in self.targets:
            return set()
        unknown: set[str] = set()
        pending = [target_name]
        visited: set[str] = set()
        while pending:
            current = pending.pop()
            if current in visited:
                continue
            visited.add(current)
            target = self.targets.get(current)
            if target is None:
                continue
            for dependency in target.links:
                if dependency in self.targets:
                    pending.append(dependency)
                elif looks_internal_target(dependency):
                    unknown.add(dependency)
        return unknown

    def reachable_targets(self, target_name: str) -> set[str]:
        if target_name not in self.targets:
            return set()
        reachable: set[str] = set()
        pending = [target_name]
        while pending:
            current = pending.pop()
            if current in reachable or current not in self.targets:
                continue
            reachable.add(current)
            pending.extend(
                dependency
                for dependency in self.targets[current].links
                if dependency in self.targets
            )
        return reachable

    def reachable_sources(self, target_name: str) -> set[str]:
        sources: set[str] = set()
        for reachable in self.reachable_targets(target_name):
            sources.update(self.targets[reachable].sources)
        return sources


def looks_internal_target(token: str) -> bool:
    return bool(INTERNAL_TARGET_RE.match(token))


def is_test_target(target: Target) -> bool:
    lowered = target.name.lower()
    if "test" in lowered or lowered.endswith("_fixture") or lowered.endswith("_harness"):
        return True
    return bool(target.sources) and all(source.startswith("tests/") for source in target.sources)


def is_generated_authority_path(relative: Path) -> bool:
    parts = relative.parts
    if not parts:
        return False
    if parts[0] in GENERATED_EXACT_PARTS:
        return True
    if any(part in GENERATED_EXACT_PARTS for part in parts):
        return True
    first = parts[0].lower()
    return any(first == prefix or first.startswith(prefix + "-") for prefix in GENERATED_TOP_LEVEL_PREFIXES)


def cmake_authority_files(root: Path) -> list[Path]:
    candidates: set[Path] = set()
    for directory, dirnames, filenames in os.walk(root):
        current = Path(directory)
        relative_current = current.relative_to(root)
        if relative_current.parts and is_generated_authority_path(relative_current):
            dirnames[:] = []
            continue

        # Prune generated build/QA trees before os.walk descends into them. Merely
        # declining their CMakeLists after a recursive scan is both slow and too
        # close to treating generated build state as repository authority.
        kept_dirs: list[str] = []
        for name in dirnames:
            child_relative = relative_current / name
            if not is_generated_authority_path(child_relative):
                kept_dirs.append(name)
        dirnames[:] = kept_dirs

        if "CMakeLists.txt" in filenames:
            candidates.add(current / "CMakeLists.txt")
        if relative_current.parts and relative_current.parts[0] == "cmake":
            for name in filenames:
                if name.endswith(".cmake"):
                    candidates.add(current / name)

    return sorted(candidates, key=lambda path: path.relative_to(root).as_posix())


def strip_cmake_comments(text: str) -> str:
    output: list[str] = []
    quoted = False
    escaped = False
    index = 0
    while index < len(text):
        char = text[index]
        if escaped:
            output.append(char)
            escaped = False
            index += 1
            continue
        if char == "\\" and quoted:
            output.append(char)
            escaped = True
            index += 1
            continue
        if char == '"':
            quoted = not quoted
            output.append(char)
            index += 1
            continue
        if char == "#" and not quoted:
            while index < len(text) and text[index] not in "\r\n":
                index += 1
            continue
        output.append(char)
        index += 1
    return "".join(output)


def iter_cmake_commands(text: str) -> Iterator[tuple[str, str, int]]:
    text = strip_cmake_comments(text)
    index = 0
    while index < len(text):
        match = COMMAND_NAME_RE.search(text, index)
        if match is None:
            return
        name = match.group(0).lower()
        cursor = match.end()
        while cursor < len(text) and text[cursor].isspace():
            cursor += 1
        if cursor >= len(text) or text[cursor] != "(":
            index = match.end()
            continue
        start = cursor + 1
        depth = 1
        quoted = False
        escaped = False
        cursor = start
        while cursor < len(text) and depth:
            char = text[cursor]
            if escaped:
                escaped = False
            elif char == "\\" and quoted:
                escaped = True
            elif char == '"':
                quoted = not quoted
            elif not quoted:
                if char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
            cursor += 1
        if depth != 0:
            raise ValueError(f"unterminated CMake command {name} near offset {match.start()}")
        yield name, text[start : cursor - 1], match.start()
        index = cursor


def tokenize_cmake_arguments(body: str) -> list[str]:
    tokens: list[str] = []
    current: list[str] = []
    quoted = False
    escaped = False

    def finish() -> None:
        if current:
            token = "".join(current)
            current.clear()
            if ";" in token and not token.startswith("$<"):
                tokens.extend(piece for piece in token.split(";") if piece)
            else:
                tokens.append(token)

    for char in body:
        if escaped:
            current.append(char)
            escaped = False
            continue
        if char == "\\" and quoted:
            escaped = True
            continue
        if char == '"':
            quoted = not quoted
            continue
        if char.isspace() and not quoted:
            finish()
            continue
        current.append(char)
    if quoted:
        raise ValueError("unterminated quoted CMake argument")
    finish()
    return tokens


def expand_token(token: str, variables: dict[str, list[str]], stack: tuple[str, ...] = ()) -> list[str]:
    match = VAR_TOKEN_RE.match(token)
    if match is None:
        return [token]
    name = match.group(1)
    if name in stack:
        raise ValueError("recursive CMake variable expansion: " + " -> ".join((*stack, name)))
    values = variables.get(name)
    if values is None:
        return [token]
    expanded: list[str] = []
    for value in values:
        expanded.extend(expand_token(value, variables, (*stack, name)))
    return expanded


def expand_tokens(tokens: Iterable[str], variables: dict[str, list[str]]) -> list[str]:
    result: list[str] = []
    for token in tokens:
        result.extend(expand_token(token, variables))
    return result


def normalize_source_token(token: str) -> str:
    normalized = token.replace("\\", "/")
    for prefix in (
        "${CMAKE_CURRENT_SOURCE_DIR}/",
        "${PROJECT_SOURCE_DIR}/",
        "${CMAKE_SOURCE_DIR}/",
    ):
        if normalized.startswith(prefix):
            normalized = normalized[len(prefix) :]
    while normalized.startswith("./"):
        normalized = normalized[2:]
    return normalized


def parse_build_graph(root: Path) -> BuildGraph:
    variables: dict[str, list[str]] = {}
    targets: dict[str, Target] = {}
    authority_files = cmake_authority_files(root)
    if not authority_files:
        raise ValueError("no source CMake authority files found")

    for path in authority_files:
        relative_cmake = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        for command, body, offset in iter_cmake_commands(text):
            arguments = tokenize_cmake_arguments(body)
            if not arguments:
                continue
            if command == "set":
                variables[arguments[0]] = arguments[1:]
                continue
            if command == "list" and len(arguments) >= 3 and arguments[0].upper() == "APPEND":
                variables.setdefault(arguments[1], []).extend(arguments[2:])
                continue
            if command in {"add_library", "add_executable"}:
                name = arguments[0]
                if name in targets:
                    raise ValueError(f"duplicate CMake target definition: {name}")
                remaining = arguments[1:]
                kind = "EXECUTABLE" if command == "add_executable" else "LIBRARY"
                if command == "add_library" and remaining and remaining[0].upper() in TARGET_KINDS:
                    kind = remaining.pop(0).upper()
                if any(token.upper() in {"ALIAS", "IMPORTED"} for token in remaining[:2]):
                    # Imported/alias targets do not own repository implementation sources.
                    continue
                while command == "add_executable" and remaining and remaining[0].upper() in EXECUTABLE_OPTIONS:
                    remaining.pop(0)
                sources = {
                    normalize_source_token(token)
                    for token in expand_tokens(remaining, variables)
                    if token and not token.startswith("$<")
                }
                line = text.count("\n", 0, offset) + 1
                targets[name] = Target(
                    name=name,
                    kind=kind,
                    sources=sources,
                    definition=f"{relative_cmake}:{line}",
                )
                continue
            if command == "target_sources" and len(arguments) >= 2:
                name = arguments[0]
                target = targets.get(name)
                if target is None:
                    raise ValueError(f"target_sources references unknown target {name}")
                remaining = [token for token in arguments[1:] if token.upper() not in SOURCE_SCOPE_KEYWORDS]
                target.sources.update(
                    normalize_source_token(token)
                    for token in expand_tokens(remaining, variables)
                    if token and not token.startswith("$<")
                )
                continue
            if command == "target_link_libraries" and len(arguments) >= 2:
                name = arguments[0]
                target = targets.get(name)
                if target is None:
                    raise ValueError(f"target_link_libraries references unknown target {name}")
                dependencies = expand_tokens(arguments[1:], variables)
                for dependency in dependencies:
                    if dependency in LINK_KEYWORDS or dependency.startswith("$<") or not dependency:
                        continue
                    target.links.append(dependency)
                continue

    return BuildGraph(targets=targets)


def real_components() -> tuple[ProductionComponent, ...]:
    return (
        ProductionComponent(
            name="trusted-runtime-requirement-source",
            implementation_sources=("src/game_runtime_requirement_resolver.cpp",),
            required_by_targets=("HydraSeat", "hydra_host"),
            allowed_owner_targets=("hydra_game_runtime_requirement_resolver",),
            required_owner_dependencies=(
                "hydra_steam_provider",
                "hydra_custom_executable_provider",
                "hydra_community_submission",
            ),
            required_owner_link_tokens=("Bcrypt.lib",),
            activate_if_paths_exist=(
                "include/hydra/game_runtime_requirement_resolver.hpp",
                "src/game_runtime_requirement_resolver.cpp",
            ),
            suggested_owner="hydra_game_runtime_requirement_resolver",
            suggested_insertion=(
                "link hydra_game_runtime_requirement_resolver to hydra_steam_provider, "
                "hydra_custom_executable_provider, and hydra_community_submission; then link "
                "HydraSeat and hydra_host to hydra_game_runtime_requirement_resolver"
            ),
        ),
        ProductionComponent(
            name="production-launch-runtime",
            implementation_sources=("src/production_launch_runtime.cpp",),
            required_by_targets=("HydraSeat", "hydra_host"),
            allowed_owner_targets=("hydra_production_launch_runtime",),
            required_owner_dependencies=(
                "hydra_two_seat_launch",
                "hydra_provider_launch_plan",
                "hydra_window_placement",
                "hydra_controller_runtime",
                "hydra_audio_routing",
            ),
            activate_if_paths_exist=(
                "include/hydra/production_launch_runtime.hpp",
                "src/production_launch_runtime.cpp",
            ),
            suggested_owner="hydra_production_launch_runtime",
            suggested_insertion="keep production launch runtime in its canonical library and link production consumers through that target",
        ),
        ProductionComponent(
            name="production-launch-installer",
            implementation_sources=("src/production_launch_installer.cpp",),
            required_by_targets=("HydraSeat",),
            allowed_owner_targets=("hydra_production_launch_installer",),
            required_owner_dependencies=(
                "hydra_seat_launcher",
                "hydra_production_launch_runtime",
            ),
            activate_if_paths_exist=(
                "include/hydra/production_launch_installer.hpp",
                "src/production_launch_installer.cpp",
            ),
            suggested_owner="hydra_production_launch_installer",
            suggested_insertion="link HydraSeat to hydra_production_launch_installer at the GUI production composition target",
        ),
        ProductionComponent(
            name="production-compatibility-activation",
            implementation_sources=("src/production_compatibility_activation.cpp",),
            required_by_targets=("hydra_production_launch_runtime",),
            allowed_owner_targets=(
                "hydra_production_compatibility_activation",
                "hydra_production_launch_runtime",
            ),
            required_owner_dependencies=(
                "hydra_provider_launch_plan",
                "hydra_two_seat_launch",
            ),
            activate_if_paths_exist=("src/production_compatibility_activation.cpp",),
            suggested_owner="hydra_production_compatibility_activation or hydra_production_launch_runtime",
            suggested_insertion=(
                "register src/production_compatibility_activation.cpp in a canonical production target "
                "and make hydra_production_launch_runtime reach it"
            ),
        ),
        ProductionComponent(
            name="production-activation-bridges",
            implementation_sources=("src/production_activation_bridges.cpp",),
            required_by_targets=("hydra_host",),
            allowed_owner_targets=(
                "hydra_production_activation_bridges",
                "hydra_host",
            ),
            activate_if_paths_exist=("src/production_activation_bridges.cpp",),
            suggested_owner="hydra_production_activation_bridges or hydra_host",
            suggested_insertion=(
                "register src/production_activation_bridges.cpp in a production target and make hydra_host reach it"
            ),
        ),
        ProductionComponent(
            name="recovery-process-attachment-authority",
            implementation_sources=("src/watchdog_protocol.cpp",),
            required_by_targets=("hydra_watchdog", "hydra_reset", "hydra_gate_c_recovery"),
            allowed_owner_targets=("hydra_watchdog_core",),
            activate_if_paths_exist=("include/hydra/recovery_process_attachment.hpp",),
            suggested_owner="hydra_watchdog_core",
            suggested_insertion="keep recovery attachment implementation owned by hydra_watchdog_core and linked by recovery consumers",
        ),
        ProductionComponent(
            name="acceptance-campaign-core",
            implementation_sources=("src/acceptance_campaign.cpp",),
            required_by_targets=("hydraseat_acceptance_campaignctl",),
            allowed_owner_targets=("hydra_acceptance_campaign",),
            activate_if_paths_exist=("src/acceptance_campaign.cpp",),
            suggested_owner="hydra_acceptance_campaign",
            suggested_insertion="link hydraseat_acceptance_campaignctl to hydra_acceptance_campaign",
        ),
        ProductionComponent(
            name="acceptance-probe-core",
            implementation_sources=("src/acceptance_probe.cpp",),
            required_by_targets=("hydraseat_acceptance_probe",),
            allowed_owner_targets=("hydra_acceptance_probe",),
            activate_if_paths_exist=("src/acceptance_probe.cpp",),
            suggested_owner="hydra_acceptance_probe",
            suggested_insertion="link hydraseat_acceptance_probe to hydra_acceptance_probe",
        ),
    )


def component_is_active(root: Path, component: ProductionComponent) -> bool:
    if component.activate_if_paths_exist:
        return any((root / path).exists() for path in component.activate_if_paths_exist)
    return True


def validate_component(
    root: Path,
    graph: BuildGraph,
    component: ProductionComponent,
    result: ValidationResult,
) -> None:
    if not component_is_active(root, component):
        result.skipped_components.append(component.name)
        return
    result.checked_components.append(component.name)

    if component.header_only:
        for header in component.header_only_headers:
            if not (root / header).is_file():
                result.add(
                    "MISSING_HEADER_ONLY_CONTRACT",
                    component,
                    f"explicit header-only production contract is missing {header}",
                    source=header,
                    suggested_insertion=component.suggested_insertion,
                )
        for target_name in component.required_by_targets:
            if target_name not in graph.targets:
                result.add(
                    "MISSING_TARGET",
                    component,
                    f"required production target {target_name} is not defined",
                    target=target_name,
                    suggested_insertion=component.suggested_insertion,
                )
        return

    source_owners: dict[str, list[str]] = {}
    for source in component.implementation_sources:
        if not (root / source).is_file():
            result.add(
                "MISSING_IMPLEMENTATION_SOURCE",
                component,
                f"required implementation source {source} does not exist",
                source=source,
                required_library=component.suggested_owner,
                suggested_insertion=component.suggested_insertion,
            )
        owners = graph.owners(source)
        source_owners[source] = owners
        production_owners = [
            owner for owner in owners if not is_test_target(graph.targets[owner])
        ]
        test_owners = [owner for owner in owners if is_test_target(graph.targets[owner])]
        if not owners:
            result.add(
                "SOURCE_UNREGISTERED",
                component,
                f"implementation {source} exists but no source CMake target owns it",
                source=source,
                required_library=component.suggested_owner,
                current_missing_edge="source exists in repository but is absent from tracked target sources",
                suggested_insertion=component.suggested_insertion,
            )
        elif not production_owners:
            result.add(
                "TEST_ONLY_SATISFACTION",
                component,
                f"implementation {source} is registered only by test targets: {', '.join(test_owners)}",
                source=source,
                required_library=component.suggested_owner,
                current_missing_edge="tests can compile the source but no production target owns it",
                suggested_insertion=component.suggested_insertion,
            )
        if production_owners and component.allowed_owner_targets:
            unexpected = sorted(set(production_owners) - set(component.allowed_owner_targets))
            if unexpected:
                result.add(
                    "MISOWNED_SOURCE",
                    component,
                    f"implementation {source} is owned by unexpected production target(s): {', '.join(unexpected)}",
                    source=source,
                    required_library=" | ".join(component.allowed_owner_targets),
                    current_missing_edge="implementation is registered under the wrong production role",
                    suggested_insertion=component.suggested_insertion,
                )
        if component.unique_production_owner and len(production_owners) > 1:
            result.add(
                "DUPLICATE_PRODUCTION_OWNER",
                component,
                f"implementation {source} has incompatible multiple production owners: {', '.join(production_owners)}",
                source=source,
                required_library=" | ".join(component.allowed_owner_targets),
                current_missing_edge="one canonical production implementation is compiled into multiple standalone roles",
                suggested_insertion=component.suggested_insertion,
            )

    for target_name in component.required_by_targets:
        target = graph.targets.get(target_name)
        if target is None:
            result.add(
                "MISSING_TARGET",
                component,
                f"required production target {target_name} is not defined",
                target=target_name,
                required_library=component.suggested_owner,
                current_missing_edge="production entrypoint/target definition is absent",
                suggested_insertion=component.suggested_insertion,
            )
            continue
        unknown = sorted(graph.unknown_internal_dependencies(target_name))
        for dependency in unknown:
            result.add(
                "UNKNOWN_INTERNAL_DEPENDENCY",
                component,
                f"production closure for {target_name} references undefined internal target {dependency}",
                target=target_name,
                required_library=dependency,
                current_missing_edge=f"{target_name} link closure cannot be determined through undefined target {dependency}",
                suggested_insertion="define the canonical internal dependency or correct the target_link_libraries edge",
            )
        reachable_sources = graph.reachable_sources(target_name)
        for source in component.implementation_sources:
            if normalize_source_token(source) in reachable_sources:
                continue
            owners = source_owners.get(source, [])
            result.add(
                "UNREACHABLE_IMPLEMENTATION",
                component,
                (
                    f"implementation {source} is not reachable from production target {target_name}; "
                    f"current owners: {', '.join(owners) if owners else 'none'}"
                ),
                target=target_name,
                source=source,
                required_library=component.suggested_owner,
                current_missing_edge=(
                    f"{target_name} target source/link closure omits {source}"
                ),
                suggested_insertion=component.suggested_insertion,
            )

    if component.required_owner_dependencies:
        production_owners: set[str] = set()
        for owners in source_owners.values():
            for owner in owners:
                if owner in graph.targets and not is_test_target(graph.targets[owner]):
                    production_owners.add(owner)
        preferred = [
            owner for owner in component.allowed_owner_targets if owner in production_owners
        ]
        owners_to_check = preferred or sorted(production_owners)
        if owners_to_check:
            for dependency in component.required_owner_dependencies:
                if dependency not in graph.targets:
                    result.add(
                        "MISSING_DEPENDENCY_TARGET",
                        component,
                        f"required production dependency target {dependency} is not defined",
                        target=owners_to_check[0],
                        source=component.implementation_sources[0] if component.implementation_sources else "",
                        required_library=dependency,
                        current_missing_edge="component implementation depends on a missing canonical production target",
                        suggested_insertion=component.suggested_insertion,
                    )
                    continue
                if not any(dependency in graph.reachable_targets(owner) for owner in owners_to_check):
                    result.add(
                        "MISSING_OWNER_DEPENDENCY",
                        component,
                        (
                            f"component owner(s) {', '.join(owners_to_check)} cannot reach required dependency {dependency}"
                        ),
                        target=owners_to_check[0],
                        source=component.implementation_sources[0] if component.implementation_sources else "",
                        required_library=dependency,
                        current_missing_edge=(
                            f"{owners_to_check[0]} does not link {dependency} in its tracked production closure"
                        ),
                        suggested_insertion=component.suggested_insertion,
                    )
            for token in component.required_owner_link_tokens:
                if not any(token in graph.targets[owner].links for owner in owners_to_check):
                    result.add(
                        "MISSING_OWNER_LINK_TOKEN",
                        component,
                        f"component owner(s) {', '.join(owners_to_check)} do not link required platform library {token}",
                        target=owners_to_check[0],
                        source=component.implementation_sources[0] if component.implementation_sources else "",
                        required_library=token,
                        current_missing_edge=(
                            f"{owners_to_check[0]} target_link_libraries omits required platform token {token}"
                        ),
                        suggested_insertion=component.suggested_insertion,
                    )


def validate_repository(
    root: Path,
    components: Sequence[ProductionComponent] | None = None,
) -> tuple[ValidationResult, BuildGraph]:
    root = root.resolve()
    graph = parse_build_graph(root)
    result = ValidationResult()
    for component in components if components is not None else real_components():
        validate_component(root, graph, component, result)
    return result, graph


def print_result(result: ValidationResult, root: Path) -> None:
    if result.errors:
        print(
            f"Production reachability INVALID: {len(result.errors)} error(s), "
            f"{len(result.checked_components)} component(s) checked, "
            f"{len(result.skipped_components)} inactive component(s).",
            file=sys.stderr,
        )
        for error in result.errors:
            print(f"\nERROR [{error.code}]: component {error.component}", file=sys.stderr)
            print(error.message, file=sys.stderr)
            if error.target:
                print(f"TARGET: {error.target}", file=sys.stderr)
            if error.source:
                print(f"SOURCE: {error.source}", file=sys.stderr)
            if error.required_library:
                print(f"REQUIRED LIBRARY: {error.required_library}", file=sys.stderr)
            if error.current_missing_edge:
                print(f"CURRENT MISSING EDGE: {error.current_missing_edge}", file=sys.stderr)
            if error.suggested_insertion:
                print(f"SUGGESTED CMake insertion location: {error.suggested_insertion}", file=sys.stderr)
        if result.skipped_components:
            print(
                "\nInactive optional-yet production modules: "
                + ", ".join(sorted(result.skipped_components)),
                file=sys.stderr,
            )
        return
    print(
        f"Production reachability valid: {len(result.checked_components)} component(s) checked; "
        f"{len(result.skipped_components)} inactive component(s); root={root}"
    )


def component_from_fixture(data: dict[str, object]) -> ProductionComponent:
    def strings(name: str) -> tuple[str, ...]:
        value = data.get(name, [])
        if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
            raise ValueError(f"fixture component field {name} must be a string list")
        return tuple(value)

    name = data.get("name")
    if not isinstance(name, str) or not name:
        raise ValueError("fixture component name is required")
    unique = data.get("unique_production_owner", True)
    if not isinstance(unique, bool):
        raise ValueError("fixture unique_production_owner must be boolean")
    suggested_owner = data.get("suggested_owner", "")
    suggested_insertion = data.get("suggested_insertion", "fixture insertion")
    if not isinstance(suggested_owner, str) or not isinstance(suggested_insertion, str):
        raise ValueError("fixture suggestion fields must be strings")
    return ProductionComponent(
        name=name,
        implementation_sources=strings("implementation_sources"),
        required_by_targets=strings("required_by_targets"),
        allowed_owner_targets=strings("allowed_owner_targets"),
        required_owner_dependencies=strings("required_owner_dependencies"),
        required_owner_link_tokens=strings("required_owner_link_tokens"),
        activate_if_paths_exist=strings("activate_if_paths_exist"),
        header_only_headers=strings("header_only_headers"),
        unique_production_owner=unique,
        suggested_owner=suggested_owner,
        suggested_insertion=suggested_insertion,
    )


def write_fixture_tree(root: Path, files: dict[str, str]) -> None:
    for relative, content in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")


def run_self_tests(fixture_path: Path = FIXTURE_PATH) -> None:
    data = json.loads(fixture_path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise ValueError("production reachability fixture schema_version must be 1")
    cases = data.get("cases")
    if not isinstance(cases, list) or len(cases) != 12:
        raise ValueError("production reachability self-test requires exactly 12 reviewed cases")

    failures: list[str] = []
    seen_names: set[str] = set()
    temp_parent = Path(tempfile.mkdtemp(prefix="hydraseat-production-reachability-"))
    try:
        for index, case in enumerate(cases):
            if not isinstance(case, dict):
                failures.append(f"case {index}: fixture case must be an object")
                continue
            name = case.get("name")
            expect = case.get("expect")
            files = case.get("files")
            components_data = case.get("components")
            expected_codes = case.get("expected_error_codes", [])
            if (
                not isinstance(name, str)
                or not name
                or name in seen_names
                or expect not in {"pass", "fail"}
                or not isinstance(files, dict)
                or not all(isinstance(key, str) and isinstance(value, str) for key, value in files.items())
                or not isinstance(components_data, list)
                or not isinstance(expected_codes, list)
                or not all(isinstance(code, str) for code in expected_codes)
            ):
                failures.append(f"case {index}: invalid fixture metadata")
                continue
            seen_names.add(name)
            case_root = temp_parent / f"case-{index:02d}-{name}"
            case_root.mkdir()
            write_fixture_tree(case_root, files)
            components = tuple(component_from_fixture(item) for item in components_data)
            try:
                result, _ = validate_repository(case_root, components)
            except (OSError, UnicodeError, ValueError) as exc:
                failures.append(f"{name}: validator raised unexpectedly: {exc}")
                continue
            actual_codes = {error.code for error in result.errors}
            if expect == "pass" and result.errors:
                failures.append(
                    f"{name}: expected pass but got "
                    + ", ".join(f"{error.code}:{error.component}" for error in result.errors)
                )
            elif expect == "fail" and not result.errors:
                failures.append(f"{name}: expected failure but validator passed")
            missing_codes = sorted(set(expected_codes) - actual_codes)
            if missing_codes:
                failures.append(
                    f"{name}: expected error code(s) not observed: {', '.join(missing_codes)}; "
                    f"actual={', '.join(sorted(actual_codes)) or 'none'}"
                )
    finally:
        shutil.rmtree(temp_parent, ignore_errors=True)

    if failures:
        raise ValueError("production reachability self-test failed:\n  " + "\n  ".join(failures))
    print(f"Production reachability self-test passed: {len(cases)}/12 cases.")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--self-test", action="store_true", help="run the 12 deterministic fixture cases")
    parser.add_argument("--fixture", type=Path, default=FIXTURE_PATH, help=argparse.SUPPRESS)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.self_test:
            run_self_tests(args.fixture)
            return 0
        result, _ = validate_repository(args.root)
        print_result(result, args.root.resolve())
        return 1 if result.errors else 0
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        print(f"Production reachability validator failed closed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
