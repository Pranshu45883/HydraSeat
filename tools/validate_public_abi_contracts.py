#!/usr/bin/env python3
"""Validate bounded HydraSeat public ABI and wire-protocol architecture contracts."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PATH = ROOT / "tools" / "testdata" / "public_abi_contracts" / "cases.json"

FIXED_C_SCALAR_BYTES = {
    "uint8_t": 1,
    "int8_t": 1,
    "uint16_t": 2,
    "int16_t": 2,
    "uint32_t": 4,
    "int32_t": 4,
    "uint64_t": 8,
    "int64_t": 8,
}
UNSIGNED_FIXED_C_TYPES = {"uint8_t", "uint16_t", "uint32_t", "uint64_t"}
NATIVE_TYPE_PATTERNS = (
    ("size_t", re.compile(r"\b(?:std::)?size_t\b")),
    ("ptrdiff_t", re.compile(r"\b(?:std::)?ptrdiff_t\b")),
    ("uintptr_t", re.compile(r"\b(?:std::)?uintptr_t\b")),
    ("intptr_t", re.compile(r"\b(?:std::)?intptr_t\b")),
    ("ssize_t", re.compile(r"\bssize_t\b")),
    ("unsigned long", re.compile(r"\bunsigned\s+long\b")),
    ("long", re.compile(r"(?<!unsigned\s)\blong\b")),
    ("unsigned int", re.compile(r"\bunsigned\s+int\b")),
    ("int", re.compile(r"(?<!unsigned\s)\bint\b")),
    ("unsigned short", re.compile(r"\bunsigned\s+short\b")),
    ("short", re.compile(r"(?<!unsigned\s)\bshort\b")),
    ("HANDLE", re.compile(r"\bHANDLE\b")),
    ("HWND", re.compile(r"\bHWND\b")),
    ("WPARAM", re.compile(r"\bWPARAM\b")),
    ("LPARAM", re.compile(r"\bLPARAM\b")),
    ("DWORD_PTR", re.compile(r"\bDWORD_PTR\b")),
    ("ULONG_PTR", re.compile(r"\bULONG_PTR\b")),
)

C_STRUCT_RE = re.compile(
    r"typedef\s+struct\s+(?P<tag>[A-Za-z_]\w*)\s*\{(?P<body>.*?)\}\s*(?P<alias>[A-Za-z_]\w*)\s*;",
    re.DOTALL,
)
CPP_STRUCT_RE = re.compile(
    r"\bstruct\s+(?P<name>[A-Za-z_]\w*)\s*\{(?P<body>.*?)\n\};",
    re.DOTALL,
)
ENUM_CLASS_RE = re.compile(
    r"\benum\s+class\s+(?P<name>[A-Za-z_]\w*)(?:\s*:\s*(?P<underlying>[^\{]+))?\s*\{",
    re.DOTALL,
)
DEFINE_INT_RE = re.compile(
    r"(?m)^\s*#\s*define\s+(?P<name>[A-Za-z_]\w*)\s+(?P<value>(?:0[xX][0-9A-Fa-f]+|[0-9]+)[uUlL]*)\s*$"
)
EXPORTED_DECL_TEMPLATE = r"(?ms)^[ \t]*{marker}\b(?P<decl>.*?;)"


@dataclass(frozen=True)
class EvidenceRequirement:
    path: str
    pattern: str
    description: str
    code: str


@dataclass(frozen=True)
class AbiStructSpec:
    name: str
    size_macro: str
    reserved_evidence_patterns: tuple[str, ...] = ()


@dataclass(frozen=True)
class CAbiContract:
    name: str
    header: str
    struct_prefix: str
    api_version_macro: str
    export_marker: str
    structs: tuple[AbiStructSpec, ...]
    layout_evidence_files: tuple[str, ...]
    reserved_evidence_file: str = ""
    native_param_allowlist: tuple[str, ...] = ()
    extra_evidence: tuple[EvidenceRequirement, ...] = ()


@dataclass(frozen=True)
class WireStructSource:
    path: str
    names: tuple[str, ...] = ()
    scan_all: bool = False


@dataclass(frozen=True)
class WireEnumSource:
    path: str
    names: tuple[str, ...] = ()
    scan_all: bool = False


@dataclass(frozen=True)
class WireContract:
    name: str
    struct_sources: tuple[WireStructSource, ...]
    enum_sources: tuple[WireEnumSource, ...]
    requirements: tuple[EvidenceRequirement, ...]
    evidence: tuple[EvidenceRequirement, ...] = ()


@dataclass(frozen=True)
class AuditError:
    code: str
    contract: str
    path: str
    message: str


@dataclass
class AuditResult:
    errors: list[AuditError] = field(default_factory=list)
    checked_contracts: list[str] = field(default_factory=list)
    checked_c_structs: int = 0
    checked_wire_structs: int = 0
    intentional_native_exceptions: list[str] = field(default_factory=list)

    def add(self, code: str, contract: str, path: str, message: str) -> None:
        self.errors.append(AuditError(code=code, contract=contract, path=path, message=message))


@dataclass(frozen=True)
class CField:
    type_name: str
    name: str
    array_count_token: str = ""


@dataclass(frozen=True)
class ParsedCStruct:
    name: str
    body: str
    fields: tuple[CField, ...]
    start: int
    end: int


def evidence(path: str, pattern: str, description: str, code: str = "EVIDENCE_MISSING") -> EvidenceRequirement:
    return EvidenceRequirement(path=path, pattern=pattern, description=description, code=code)


def abi_struct(name: str, size_macro: str, *reserved_patterns: str) -> AbiStructSpec:
    return AbiStructSpec(name=name, size_macro=size_macro, reserved_evidence_patterns=tuple(reserved_patterns))


ADAPTER_ABI = CAbiContract(
    name="gate-c-adapter-c-abi",
    header="include/hydra/gate_c_adapter.h",
    struct_prefix="HydraGateCAdapter",
    api_version_macro="HYDRA_GATE_C_ADAPTER_API_VERSION",
    export_marker="HYDRA_GATE_C_ADAPTER_API",
    structs=(
        abi_struct("HydraGateCAdapterInputEventV1", "HYDRA_GATE_C_ADAPTER_INPUT_EVENT_V1_BYTES", r"eventData->reserved0\s*!=\s*0"),
        abi_struct("HydraGateCAdapterControlStateV1", "HYDRA_GATE_C_ADAPTER_CONTROL_STATE_V1_BYTES", r"controlData->reserved0\s*!=\s*0"),
        abi_struct("HydraGateCAdapterSnapshotV1", "HYDRA_GATE_C_ADAPTER_SNAPSHOT_V1_BYTES"),
        abi_struct("HydraGateCAdapterClipRectV2", "HYDRA_GATE_C_ADAPTER_CLIP_RECT_V2_BYTES", r"allZero\(clip->reserved0\)"),
        abi_struct("HydraGateCAdapterWindowStateV2", "HYDRA_GATE_C_ADAPTER_WINDOW_STATE_V2_BYTES", r"windowState->reserved0\s*!=\s*0"),
        abi_struct("HydraGateCAdapterRawRegistrationV3", "HYDRA_GATE_C_ADAPTER_RAW_REGISTRATION_V3_BYTES", r"allZero\(value\.reserved0\)"),
        abi_struct("HydraGateCAdapterRawRegistrationEntryV3", "HYDRA_GATE_C_ADAPTER_RAW_REGISTRATION_ENTRY_V3_BYTES"),
        abi_struct("HydraGateCAdapterRawDeliveryV3", "HYDRA_GATE_C_ADAPTER_RAW_DELIVERY_V3_BYTES"),
        abi_struct("HydraGateCAdapterXInputSourceV4", "HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_V4_BYTES", r"allZero\(sourceData->reserved0\)"),
        abi_struct("HydraGateCAdapterXInputMappingV4", "HYDRA_GATE_C_ADAPTER_XINPUT_MAPPING_V4_BYTES", r"mapping->reserved0\s*!=\s*0"),
        abi_struct("HydraGateCAdapterXInputSourceStateV4", "HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_STATE_V4_BYTES", r"allZero\(state->reserved0\)"),
        abi_struct("HydraGateCAdapterXInputStateV4", "HYDRA_GATE_C_ADAPTER_XINPUT_STATE_V4_BYTES"),
        abi_struct("HydraGateCAdapterXInputSourceCapabilitiesV4", "HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_CAPABILITIES_V4_BYTES", r"allZero\(capabilities->reserved0\)"),
        abi_struct("HydraGateCAdapterXInputCapabilitiesV4", "HYDRA_GATE_C_ADAPTER_XINPUT_CAPABILITIES_V4_BYTES"),
        abi_struct("HydraGateCAdapterXInputSourceBatteryV4", "HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_BATTERY_V4_BYTES", r"allZero\(battery->reserved0\)"),
        abi_struct("HydraGateCAdapterXInputBatteryV4", "HYDRA_GATE_C_ADAPTER_XINPUT_BATTERY_V4_BYTES"),
        abi_struct("HydraGateCAdapterXInputVibrationV4", "HYDRA_GATE_C_ADAPTER_XINPUT_VIBRATION_V4_BYTES", r"vibration->reserved0\s*!=\s*0"),
    ),
    layout_evidence_files=(
        "tests/test_gate_c_architecture.cpp",
        "tests/test_gate_c_adapter.cpp",
        "tests/test_gate_c_adapter_c.c",
        "tests/test_gate_c_xinput_adapter.cpp",
    ),
    reserved_evidence_file="src/gate_c_adapter.cpp",
    native_param_allowlist=("hydra_gate_c_adapter_get_keyboard_state:size_t",),
    extra_evidence=(
        evidence("tests/test_gate_c_architecture.cpp", r"ProcessArchitecture::X86", "Gate-C architecture test must retain explicit x86 coverage", "CABI_ARCH_EVIDENCE"),
        evidence("tests/test_gate_c_architecture.cpp", r"ProcessArchitecture::X64", "Gate-C architecture test must retain explicit x64 coverage", "CABI_ARCH_EVIDENCE"),
        evidence("tests/test_gate_c_adapter_c.c", r"HYDRA_GATE_C_ADAPTER_API_VERSION", "adapter C-language smoke test must compile the public API version", "CABI_C_EVIDENCE"),
    ),
)

SHIM_ABI = CAbiContract(
    name="gate-c-shim-c-abi",
    header="include/hydra/gate_c_shim_api.h",
    struct_prefix="HydraGateCShim",
    api_version_macro="HYDRA_GATE_C_SHIM_API_VERSION",
    export_marker="HYDRA_GATE_C_SHIM_API",
    structs=(
        abi_struct("HydraGateCShimConfigV1", "HYDRA_GATE_C_SHIM_CONFIG_V1_BYTES"),
        abi_struct("HydraGateCShimConfigV2", "HYDRA_GATE_C_SHIM_CONFIG_V2_BYTES", r"v3\.reserved0\s*=\s*config->reserved0"),
        abi_struct("HydraGateCShimConfigV3", "HYDRA_GATE_C_SHIM_CONFIG_V3_BYTES", r"config->reserved0\s*!=\s*0", r"config->reserved1\s*!=\s*0"),
        abi_struct("HydraGateCShimStatusV1", "HYDRA_GATE_C_SHIM_STATUS_V1_BYTES"),
    ),
    layout_evidence_files=(
        "tests/test_gate_c_polling_shim.cpp",
        "tests/test_gate_c_shim_c.c",
    ),
    reserved_evidence_file="src/gate_c_shim.cpp",
    extra_evidence=(
        evidence("tests/test_gate_c_shim_c.c", r"HYDRA_GATE_C_SHIM_API_VERSION", "shim C-language smoke test must compile the public API version", "CABI_C_EVIDENCE"),
    ),
)

GATE_WIRE = WireContract(
    name="gate-c-wire-protocol",
    struct_sources=(
        WireStructSource("include/hydra/gate_c_protocol.hpp", scan_all=True),
        WireStructSource(
            "include/hydra/virtual_xinput_state.hpp",
            names=(
                "ControllerSourceIdentity",
                "NormalizedXInputGamepad",
                "NormalizedXInputCapabilities",
                "NormalizedXInputBattery",
                "VirtualXInputMapping",
                "VirtualXInputState",
                "VirtualXInputCapabilities",
                "VirtualXInputBattery",
                "VirtualXInputVibrationRequest",
                "VirtualXInputVibrationRoute",
            ),
        ),
    ),
    enum_sources=(
        WireEnumSource("include/hydra/gate_c_protocol.hpp", scan_all=True),
        WireEnumSource(
            "include/hydra/virtual_xinput_state.hpp",
            names=(
                "ControllerSourceKind",
                "XInputCapabilityType",
                "XInputBatteryDeviceType",
                "XInputBatteryType",
                "XInputBatteryLevel",
                "VirtualXInputResult",
            ),
        ),
    ),
    requirements=(
        evidence("include/hydra/gate_c_protocol.hpp", r"std::uint32_t\s+kProtocolMagic\s*=\s*0x[0-9A-Fa-f]+u", "Gate-C wire magic must be fixed-width", "WIRE_CONSTRAINT"),
        evidence("include/hydra/gate_c_protocol.hpp", r"std::uint16_t\s+kProtocolVersion\s*=\s*[0-9]+u?", "Gate-C protocol version must be explicit and fixed-width", "WIRE_CONSTRAINT"),
        evidence("include/hydra/gate_c_protocol.hpp", r"kFrameHeaderBytes\s*=\s*[0-9]+u?\s*;", "Gate-C frame header byte count must be explicit", "WIRE_CONSTRAINT"),
        evidence("include/hydra/gate_c_protocol.hpp", r"kMaximumPayloadBytes\s*=\s*[0-9]+u?\s*;", "Gate-C payload maximum must be explicit", "WIRE_CONSTRAINT"),
    ),
    evidence=(
        evidence("tests/test_gate_c_protocol.cpp", r"unsupported protocol version is rejected", "Gate-C focused test must reject future protocol versions", "WIRE_VERSION_EVIDENCE"),
        evidence("tests/test_gate_c_protocol.cpp", r"controller reserved fields must be zero", "Gate-C focused test must reject nonzero reserved controller bytes", "WIRE_RESERVED_EVIDENCE"),
        evidence("tests/test_gate_c_protocol.cpp", r"fixed-width source identity", "Gate-C controller wire test must retain fixed-width identity coverage", "WIRE_ARCH_EVIDENCE"),
        evidence("tests/test_gate_c_protocol.cpp", r"without raw XINPUT structs", "Gate-C wire state must not serialize native XINPUT layouts", "WIRE_ARCH_EVIDENCE"),
    ),
)

HOST_WIRE = WireContract(
    name="host-wire-protocol",
    struct_sources=(WireStructSource("include/hydra/host_protocol.hpp", scan_all=True),),
    enum_sources=(WireEnumSource("include/hydra/host_protocol.hpp", scan_all=True),),
    requirements=(
        evidence("include/hydra/host_protocol.hpp", r"std::uint32_t\s+kHostProtocolMagic\s*=\s*0x[0-9A-Fa-f]+u", "Host wire magic must be fixed-width", "WIRE_CONSTRAINT"),
        evidence("include/hydra/host_protocol.hpp", r"std::uint16_t\s+kHostProtocolVersion\s*=\s*[0-9]+u?", "Host protocol version must be explicit and fixed-width", "WIRE_CONSTRAINT"),
        evidence("include/hydra/host_protocol.hpp", r"kHostProtocolHeaderBytes\s*=\s*[0-9]+u?\s*;", "Host frame header byte count must be explicit", "WIRE_CONSTRAINT"),
        evidence("include/hydra/host_protocol.hpp", r"kHostProtocolMaxPayloadBytes\s*=", "Host payload maximum must be explicit", "WIRE_CONSTRAINT"),
        evidence("include/hydra/host_protocol.hpp", r"kHostProtocolMaxStringBytes\s*=", "Host variable text must retain an explicit bound", "WIRE_CONSTRAINT"),
        evidence("include/hydra/workspace_manager.hpp", r"using\s+SeatId\s*=\s*std::uint32_t\s*;", "Host SeatId alias crossing the wire must stay uint32_t", "WIRE_ALIAS_WIDTH"),
    ),
    evidence=(
        evidence("tests/test_host_protocol.cpp", r"future protocol version is rejected explicitly", "Host focused test must reject future protocol versions", "WIRE_VERSION_EVIDENCE"),
        evidence("tests/test_host_protocol.cpp", r"nonzero reserved header fields are rejected", "Host focused test must reject nonzero reserved frame bytes", "WIRE_RESERVED_EVIDENCE"),
        evidence("tests/test_host_protocol.cpp", r"without pointer-size assumptions", "Host snapshot test must retain architecture-neutral serialization evidence", "WIRE_ARCH_EVIDENCE"),
    ),
)

PRODUCTION_CONTRACTS: tuple[CAbiContract | WireContract, ...] = (
    ADAPTER_ABI,
    SHIM_ABI,
    GATE_WIRE,
    HOST_WIRE,
)


def read_text(root: Path, relative: str) -> str:
    path = root / relative
    if not path.is_file():
        raise ValueError(f"required contract authority file is missing: {relative}")
    return path.read_text(encoding="utf-8")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\r\n]*", "", text)


def parse_int_literal(value: str) -> int:
    return int(re.sub(r"[uUlL]+$", "", value), 0)


def parse_int_macros(text: str) -> dict[str, int]:
    return {
        match.group("name"): parse_int_literal(match.group("value"))
        for match in DEFINE_INT_RE.finditer(text)
    }


def parse_c_fields(body: str) -> tuple[CField, ...]:
    cleaned = strip_comments(body)
    fields: list[CField] = []
    for raw in cleaned.split(";"):
        statement = " ".join(raw.split())
        if not statement:
            continue
        match = re.fullmatch(
            r"(?P<type>[A-Za-z_]\w*)\s*(?P<pointer>\*+)?\s*(?P<name>[A-Za-z_]\w*)"
            r"(?:\s*\[\s*(?P<count>[A-Za-z_]\w*|[0-9]+[uU]?)\s*\])?",
            statement,
        )
        if match is None:
            raise ValueError(f"unsupported public C struct field declaration: {statement}")
        type_name = match.group("type")
        if match.group("pointer"):
            type_name += match.group("pointer")
        fields.append(
            CField(
                type_name=type_name,
                name=match.group("name"),
                array_count_token=match.group("count") or "",
            )
        )
    return tuple(fields)


def parse_c_structs(text: str) -> dict[str, ParsedCStruct]:
    output: dict[str, ParsedCStruct] = {}
    for match in C_STRUCT_RE.finditer(text):
        tag = match.group("tag")
        alias = match.group("alias")
        if tag != alias:
            raise ValueError(f"public C struct tag/alias mismatch: {tag} != {alias}")
        if tag in output:
            raise ValueError(f"duplicate public C struct: {tag}")
        output[tag] = ParsedCStruct(
            name=tag,
            body=match.group("body"),
            fields=parse_c_fields(match.group("body")),
            start=match.start(),
            end=match.end(),
        )
    return output


def array_count(field_value: CField, macros: dict[str, int]) -> int:
    if not field_value.array_count_token:
        return 1
    count = field_value.array_count_token
    if re.fullmatch(r"[0-9]+[uU]?", count):
        return int(count.rstrip("uU"), 10)
    if count not in macros:
        raise ValueError(f"array count macro is not a simple reviewed integer: {count}")
    return macros[count]


def packed_struct_bytes(struct: ParsedCStruct, macros: dict[str, int]) -> int:
    total = 0
    for field_value in struct.fields:
        if field_value.type_name not in FIXED_C_SCALAR_BYTES:
            raise ValueError(
                f"non-fixed field type {field_value.type_name} in {struct.name}.{field_value.name}"
            )
        total += FIXED_C_SCALAR_BYTES[field_value.type_name] * array_count(field_value, macros)
    return total


def validate_requirement(
    root: Path,
    contract: str,
    requirement: EvidenceRequirement,
    result: AuditResult,
) -> None:
    try:
        text = read_text(root, requirement.path)
    except ValueError as exc:
        result.add(requirement.code, contract, requirement.path, str(exc))
        return
    if re.search(requirement.pattern, text, flags=re.DOTALL) is None:
        result.add(requirement.code, contract, requirement.path, requirement.description)


def native_type_hits(text: str) -> list[str]:
    return [name for name, pattern in NATIVE_TYPE_PATTERNS if pattern.search(text)]


def validate_c_abi(root: Path, spec: CAbiContract, result: AuditResult) -> None:
    try:
        header = read_text(root, spec.header)
        structs = parse_c_structs(header)
    except ValueError as exc:
        result.add("CABI_PARSE", spec.name, spec.header, str(exc))
        return

    macros = parse_int_macros(header)
    if spec.api_version_macro not in macros or macros[spec.api_version_macro] <= 0:
        result.add(
            "CABI_API_VERSION",
            spec.name,
            spec.header,
            f"missing positive integer API version macro {spec.api_version_macro}",
        )

    declared = {entry.name for entry in spec.structs}
    public_structs = {name for name in structs if name.startswith(spec.struct_prefix)}
    for name in sorted(public_structs - declared):
        result.add(
            "CABI_UNMANIFESTED_STRUCT",
            spec.name,
            spec.header,
            f"new public struct {name} requires explicit ABI manifest review",
        )
    for name in sorted(declared - public_structs):
        result.add(
            "CABI_MISSING_STRUCT",
            spec.name,
            spec.header,
            f"manifested public struct {name} is missing",
        )

    pack_push = header.find("#pragma pack(push, 1)")
    pack_pop = header.find("#pragma pack(pop)")
    if pack_push < 0 or pack_pop < 0 or pack_pop <= pack_push:
        result.add(
            "CABI_PACKING",
            spec.name,
            spec.header,
            "public fixed-layout structs must be enclosed by #pragma pack(push, 1)/pop",
        )

    layout_parts: list[str] = []
    for path in spec.layout_evidence_files:
        try:
            layout_parts.append(read_text(root, path))
        except ValueError as exc:
            result.add("CABI_LAYOUT_EVIDENCE", spec.name, path, str(exc))
    layout_evidence = "\n".join(layout_parts)

    reserved_source = ""
    if spec.reserved_evidence_file:
        try:
            reserved_source = read_text(root, spec.reserved_evidence_file)
        except ValueError as exc:
            result.add(
                "CABI_RESERVED_VALIDATION",
                spec.name,
                spec.reserved_evidence_file,
                str(exc),
            )

    for entry in spec.structs:
        parsed = structs.get(entry.name)
        if parsed is None:
            continue
        result.checked_c_structs += 1
        if pack_push >= 0 and pack_pop >= 0 and not (
            pack_push < parsed.start < parsed.end < pack_pop
        ):
            result.add(
                "CABI_PACKING",
                spec.name,
                spec.header,
                f"{entry.name} is outside the reviewed pack(1) region",
            )
        if (
            not parsed.fields
            or parsed.fields[0].name != "struct_size"
            or parsed.fields[0].type_name != "uint32_t"
        ):
            result.add(
                "CABI_STRUCT_SIZE_FIELD",
                spec.name,
                spec.header,
                f"{entry.name} must begin with uint32_t struct_size",
            )

        suffix = re.search(r"V([0-9]+)$", entry.name)
        if suffix is None or f"_V{suffix.group(1)}_BYTES" not in entry.size_macro:
            result.add(
                "CABI_VERSIONED_NAME",
                spec.name,
                spec.header,
                f"{entry.name} and {entry.size_macro} must carry the same explicit version suffix",
            )

        if entry.size_macro not in macros:
            result.add(
                "CABI_SIZE_MACRO",
                spec.name,
                spec.header,
                f"missing integer byte-size macro {entry.size_macro} for {entry.name}",
            )
        else:
            try:
                actual = packed_struct_bytes(parsed, macros)
            except ValueError as exc:
                result.add("CABI_NATIVE_FIELD", spec.name, spec.header, str(exc))
            else:
                if actual != macros[entry.size_macro]:
                    result.add(
                        "CABI_SIZE_MISMATCH",
                        spec.name,
                        spec.header,
                        f"{entry.name} packed fields total {actual} bytes but "
                        f"{entry.size_macro} declares {macros[entry.size_macro]}",
                    )

        api_field = next(
            (field_value for field_value in parsed.fields if field_value.name == "api_version"),
            None,
        )
        if api_field is not None and api_field.type_name != "uint32_t":
            result.add(
                "CABI_API_VERSION_FIELD",
                spec.name,
                spec.header,
                f"{entry.name}.api_version must be uint32_t when present",
            )

        for field_value in parsed.fields:
            if "*" in field_value.type_name or field_value.type_name not in FIXED_C_SCALAR_BYTES:
                result.add(
                    "CABI_NATIVE_FIELD",
                    spec.name,
                    spec.header,
                    f"{entry.name}.{field_value.name} uses architecture/native field type "
                    f"{field_value.type_name}",
                )
            if (
                field_value.name.startswith("reserved")
                and field_value.type_name not in UNSIGNED_FIXED_C_TYPES
            ):
                result.add(
                    "CABI_RESERVED_FIELD",
                    spec.name,
                    spec.header,
                    f"{entry.name}.{field_value.name} must use an unsigned fixed-width integer type",
                )

        layout_pattern = (
            rf"sizeof\s*\(\s*{re.escape(entry.name)}\s*\)\s*==\s*"
            rf"{re.escape(entry.size_macro)}"
        )
        if re.search(layout_pattern, layout_evidence, flags=re.DOTALL) is None:
            result.add(
                "CABI_LAYOUT_EVIDENCE",
                spec.name,
                ",".join(spec.layout_evidence_files),
                f"missing compile-time sizeof assertion for {entry.name}",
            )

        for reserved_pattern in entry.reserved_evidence_patterns:
            if not reserved_source or re.search(
                reserved_pattern, reserved_source, flags=re.DOTALL
            ) is None:
                result.add(
                    "CABI_RESERVED_VALIDATION",
                    spec.name,
                    spec.reserved_evidence_file or spec.header,
                    f"missing fail-closed reserved-field handling evidence for {entry.name}",
                )

    marker_re = re.compile(
        EXPORTED_DECL_TEMPLATE.format(marker=re.escape(spec.export_marker))
    )
    allowed = set(spec.native_param_allowlist)
    observed_allowed: set[str] = set()
    for match in marker_re.finditer(header):
        declaration = match.group("decl")
        fn_match = re.search(r"([A-Za-z_]\w*)\s*\(", declaration)
        if fn_match is None:
            continue
        function_name = fn_match.group(1)
        for native_name in native_type_hits(declaration):
            key = f"{function_name}:{native_name}"
            if key in allowed:
                observed_allowed.add(key)
                result.intentional_native_exceptions.append(f"{spec.name}:{key}")
            else:
                result.add(
                    "CABI_NATIVE_PARAM",
                    spec.name,
                    spec.header,
                    f"exported function {function_name} exposes native-width scalar type "
                    f"{native_name}; use an exact fixed-width type or add a reviewed exception",
                )
    for stale in sorted(allowed - observed_allowed):
        result.add(
            "CABI_STALE_ALLOWLIST",
            spec.name,
            spec.header,
            f"native-width exception {stale} is no longer present; remove the stale exception",
        )

    for requirement in spec.extra_evidence:
        validate_requirement(root, spec.name, requirement, result)
    result.checked_contracts.append(spec.name)


def cpp_struct_map(text: str) -> dict[str, str]:
    return {
        match.group("name"): match.group("body")
        for match in CPP_STRUCT_RE.finditer(text)
    }


def cpp_fields(body: str) -> list[tuple[str, str]]:
    cleaned = strip_comments(body)
    fields: list[tuple[str, str]] = []
    for raw in cleaned.split(";"):
        statement = " ".join(raw.split())
        if (
            not statement
            or "(" in statement
            or statement.startswith(("using ", "friend ", "static "))
        ):
            continue
        statement = statement.split("{", 1)[0].strip()
        if not statement:
            continue
        match = re.fullmatch(
            r"(?P<type>.+?)\s+(?P<name>[A-Za-z_]\w*)(?:\[[^\]]+\])?",
            statement,
        )
        if match is None:
            continue
        fields.append((match.group("type").strip(), match.group("name")))
    return fields


def validate_wire_struct_source(
    root: Path,
    contract: str,
    source: WireStructSource,
    result: AuditResult,
) -> None:
    try:
        text = read_text(root, source.path)
    except ValueError as exc:
        result.add("WIRE_SOURCE", contract, source.path, str(exc))
        return
    structs = cpp_struct_map(text)
    selected = sorted(structs) if source.scan_all else list(source.names)
    for name in selected:
        body = structs.get(name)
        if body is None:
            result.add(
                "WIRE_MISSING_STRUCT",
                contract,
                source.path,
                f"wire/support struct {name} is missing",
            )
            continue
        result.checked_wire_structs += 1
        for type_name, field_name in cpp_fields(body):
            hits = native_type_hits(type_name)
            if "*" in type_name:
                hits.append("raw pointer")
            if hits:
                result.add(
                    "WIRE_NATIVE_FIELD",
                    contract,
                    source.path,
                    f"{name}.{field_name} uses architecture/native field type {type_name} "
                    f"({', '.join(sorted(set(hits)))})",
                )


def validate_wire_enum_source(
    root: Path,
    contract: str,
    source: WireEnumSource,
    result: AuditResult,
) -> None:
    try:
        text = read_text(root, source.path)
    except ValueError as exc:
        result.add("WIRE_ENUM_SOURCE", contract, source.path, str(exc))
        return
    enums = {
        match.group("name"): (match.group("underlying") or "").strip()
        for match in ENUM_CLASS_RE.finditer(text)
    }
    selected = sorted(enums) if source.scan_all else list(source.names)
    for name in selected:
        if name not in enums:
            result.add(
                "WIRE_MISSING_ENUM",
                contract,
                source.path,
                f"wire/support enum {name} is missing",
            )
            continue
        underlying = enums[name]
        if re.fullmatch(r"std::(?:u?int)(?:8|16|32|64)_t", underlying) is None:
            result.add(
                "WIRE_ENUM_WIDTH",
                contract,
                source.path,
                f"enum class {name} must declare an explicit fixed-width integer "
                f"underlying type, found {underlying or 'implicit'}",
            )


def validate_wire(root: Path, spec: WireContract, result: AuditResult) -> None:
    for source in spec.struct_sources:
        validate_wire_struct_source(root, spec.name, source, result)
    for source in spec.enum_sources:
        validate_wire_enum_source(root, spec.name, source, result)
    for requirement in spec.requirements:
        validate_requirement(root, spec.name, requirement, result)
    for requirement in spec.evidence:
        validate_requirement(root, spec.name, requirement, result)
    result.checked_contracts.append(spec.name)


def validate_repository(
    root: Path,
    contracts: Sequence[CAbiContract | WireContract] = PRODUCTION_CONTRACTS,
) -> AuditResult:
    result = AuditResult()
    root = root.resolve()
    for contract in contracts:
        if isinstance(contract, CAbiContract):
            validate_c_abi(root, contract, result)
        else:
            validate_wire(root, contract, result)
    result.errors.sort(
        key=lambda item: (item.contract, item.code, item.path, item.message)
    )
    result.checked_contracts.sort()
    result.intentional_native_exceptions.sort()
    return result


def print_result(result: AuditResult, root: Path) -> None:
    if result.errors:
        print(
            f"Public ABI contract audit FAILED: {len(result.errors)} error(s); root={root}",
            file=sys.stderr,
        )
        for error in result.errors:
            print(
                f"ERROR [{error.code}] {error.contract} {error.path}: {error.message}",
                file=sys.stderr,
            )
        if result.intentional_native_exceptions:
            print(
                "Intentional native-width exceptions: "
                + ", ".join(result.intentional_native_exceptions),
                file=sys.stderr,
            )
        return
    print(
        "Public ABI contract audit valid: "
        f"{len(result.checked_contracts)} contract(s), "
        f"{result.checked_c_structs} packed C struct(s), "
        f"{result.checked_wire_structs} wire/support struct(s); root={root}"
    )
    if result.intentional_native_exceptions:
        print(
            "Intentional reviewed native-width exceptions: "
            + ", ".join(result.intentional_native_exceptions)
        )


def requirement_from_fixture(data: dict[str, object]) -> EvidenceRequirement:
    values = {key: data.get(key) for key in ("path", "pattern", "description", "code")}
    if not all(isinstance(value, str) and value for value in values.values()):
        raise ValueError(
            "fixture evidence requirement needs nonempty path/pattern/description/code"
        )
    return EvidenceRequirement(
        path=str(values["path"]),
        pattern=str(values["pattern"]),
        description=str(values["description"]),
        code=str(values["code"]),
    )


def abi_struct_from_fixture(data: dict[str, object]) -> AbiStructSpec:
    name = data.get("name")
    size_macro = data.get("size_macro")
    patterns = data.get("reserved_evidence_patterns", [])
    if (
        not isinstance(name, str)
        or not name
        or not isinstance(size_macro, str)
        or not size_macro
    ):
        raise ValueError("fixture C ABI struct needs name and size_macro")
    if not isinstance(patterns, list) or not all(
        isinstance(item, str) for item in patterns
    ):
        raise ValueError("fixture reserved_evidence_patterns must be a string list")
    return AbiStructSpec(
        name=name,
        size_macro=size_macro,
        reserved_evidence_patterns=tuple(patterns),
    )


def contract_from_fixture(data: dict[str, object]) -> CAbiContract | WireContract:
    kind = data.get("kind")
    name = data.get("name")
    if not isinstance(name, str) or not name:
        raise ValueError("fixture contract name is required")

    if kind == "c_abi":
        def string_value(key: str, default: str = "") -> str:
            value = data.get(key, default)
            if not isinstance(value, str):
                raise ValueError(f"fixture {key} must be a string")
            return value

        def string_list(key: str) -> tuple[str, ...]:
            value = data.get(key, [])
            if not isinstance(value, list) or not all(
                isinstance(item, str) for item in value
            ):
                raise ValueError(f"fixture {key} must be a string list")
            return tuple(value)

        structs_data = data.get("structs", [])
        extra_data = data.get("extra_evidence", [])
        if not isinstance(structs_data, list) or not all(
            isinstance(item, dict) for item in structs_data
        ):
            raise ValueError("fixture C ABI structs must be objects")
        if not isinstance(extra_data, list) or not all(
            isinstance(item, dict) for item in extra_data
        ):
            raise ValueError("fixture C ABI extra_evidence must be objects")
        return CAbiContract(
            name=name,
            header=string_value("header"),
            struct_prefix=string_value("struct_prefix"),
            api_version_macro=string_value("api_version_macro"),
            export_marker=string_value("export_marker"),
            structs=tuple(abi_struct_from_fixture(item) for item in structs_data),
            layout_evidence_files=string_list("layout_evidence_files"),
            reserved_evidence_file=string_value("reserved_evidence_file"),
            native_param_allowlist=string_list("native_param_allowlist"),
            extra_evidence=tuple(
                requirement_from_fixture(item) for item in extra_data
            ),
        )

    if kind == "wire":
        struct_data = data.get("struct_sources", [])
        enum_data = data.get("enum_sources", [])
        requirements = data.get("requirements", [])
        evidence_data = data.get("evidence", [])
        if not all(
            isinstance(value, list)
            for value in (struct_data, enum_data, requirements, evidence_data)
        ):
            raise ValueError("fixture wire source/requirement fields must be lists")

        def source_names(item: dict[str, object]) -> tuple[str, ...]:
            names = item.get("names", [])
            if not isinstance(names, list) or not all(
                isinstance(value, str) for value in names
            ):
                raise ValueError("fixture wire source names must be strings")
            return tuple(names)

        struct_sources: list[WireStructSource] = []
        for item in struct_data:
            if (
                not isinstance(item, dict)
                or not isinstance(item.get("path"), str)
                or not isinstance(item.get("scan_all", False), bool)
            ):
                raise ValueError("invalid fixture wire struct source")
            struct_sources.append(
                WireStructSource(
                    path=str(item["path"]),
                    names=source_names(item),
                    scan_all=bool(item.get("scan_all", False)),
                )
            )

        enum_sources: list[WireEnumSource] = []
        for item in enum_data:
            if (
                not isinstance(item, dict)
                or not isinstance(item.get("path"), str)
                or not isinstance(item.get("scan_all", False), bool)
            ):
                raise ValueError("invalid fixture wire enum source")
            enum_sources.append(
                WireEnumSource(
                    path=str(item["path"]),
                    names=source_names(item),
                    scan_all=bool(item.get("scan_all", False)),
                )
            )

        if not all(
            isinstance(item, dict) for item in list(requirements) + list(evidence_data)
        ):
            raise ValueError("fixture wire evidence entries must be objects")
        return WireContract(
            name=name,
            struct_sources=tuple(struct_sources),
            enum_sources=tuple(enum_sources),
            requirements=tuple(
                requirement_from_fixture(item) for item in requirements
            ),
            evidence=tuple(
                requirement_from_fixture(item) for item in evidence_data
            ),
        )

    raise ValueError(f"unsupported fixture contract kind: {kind}")


def write_fixture_tree(root: Path, files: dict[str, str]) -> None:
    for relative, content in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")


def run_self_tests(fixture_path: Path = FIXTURE_PATH) -> None:
    data = json.loads(fixture_path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise ValueError("public ABI fixture schema_version must be 1")
    cases = data.get("cases")
    if not isinstance(cases, list) or len(cases) < 12:
        raise ValueError("public ABI self-test requires at least 12 reviewed cases")

    failures: list[str] = []
    seen: set[str] = set()
    temp_parent = Path(tempfile.mkdtemp(prefix="hydraseat-public-abi-"))
    try:
        for index, case in enumerate(cases):
            if not isinstance(case, dict):
                failures.append(f"case {index}: case must be an object")
                continue
            name = case.get("name")
            expect = case.get("expect")
            files = case.get("files")
            contract_data = case.get("contract")
            expected_codes = case.get("expected_error_codes", [])
            if (
                not isinstance(name, str)
                or not name
                or name in seen
                or expect not in {"pass", "fail"}
                or not isinstance(files, dict)
                or not all(
                    isinstance(key, str) and isinstance(value, str)
                    for key, value in files.items()
                )
                or not isinstance(contract_data, dict)
                or not isinstance(expected_codes, list)
                or not all(isinstance(code, str) for code in expected_codes)
            ):
                failures.append(f"case {index}: invalid fixture metadata")
                continue
            seen.add(name)
            case_root = temp_parent / f"case-{index:02d}-{name}"
            case_root.mkdir()
            write_fixture_tree(case_root, files)
            try:
                contract = contract_from_fixture(contract_data)
                result = validate_repository(case_root, (contract,))
            except (OSError, UnicodeError, ValueError) as exc:
                failures.append(f"{name}: validator raised unexpectedly: {exc}")
                continue
            actual_codes = {error.code for error in result.errors}
            if expect == "pass" and result.errors:
                failures.append(
                    f"{name}: expected pass but got "
                    + ", ".join(
                        f"{error.code}:{error.message}" for error in result.errors
                    )
                )
            elif expect == "fail" and not result.errors:
                failures.append(f"{name}: expected failure but validator passed")
            missing = sorted(set(expected_codes) - actual_codes)
            if missing:
                failures.append(
                    f"{name}: expected error code(s) not observed: {', '.join(missing)}; "
                    f"actual={', '.join(sorted(actual_codes)) or 'none'}"
                )
    finally:
        shutil.rmtree(temp_parent, ignore_errors=True)

    if failures:
        raise ValueError(
            "public ABI self-test failed:\n  " + "\n  ".join(failures)
        )
    print(f"Public ABI contract self-test passed: {len(cases)}/{len(cases)} cases.")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=ROOT,
        help="repository root (default: this checkout)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run deterministic synthetic ABI/protocol cases",
    )
    parser.add_argument("--fixture", type=Path, default=FIXTURE_PATH, help=argparse.SUPPRESS)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.self_test:
            run_self_tests(args.fixture)
            return 0
        result = validate_repository(args.root)
        print_result(result, args.root.resolve())
        return 1 if result.errors else 0
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        print(f"Public ABI contract validator failed closed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
