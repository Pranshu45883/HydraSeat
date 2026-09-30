#!/usr/bin/env python3
"""Validate the P8 signing/package/installer contract as one fail-closed unit."""

from __future__ import annotations

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "config" / "release-signing-manifest.json"
SIGNER = ROOT / "tools" / "sign_release_artifacts.ps1"
INSTALLER = ROOT / "tools" / "install_hydraseat.ps1"
CONTROLLED_HARNESS = ROOT / "tools" / "run_installer_recovery_harness.py"

OWNED_FILES_START = "$OwnedFiles = @("
OWNED_FILES_END = "$OwnedArtifactIds"
QUOTED_RE = re.compile(r"^\s*\"(?P<value>[A-Za-z0-9._-]+)\"\s*,?\s*$", re.MULTILINE)


def fail(message: str) -> None:
    raise ValueError(message)


def require_once(text: str, needle: str, context: str) -> None:
    count = text.count(needle)
    if count != 1:
        fail(f"{context} must contain exactly one reviewed occurrence of {needle!r}; found {count}")


def extract_owned_files(installer_text: str) -> list[str]:
    start = installer_text.find(OWNED_FILES_START)
    if start < 0:
        fail("installer OwnedFiles allowlist could not be located")
    start += len(OWNED_FILES_START)
    end = installer_text.find(OWNED_FILES_END, start)
    if end < 0:
        fail("installer OwnedFiles allowlist end marker could not be located")
    body = installer_text[start:end].strip()
    if not body.endswith(")"):
        fail("installer OwnedFiles allowlist closing token is missing")
    body = body[:-1]
    files = [item.group("value") for item in QUOTED_RE.finditer(body)]
    residual = QUOTED_RE.sub("", body).strip().replace(",", "").strip()
    if residual:
        fail("installer OwnedFiles contains non-literal or unparsed content")
    if not files:
        fail("installer OwnedFiles cannot be empty")
    if len(files) != len(set(files)):
        fail("installer OwnedFiles contains duplicates")
    return files


