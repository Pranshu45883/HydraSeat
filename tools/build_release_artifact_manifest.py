#!/usr/bin/env python3
"""Build or inspect deterministic HydraSeat release artifact preflight metadata."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from validate_release_artifact_manifest import (
    ArtifactManifestError,
    CHECKSUMS_NAME,
    MANIFEST_NAME,
    PROVENANCE_NAME,
    ROOT,
    SBOM_NAME,
    generate_release_bundle,
    inspect_release_inputs,
)


def _add_input_group(parser: argparse.ArgumentParser) -> None:
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--build-root",
        type=Path,
        help="Exact CMake build root configured from this checkout; only reviewed release targets are read.",
    )
    group.add_argument(
        "--package-root",
        type=Path,
        help="Exact staged package root; every regular file must be allowlisted.",
    )


def _input_mode(args: argparse.Namespace) -> str:
    return "BuildRoot" if args.build_root is not None else "PackageRoot"


def _canonical_stdout(value: object) -> None:
    print(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "HydraSeat exact release artifact preflight. It inventories only the reviewed x64 release allowlist; "
            "it never recursively packages a build tree and never signs artifacts."
        )
    )
    sub = parser.add_subparsers(dest="command", required=True)

    generate = sub.add_parser(
        "generate",
        help="Generate canonical manifest/checksums/SBOM/provenance from a complete exact input set.",
    )
    _add_input_group(generate)
    generate.add_argument("--output-dir", required=True, type=Path)
    generate.add_argument("--release-version", required=True)
    generate.add_argument("--release-revision", required=True, type=int)
    generate.add_argument("--commit-sha", required=True)
    generate.add_argument(
        "--qualification-mode",
        choices=("Controlled", "ReleaseCandidate"),
        default="Controlled",
        help="Controlled is explicitly non-release-qualified. ReleaseCandidate still requires all legal/signing blockers to clear.",
    )
    generate.add_argument(
        "--verify-production-signatures",
        action="store_true",
        help=(
            "Reserved deployment gate. Controlled preflight rejects this flag rather than executing a "
            "package-contained verifier or manufacturing production signing authority."
        ),
    )

    inspect = sub.add_parser(
        "inspect",
        help="Read-only Controlled preflight report; incomplete/missing developer outputs are reported, not manufactured.",
    )
    _add_input_group(inspect)
    inspect.add_argument("--commit-sha")
    inspect.add_argument("--release-version")
    inspect.add_argument("--release-revision", type=int)

    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        mode = _input_mode(args)
        if args.command == "inspect":
            report = inspect_release_inputs(
                repo_root=ROOT,
                input_mode=mode,
                build_root=args.build_root,
                package_root=args.package_root,
                configuration="Release",
                expected_commit=args.commit_sha,
                release_version=args.release_version,
                release_revision=args.release_revision,
            )
            _canonical_stdout(report)
            return 0

        manifest = generate_release_bundle(
            repo_root=ROOT,
            output_dir=args.output_dir,
            input_mode=mode,
            build_root=args.build_root,
            package_root=args.package_root,
            configuration="Release",
            release_version=args.release_version,
            release_revision=args.release_revision,
            commit_sha=args.commit_sha,
            qualification_mode=args.qualification_mode,
            verify_production_signatures=args.verify_production_signatures,
        )
        print(
            "Release artifact preflight generated and revalidated: "
            f"{manifest['release']['version']} rev {manifest['release']['revision']} "
            f"{manifest['release']['architecture']} {manifest['integrity']['artifactPreflightQualificationState']}"
        )
        print(f"  {args.output_dir / MANIFEST_NAME}")
        print(f"  {args.output_dir / CHECKSUMS_NAME}")
        print(f"  {args.output_dir / SBOM_NAME}")
        print(f"  {args.output_dir / PROVENANCE_NAME}")
        return 0
    except (ArtifactManifestError, OSError) as exc:
        print(f"release artifact preflight failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