def validate_contract(manifest_data: object, signer_text: str, installer_text: str) -> None:
    if not isinstance(manifest_data, dict) or manifest_data.get("schemaVersion") != 1:
        fail("release signing manifest schemaVersion must be 1")
    artifacts = manifest_data.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        fail("release signing manifest artifacts must be a non-empty list")

    manifest_files: list[str] = []
    manifest_ids: dict[str, str] = {}
    script_entries = []
    executable_entries = []
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            fail("release signing artifact must be an object")
        file_name = artifact.get("fileName")
        artifact_id = artifact.get("id")
        if not isinstance(file_name, str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,128}", file_name):
            fail("release signing artifact fileName must be one safe basename")
        if not isinstance(artifact_id, str) or not re.fullmatch(r"[A-Za-z0-9._-]{1,128}", artifact_id):
            fail("release signing artifact id must be one bounded product identity")
        if file_name in manifest_files:
            fail(f"duplicate release signing fileName: {file_name}")
        manifest_files.append(file_name)
        manifest_ids[file_name] = artifact_id
        architectures = artifact.get("architectures")
        if not isinstance(architectures, list) or architectures != ["x64"]:
            fail(f"{file_name}: release host artifacts must target x64 exactly once")
        kind = artifact.get("kind")
        if kind == "cmake-executable":
            executable_entries.append(artifact)
        elif kind == "powershell-script":
            script_entries.append(artifact)
        else:
            fail(f"{file_name}: unsupported signing artifact kind {kind!r}")

    if len(script_entries) != 1:
        fail("release package must contain exactly one reviewed PowerShell installer script")
    installer_artifact = script_entries[0]
    if installer_artifact.get("id") != "installer-script":
        fail("PowerShell signing artifact must use id installer-script")
    if installer_artifact.get("sourcePath") != "tools/install_hydraseat.ps1":
        fail("PowerShell signing artifact must source tools/install_hydraseat.ps1")
    if installer_artifact.get("fileName") != "install_hydraseat.ps1":
        fail("PowerShell signing artifact must publish install_hydraseat.ps1")
    if not executable_entries:
        fail("release package must contain reviewed executable artifacts")

    owned_files = extract_owned_files(installer_text)
    if owned_files != manifest_files:
        fail(
            "installer OwnedFiles must exactly match signing-manifest artifact order; "
            f"manifest={manifest_files}, installer={owned_files}"
        )
    for file_name in manifest_files:
        mapping = f'    "{file_name}" = "{manifest_ids[file_name]}"'
        require_once(installer_text, mapping, "installer exact artifact identity map")

    # Signer invariants: executable and script artifacts take distinct reviewed paths,
    # then converge on the same Authenticode publisher/hash provenance checks.
    for needle in (
        '$kind -eq "cmake-executable"',
        '$kind -eq "powershell-script"',
        'Set-AuthenticodeSignature -LiteralPath $destination',
        '[string]$artifact.sourcePath -ne "tools/install_hydraseat.ps1"',
        '$fileName -ne "install_hydraseat.ps1"',
        '$architectures.Count -ne 1',
        '$architectures[0] -ne "x64"',
        '$architecture -ne "x64"',
        'Get-AuthenticodeSignature -LiteralPath $destination',
        'signedSha256 = $signedHash',
        'signerThumbprint = $normalizedThumbprint',
        'signatureStatus = [string]$signature.Status',
        'Assert-ReviewedSigningManifest -Manifest $manifest',
        'CommitSha must be the exact 40-hex reviewed Git commit',
        'status --porcelain',
        'Release signing requires CMake configuration Release',
        'Release build root was not configured from the reviewed repository checkout',
        'Invoke-ReviewedTargetBuild -CMakePath $cmake -BuildRoot $BuildX64',
        '"--clean-first"',
        'Failed to clean and rebuild the reviewed release target set',
        'Release rebuild changed the reviewed source checkout; refusing to sign',
        'Assert-X64PortableExecutable -Path $source',
        'Reviewed CMake release output must be unsigned before release signing',
        'Write-DetachedCmsSignature -ContentPath $provenancePath',
        'Assert-DetachedCmsSignature -ContentPath $provenancePath',
    ):
        if needle not in signer_text:
            fail(f"release signer is missing required contract fragment: {needle}")
    require_once(
        signer_text,
        'Signing manifest must be the reviewed repository config/release-signing-manifest.json',
        "release signer",
    )

    # Installer invariants: package and state ownership remain exact, files are
    # individually hash/signature verified, and per-user deletion is separated from
    # the ProgramData rollback transaction.
    for needle in (
        '$records.Count -ne $OwnedFiles.Count',
        '$seen.ContainsKey($fileName)',
        '$seenOwnedFiles.ContainsKey($fileName)',
        'Release file hash mismatch: $fileName',
        'Release file publisher/signature mismatch: $fileName',
        'signing-provenance.json.p7s',
        'Assert-DetachedCmsSignature -ContentPath $provenancePath',
        '$contentFile.Length -le 0 -or $contentFile.Length -gt $MaximumSigningProvenanceBytes',
        '$signatureFile.Length -gt $MaximumDetachedProvenanceSignatureBytes',
        'Release signing provenance contains unknown or missing fields',
        'Release artifact provenance contains unknown or missing fields',
        '$records = @($provenance.artifacts)',
        '$record.id -ne [string]$OwnedArtifactIds[$fileName]',
        '$record.kind -ne $expectedKind',
        '$record.architecture -ne $Architecture',
        'Release artifact identity/type/architecture does not match the fixed HydraSeat product contract',
        'Path escapes the owned root',
        'commitSha -notmatch',
        'if (-not [Environment]::Is64BitOperatingSystem) {',
        '$Architecture = "x64"',
        '$ProcessesThatMustBeStopped = @($OwnedFiles | Where-Object {',
        '[IO.Path]::GetFileNameWithoutExtension($_)',
        'Close all HydraSeat release processes and return to ordinary Windows before install changes',
        '$UserDataRoot = Join-Path $env:LOCALAPPDATA "HydraSeat"',
        'Assert-OwnedDirectoryNotReparsePoint -Path $UserDataRoot',
        'Remove-Item -LiteralPath $UserDataRoot -Recurse -Force',
        'Remove-EmptyMachineDataRoots',
        'Assert-OwnedDirectoryNotReparsePoint -Path $TransactionRoot',
        'function Assert-OwnedLeafNotReparsePoint {',
        'must be a normal file and not a reparse point',
        'Assert-OwnedLeafNotReparsePoint -Path $provenancePath -Label "HydraSeat signing provenance"',
        'Assert-OwnedLeafNotReparsePoint -Path $provenanceSignaturePath -Label "HydraSeat signing provenance signature"',
        'Assert-OwnedLeafNotReparsePoint -Path $filePath -Label "HydraSeat release file $fileName"',
        'Assert-OwnedLeafNotReparsePoint -Path $Path -Label "HydraSeat install state"',
        'Assert-OwnedLeafNotReparsePoint -Path $path -Label "HydraSeat installer transaction state marker"',
        'Assert-OwnedLeafNotReparsePoint -Path $destination -Label "HydraSeat install destination $($file.fileName)"',
        'if ($Mode -eq "Validate") {',
        'Assert-NoPendingInstallerTransactions',
        '$MutationLock = Enter-InstallerMutationLock',
        'Recover-InterruptedTransactions',
        '$TransactionStateFileName = "transaction-state.json"',
        'previousStatePresent = $PreviousStatePresent',
        'snapshotIdentity = $SnapshotIdentity',
        'Get-TransactionSnapshotIdentity -Backup $Snapshot.backup',
        'backup changed after its prepared snapshot was journaled',
        '-SnapshotIdentity $snapshot.snapshotIdentity',
        'Read-InstallStateFile -Path $backupStatePath',
        'Verify-RestoredTransactionSnapshot -Snapshot $snapshot',
        'Verify-UninstalledOwnedState',
        'function Verify-StagedPackage {',
        'Staged release file hash verification failed: $fileName',
        'Staged release file Authenticode verification failed: $fileName',
        'Installer staging root does not contain the exact verified owned file set',
        'Verify-StagedPackage -Package $package -StageRoot $snapshot.stage -ExpectedSigner $OwnSigner',
        'Repair requires the exact installed release identity; use the approved update/rollback flow for a different release',
        'Installed state identity does not match the verified release package',
        'Uninstall registration does not match the verified installed release',
        'Assert-Administrator',
    ):
        if needle not in installer_text:
            fail(f"installer is missing required contract fragment: {needle}")

    if installer_text.count('Assert-NoPendingInstallerTransactions') != 2:
        fail("installer must define and invoke the read-only interrupted-transaction guard exactly once")
    if installer_text.count('Enter-InstallerMutationLock') != 2:
        fail("installer must define and acquire exactly one mutation lock path")
    if installer_text.count('Recover-InterruptedTransactions') != 2:
        fail("installer must define and invoke interrupted-transaction recovery exactly once")
    if installer_text.count('-Phase "snapshotting"') != 1 or installer_text.count('-Phase "prepared"') != 1:
        fail("installer snapshot creation must durably distinguish snapshotting from prepared state")
    if installer_text.count('-Phase "committed"') != 2:
        fail("install/repair and uninstall must both write a committed transaction marker")
    if installer_text.count('$cleanupSnapshot = $false') != 2 or installer_text.count(
        'if ($cleanupSnapshot -and (Test-Path -LiteralPath $snapshot.root)) {'
    ) != 2:
        fail("transaction snapshots must be retained unless commit or verified rollback permits cleanup")
    if installer_text.count('$stream.Flush($true)') != 2:
        fail("install state and transaction-state markers must both flush before replacement")
    if installer_text.count('Verify-StagedPackage') != 2:
        fail("staged package verification must be defined once and invoked once before machine mutation")
    staged_verify_call = installer_text.find(
        'Verify-StagedPackage -Package $package -StageRoot $snapshot.stage -ExpectedSigner $OwnSigner'
    )
    install_copy = installer_text.find(
        'Copy-Item -LiteralPath $source -Destination $destination -Force',
        staged_verify_call,
    )
    if staged_verify_call < 0 or install_copy < 0 or staged_verify_call >= install_copy:
        fail("signed/hash-verified staging must complete before owned files are copied into Program Files")

    if '#Requires -RunAsAdministrator' in installer_text:
        fail("read-only Validate mode must not be forced to run elevated")
    if 'Remove-Item -LiteralPath $DataRoot -Recurse -Force' in installer_text:
        fail("installer must never recursively delete ProgramData while rollback state lives below it")

    user_delete = installer_text.find('Remove-Item -LiteralPath $UserDataRoot -Recurse -Force')
    snapshot_finally = installer_text.find('Remove-Item -LiteralPath $snapshot.root -Recurse -Force')
    if user_delete < 0 or snapshot_finally < 0 or user_delete <= snapshot_finally:
        fail("per-user deletion must occur only after the machine rollback snapshot has been released")


def validate_controlled_harness(installer_text: str, harness_text: str) -> None:
    # The shipping installer keeps its fixed real-system roots and has no hidden
    # harness/root override. Controlled fault evidence lives in a separate tool.
    for needle in (
        '$InstallRoot = Join-Path $env:ProgramFiles "HydraSeat"',
        '$DataRoot = Join-Path $env:ProgramData "HydraSeat"',
        '$UninstallKey = "HKLM:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HydraSeat"',
        'Assert-Administrator',
    ):
        if needle not in installer_text:
            fail(f"shipping installer boundary is missing required production root/authority: {needle}")
    for forbidden in ("ControlledTestRoot", "AcceptanceTestRoot", "TestInstallRoot", "-TestRoot"):
        if forbidden in installer_text:
            fail("shipping installer must not expose a controlled-harness root bypass")

    for needle in (
        'CONTROLLED_EVIDENCE_CLASS = "Controlled"',
        'CONTROLLED_ROOT_NAME = "HydraSeatInstallerAcceptance"',
        'pathlib.Path(tempfile.gettempdir()).resolve() / CONTROLLED_ROOT_NAME',
        'def assert_safe_root(',
        'candidate.parent != base',
        'controlled root overlaps an installed-product path',
        'controlled process-interruption evidence',
        'os._exit(CONTROLLED_INTERRUPT_EXIT)',
        'sys.executable',
        'subprocess.run(',
        'subprocess.Popen(',
        'CONTROLLED_LOCK_ACQUIRED',
        'expected-previous CAS rejected a stale controlled writer',
        'assert_sentinels(root)',
        'os.symlink(target, tx_root / "stage", target_is_directory=True)',
        'another controlled installer mutation owns the authoritative lock',
        'controlled transaction set exceeds the bounded maximum',
    ):
        if needle not in harness_text:
            fail(f"controlled installer recovery harness is missing required safety/evidence fragment: {needle}")

    for fault in (
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
    ):
        if f'"{fault}"' not in harness_text:
            fail(f"controlled installer recovery harness is missing fault point {fault}")

    for corruption in (
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
    ):
        if f'"{corruption}"' not in harness_text:
            fail(f"controlled installer recovery harness is missing corruption case {corruption}")

    for forbidden in (
        'evidence_class": "Physical"',
        'evidence_class": "CleanMachine"',
        'evidence_class": "Manual"',
        'evidence_class": "Production"',
        "powershell.exe",
        "pwsh",
        "winreg",
        "sc.exe",
        "reg.exe",
        "Start-Process",
        "Get-AuthenticodeSignature",
        "Set-AuthenticodeSignature",
        "Global\\HydraSeatInstallerMutationV1",
    ):
        if forbidden in harness_text:
            fail(f"controlled installer recovery harness crosses a forbidden production boundary: {forbidden}")


def validate_live() -> None:
    manifest_data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    signer_text = SIGNER.read_text(encoding="utf-8")
    installer_text = INSTALLER.read_text(encoding="utf-8")
    harness_text = CONTROLLED_HARNESS.read_text(encoding="utf-8")
    validate_contract(manifest_data, signer_text, installer_text)
    validate_controlled_harness(installer_text, harness_text)


def expect_rejected(manifest_data: dict, signer_text: str, installer_text: str,
                    mutate, label: str) -> None:
    copied = json.loads(json.dumps(manifest_data))
    signer = signer_text
    installer = installer_text
    copied, signer, installer = mutate(copied, signer, installer)
    try:
        validate_contract(copied, signer, installer)
    except ValueError:
        return
    fail(f"self-test failed to reject {label}")


def expect_controlled_harness_rejected(
    installer_text: str, harness_text: str, mutate, label: str
) -> None:
    installer, harness = mutate(installer_text, harness_text)
    try:
        validate_controlled_harness(installer, harness)
    except ValueError:
        return
    fail(f"self-test failed to reject controlled harness weakening: {label}")


def self_test() -> None:
    manifest_data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    signer_text = SIGNER.read_text(encoding="utf-8")
    installer_text = INSTALLER.read_text(encoding="utf-8")
    harness_text = CONTROLLED_HARNESS.read_text(encoding="utf-8")
    validate_contract(manifest_data, signer_text, installer_text)
    validate_controlled_harness(installer_text, harness_text)

    def drop_installer_artifact(data, signer, installer):
        data["artifacts"] = [a for a in data["artifacts"] if a.get("id") != "installer-script"]
        return data, signer, installer

    def drift_owned_files(data, signer, installer):
        installer = installer.replace('    "install_hydraseat.ps1"\n)', '    "other.ps1"\n)', 1)
        return data, signer, installer

    def remove_script_signing(data, signer, installer):
        signer = signer.replace("Set-AuthenticodeSignature -LiteralPath $destination", "Missing-Signer", 1)
        return data, signer, installer

    def broad_programdata_delete(data, signer, installer):
        installer += "\nRemove-Item -LiteralPath $DataRoot -Recurse -Force\n"
        return data, signer, installer

    def duplicate_state_weakening(data, signer, installer):
        installer = installer.replace("$seenOwnedFiles.ContainsKey($fileName)", "$false")
        return data, signer, installer

    def force_validate_elevation(data, signer, installer):
        installer = "#Requires -RunAsAdministrator\n" + installer
        return data, signer, installer

    def remove_provenance_signature_check(data, signer, installer):
        installer = installer.replace(
            "Assert-DetachedCmsSignature -ContentPath $provenancePath",
            "Missing-Provenance-Signature-Check",
            1,
        )
        return data, signer, installer

    def remove_provenance_size_bound(data, signer, installer):
        installer = installer.replace(
            "$contentFile.Length -le 0 -or $contentFile.Length -gt $MaximumSigningProvenanceBytes",
            "$false",
            1,
        )
        return data, signer, installer

    def weaken_exact_artifact_identity(data, signer, installer):
        installer = installer.replace(
            "$record.id -ne [string]$OwnedArtifactIds[$fileName]",
            "$false",
            1,
        )
        return data, signer, installer

    def weaken_path_escape_guard(data, signer, installer):
        installer = installer.replace(
            'throw "Path escapes the owned root"',
            'Write-Verbose "path escape accepted"',
            1,
        )
        return data, signer, installer

    def remove_signer_allowlist_check(data, signer, installer):
        signer = signer.replace(
            "Assert-ReviewedSigningManifest -Manifest $manifest",
            "Missing-Reviewed-Allowlist-Check",
            1,
        )
        return data, signer, installer

    def remove_release_configuration_guard(data, signer, installer):
        signer = signer.replace(
            "Release signing requires CMake configuration Release",
            "Missing-Release-Configuration-Guard",
            1,
        )
        return data, signer, installer

    def remove_build_root_binding(data, signer, installer):
        signer = signer.replace(
            "Release build root was not configured from the reviewed repository checkout",
            "Missing-Build-Root-Source-Binding",
            1,
        )
        return data, signer, installer

    def remove_reviewed_target_rebuild(data, signer, installer):
        signer = signer.replace(
            "Invoke-ReviewedTargetBuild -CMakePath $cmake -BuildRoot $BuildX64",
            "Missing-Reviewed-Target-Rebuild",
            1,
        )
        return data, signer, installer

    def remove_clean_first_rebuild(data, signer, installer):
        signer = signer.replace('"--clean-first", ', "", 1)
        return data, signer, installer

    def remove_pe_architecture_check(data, signer, installer):
        signer = signer.replace(
            "Assert-X64PortableExecutable -Path $source",
            "Missing-X64-PE-Check",
            1,
        )
        return data, signer, installer

    def remove_unsigned_source_check(data, signer, installer):
        signer = signer.replace(
            "Reviewed CMake release output must be unsigned before release signing",
            "Missing-Unsigned-Source-Check",
            1,
        )
        return data, signer, installer

    def reintroduce_x86_host_package(data, signer, installer):
        data["artifacts"][0]["architectures"] = ["x64", "x86"]
        return data, signer, installer

    def remove_x64_installer_guard(data, signer, installer):
        installer = installer.replace(
            "if (-not [Environment]::Is64BitOperatingSystem) {",
            "if ($false) {",
            1,
        )
        return data, signer, installer

    def narrow_running_process_guard(data, signer, installer):
        installer = installer.replace(
            "$ProcessesThatMustBeStopped = @($OwnedFiles | Where-Object {",
            "$ProcessesThatMustBeStopped = @(\"HydraSeat\") # weakened",
            1,
        )
        return data, signer, installer

    def weaken_leaf_reparse_guard(data, signer, installer):
        installer = installer.replace(
            'throw "$Label must be a normal file and not a reparse point"',
            'Write-Verbose "leaf reparse accepted"',
            1,
        )
        return data, signer, installer

    def remove_provenance_leaf_guard(data, signer, installer):
        installer = installer.replace(
            'Assert-OwnedLeafNotReparsePoint -Path $provenancePath -Label "HydraSeat signing provenance"',
            'Missing-Provenance-Leaf-Guard',
            1,
        )
        return data, signer, installer

    def remove_install_destination_leaf_guard(data, signer, installer):
        installer = installer.replace(
            'Assert-OwnedLeafNotReparsePoint -Path $destination -Label "HydraSeat install destination $($file.fileName)"',
            'Missing-Install-Destination-Leaf-Guard',
            1,
        )
        return data, signer, installer

    def remove_staged_package_verification(data, signer, installer):
        installer = installer.replace(
            "    Verify-StagedPackage -Package $package -StageRoot $snapshot.stage -ExpectedSigner $OwnSigner\n",
            "",
            1,
        )
        return data, signer, installer

    def remove_readonly_pending_guard(data, signer, installer):
        installer = installer.replace("    Assert-NoPendingInstallerTransactions\n", "", 1)
        return data, signer, installer

    def remove_interrupted_recovery_call(data, signer, installer):
        installer = installer.replace("Recover-InterruptedTransactions\n", "", 1)
        return data, signer, installer

    def remove_mutation_lock_call(data, signer, installer):
        installer = installer.replace("$MutationLock = Enter-InstallerMutationLock\n", "", 1)
        return data, signer, installer

    def remove_one_committed_marker(data, signer, installer):
        installer = installer.replace('-Phase "committed"', '-Phase "prepared"', 1)
        return data, signer, installer

    def make_snapshot_cleanup_unconditional(data, signer, installer):
        installer = installer.replace(
            'if ($cleanupSnapshot -and (Test-Path -LiteralPath $snapshot.root)) {',
            'if (Test-Path -LiteralPath $snapshot.root) {',
            1,
        )
        return data, signer, installer

    def remove_one_durable_flush(data, signer, installer):
        installer = installer.replace("        $stream.Flush($true)\n", "", 1)
        return data, signer, installer

    def remove_snapshot_identity_guard(data, signer, installer):
        installer = installer.replace(
            'Get-TransactionSnapshotIdentity -Backup $Snapshot.backup',
            'Missing-Transaction-Snapshot-Identity-Check',
            1,
        )
        return data, signer, installer

    def remove_exact_repair_identity_guard(data, signer, installer):
        installer = installer.replace(
            "Repair requires the exact installed release identity; use the approved update/rollback flow for a different release",
            "Repair may change release identity",
            1,
        )
        return data, signer, installer

    def remove_installed_state_identity_verification(data, signer, installer):
        installer = installer.replace(
            "Installed state identity does not match the verified release package",
            "Missing installed-state identity verification",
            1,
        )
        return data, signer, installer

    def remove_uninstall_registration_verification(data, signer, installer):
        installer = installer.replace(
            "Uninstall registration does not match the verified installed release",
            "Missing uninstall registration verification",
            1,
        )
        return data, signer, installer

    for mutate, label in (
        (drop_installer_artifact, "missing signed installer script"),
        (drift_owned_files, "signing/installer file-set drift"),
        (remove_script_signing, "missing PowerShell signing path"),
        (broad_programdata_delete, "broad ProgramData recursive deletion"),
        (duplicate_state_weakening, "weakened installed-state duplicate detection"),
        (force_validate_elevation, "forced elevation for read-only validation"),
        (remove_provenance_signature_check, "missing detached provenance verification"),
        (remove_provenance_size_bound, "unbounded release provenance parsing"),
        (weaken_exact_artifact_identity, "release artifact identity not bound to the reviewed product contract"),
        (weaken_path_escape_guard, "release/install path escape guard removed"),
        (remove_signer_allowlist_check, "missing signer-side reviewed allowlist verification"),
        (remove_release_configuration_guard, "non-Release configuration accepted by signer"),
        (remove_build_root_binding, "release build root not bound to reviewed checkout"),
        (remove_reviewed_target_rebuild, "release signer does not rebuild reviewed targets"),
        (remove_clean_first_rebuild, "release signer can reuse stale build outputs"),
        (remove_pe_architecture_check, "release signer does not verify x64 PE machine"),
        (remove_unsigned_source_check, "release signer accepts pre-signed build output"),
        (reintroduce_x86_host_package, "x86 host release package outside frozen v1 scope"),
        (remove_x64_installer_guard, "missing x64-only v1 installer guard"),
        (narrow_running_process_guard, "installer does not block every shipped executable process"),
        (weaken_leaf_reparse_guard, "leaf reparse points are accepted"),
        (remove_provenance_leaf_guard, "package provenance leaf reparse guard is missing"),
        (remove_install_destination_leaf_guard, "Program Files destination leaf reparse guard is missing"),
        (remove_staged_package_verification, "package bytes are not reverified after staging before Program Files mutation"),
        (remove_readonly_pending_guard, "missing read-only interrupted-transaction guard"),
        (remove_interrupted_recovery_call, "missing elevated interrupted-transaction recovery"),
        (remove_mutation_lock_call, "missing installer mutation serialization"),
        (remove_one_committed_marker, "missing committed transaction marker"),
        (make_snapshot_cleanup_unconditional, "unconditional deletion of rollback evidence"),
        (remove_one_durable_flush, "non-durable install/transaction state write"),
        (remove_snapshot_identity_guard, "rollback backup integrity not bound to prepared transaction journal"),
        (remove_exact_repair_identity_guard, "repair path that can change release identity"),
        (remove_installed_state_identity_verification, "missing installed-state identity verification"),
        (remove_uninstall_registration_verification, "missing uninstall registration verification"),
    ):
        expect_rejected(manifest_data, signer_text, installer_text, mutate, label)

    def escalate_controlled_evidence(installer, harness):
        return installer, harness.replace(
            'CONTROLLED_EVIDENCE_CLASS = "Controlled"',
            'CONTROLLED_EVIDENCE_CLASS = "Physical"',
            1,
        )

    def weaken_controlled_root(installer, harness):
        return installer, harness.replace(
            'candidate.parent != base',
            'False',
            1,
        )

    def invoke_shipping_installer(installer, harness):
        return installer, harness + '\n# powershell.exe install_hydraseat.ps1\n'

    def remove_controlled_fault(installer, harness):
        return installer, harness.replace('"during-commit"', '"removed-during-commit"')

    def add_shipping_test_root(installer, harness):
        return installer + '\n$ControlledTestRoot = $env:TEMP\n', harness

    for mutate, label in (
        (escalate_controlled_evidence, "evidence-class escalation"),
        (weaken_controlled_root, "unsafe arbitrary root acceptance"),
        (invoke_shipping_installer, "harness invocation of shipping installer"),
        (remove_controlled_fault, "missing reviewed fault point"),
        (add_shipping_test_root, "shipping installer hidden test-root bypass"),
    ):
        expect_controlled_harness_rejected(
            installer_text, harness_text, mutate, label
        )


def main(argv: list[str]) -> int:
    if len(argv) > 2:
        print("usage: validate_release_installer_contract.py [--self-test]", file=sys.stderr)
        return 2
    try:
        if len(argv) == 2:
            if argv[1] != "--self-test":
                print("usage: validate_release_installer_contract.py [--self-test]", file=sys.stderr)
                return 2
            self_test()
            print("Release installer contract validator self-test passed.")
        else:
            validate_live()
            print("Release installer contract valid.")
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"Release installer contract invalid: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
