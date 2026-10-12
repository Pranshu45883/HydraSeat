#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet("Install", "Repair", "Uninstall", "Validate")][string]$Mode,
    [string]$PackageRoot,
    [switch]$LaunchAfterInstall,
    [switch]$RemoveHydraSeatUserData
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ProductName = "HydraSeat"
$InstallRoot = Join-Path $env:ProgramFiles "HydraSeat"
$DataRoot = Join-Path $env:ProgramData "HydraSeat"
$UserDataRoot = Join-Path $env:LOCALAPPDATA "HydraSeat"
$StatePath = Join-Path $DataRoot "install-state.json"
$TransactionRoot = Join-Path $DataRoot "installer-transactions"
$UninstallKey = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\HydraSeat"
if (-not [Environment]::Is64BitOperatingSystem) {
    throw "HydraSeat v1 release installer requires x64 Windows"
}
$Architecture = "x64"
$CoreOwnedFiles = @(
    "HydraSeat.exe",
    "hydra_host.exe",
    "hydra_xinput_adapter.dll",
    "hydra_gate_c_adapter.dll",
    "hydra_gate_c_shim.dll",
    "hydra_gate_c_external_bridge.dll",
    "hydra_watchdog.exe",
    "hydra_reset.exe",
    "hydraseat_profilectl.exe",
    "hydraseat_community_validate.exe",
    "install_hydraseat.ps1"
)
# The verified package/install state may append bounded third-party runtime files
# (for example Qt platform plugins). Core product files remain mandatory.
$OwnedFiles = @($CoreOwnedFiles)
$MaximumOwnedFiles = 64
$OwnedArtifactIds = @{
    "HydraSeat.exe" = "main-ui"
    "hydra_host.exe" = "host"
    "hydra_xinput_adapter.dll" = "xinput-adapter"
    "hydra_gate_c_adapter.dll" = "gate-c-adapter"
    "hydra_gate_c_shim.dll" = "gate-c-shim"
    "hydra_gate_c_external_bridge.dll" = "gate-c-external-bridge"
    "hydra_watchdog.exe" = "watchdog"
    "hydra_reset.exe" = "reset"
    "hydraseat_profilectl.exe" = "profile-cli"
    "hydraseat_community_validate.exe" = "community-validator"
    "install_hydraseat.ps1" = "installer-script"
}
$ProcessesThatMustBeStopped = @($CoreOwnedFiles | Where-Object {
    $_.EndsWith(".exe", [StringComparison]::OrdinalIgnoreCase)
} | ForEach-Object {
    [IO.Path]::GetFileNameWithoutExtension($_)
})
$TransactionStateFileName = "transaction-state.json"
$MaximumInstallerTransactions = 8
$MaximumTransactionStateBytes = 4096
$MaximumSigningProvenanceBytes = 262144
$MaximumDetachedProvenanceSignatureBytes = 262144
$QtRuntimeProvider = "Qt Project"
$QtRuntimeVersion = "6.8.3"
$QtRuntimeDeploymentTool = "windeployqt"
$VcRuntimeProvider = "Microsoft"
$VcRuntimeVersion = "VS2022"
$VcRuntimeDeploymentTool = "visual-studio-redist"
$VcRuntimeInstallerFile = "vc_redist.x64.exe"
$RequiredQtRuntimeFiles = @(
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "platforms\qwindows.dll"
)

function Assert-SafeOwnedRelativePath {
    param([string]$Child)
    if ([string]::IsNullOrWhiteSpace($Child) -or
        [IO.Path]::IsPathRooted($Child) -or
        $Child.Length -gt 240 -or
        $Child.Contains("..") -or
        $Child.Contains(":") -or
        $Child.Contains("*") -or
        $Child.Contains("?") -or
        $Child.StartsWith("\") -or
        $Child.StartsWith("/")) {
        throw "Owned file path is not a safe bounded relative path"
    }
}

function Resolve-UnderRoot {
    param([string]$Root, [string]$Child)
    Assert-SafeOwnedRelativePath -Child $Child
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $candidate = [IO.Path]::GetFullPath((Join-Path $Root $Child))
    if (-not $candidate.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes the owned root"
    }
    return $candidate
}

function Ensure-OwnedParentDirectories {
    param([string]$Root, [string]$Child)
    Assert-SafeOwnedRelativePath -Child $Child
    $normalized = $Child.Replace('/', '\')
    $parts = @($normalized.Split('\') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($parts.Count -le 1) { return }

    $relative = ""
    for ($index = 0; $index -lt ($parts.Count - 1); ++$index) {
        $relative = if ([string]::IsNullOrEmpty($relative)) {
            $parts[$index]
        } else {
            Join-Path $relative $parts[$index]
        }
        $directory = Resolve-UnderRoot -Root $Root -Child $relative
        if (Test-Path -LiteralPath $directory) {
            Assert-OwnedDirectoryNotReparsePoint -Path $directory -Label "HydraSeat owned parent directory $relative"
        } else {
            New-Item -ItemType Directory -Path $directory | Out-Null
            Assert-OwnedDirectoryNotReparsePoint -Path $directory -Label "HydraSeat owned parent directory $relative"
        }
    }
}

function Remove-EmptyOwnedParentDirectories {
    param([string]$Root, [string]$Child)
    $normalized = $Child.Replace('/', '\')
    $parts = @($normalized.Split('\') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    for ($index = $parts.Count - 2; $index -ge 0; --$index) {
        $relative = ($parts[0..$index] -join '\')
        $directory = Resolve-UnderRoot -Root $Root -Child $relative
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) { continue }
        Assert-OwnedDirectoryNotReparsePoint -Path $directory -Label "HydraSeat owned parent directory $relative"
        if (@(Get-ChildItem -LiteralPath $directory -Force).Count -ne 0) { break }
        Remove-Item -LiteralPath $directory -Force
    }
}

function Assert-OwnedDirectoryNotReparsePoint {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (-not $item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Label must be a normal directory and not a reparse point"
    }
}

function Assert-OwnedLeafNotReparsePoint {
    param([string]$Path, [string]$Label, [switch]$AllowMissing)
    if (-not (Test-Path -LiteralPath $Path)) {
        if ($AllowMissing) { return }
        throw "$Label is missing"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Label must be a normal file and not a reparse point"
    }
}

function Set-ActiveOwnedFiles {
    param([string[]]$Paths)
    $pathsArray = @($Paths)
    if ($pathsArray.Count -lt $CoreOwnedFiles.Count -or
        $pathsArray.Count -gt $MaximumOwnedFiles) {
        throw "Owned file set has an invalid bounded count"
    }

    $seen = @{}
    foreach ($path in $pathsArray) {
        Assert-SafeOwnedRelativePath -Child $path
        $folded = ([string]$path).ToUpperInvariant()
        if ($seen.ContainsKey($folded)) {
            throw "Owned file set contains a duplicate/case-fold collision"
        }
        $seen[$folded] = $true
    }
    foreach ($core in $CoreOwnedFiles) {
        if (-not $seen.ContainsKey($core.ToUpperInvariant())) {
            throw "Owned file set is missing a mandatory HydraSeat core file"
        }
    }
    $script:OwnedFiles = @($pathsArray)
}

function Get-SafeRelativeFileInventory {
    param([string]$Root, [int]$MaximumFiles = $MaximumOwnedFiles)
    Assert-OwnedDirectoryNotReparsePoint -Path $Root -Label "HydraSeat owned inventory root"

    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $pending = New-Object 'System.Collections.Generic.Queue[string]'
    $pending.Enqueue($rootFull)
    $files = New-Object 'System.Collections.Generic.List[string]'
    $directoryCount = 0

    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        foreach ($entry in @(Get-ChildItem -LiteralPath $directory -Force)) {
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Owned file inventory contains a reparse point"
            }
            if ($entry.PSIsContainer) {
                ++$directoryCount
                if ($directoryCount -gt $MaximumFiles) {
                    throw "Owned file inventory contains too many directories"
                }
                $pending.Enqueue([string]$entry.FullName)
                continue
            }

            $relative = [string]$entry.FullName
            if (-not $relative.StartsWith(
                    $rootFull + '\',
                    [StringComparison]::OrdinalIgnoreCase)) {
                throw "Owned file inventory escaped its root"
            }
            $relative = $relative.Substring($rootFull.Length + 1)
            Assert-SafeOwnedRelativePath -Child $relative
            $files.Add($relative)
            if ($files.Count -gt $MaximumFiles) {
                throw "Owned file inventory exceeds its bounded file count"
            }
        }
    }

    return @($files | Sort-Object)
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Install, Repair, and Uninstall require an elevated administrator token"
    }
}

function Get-OwnSignerThumbprint {
    if ([string]::IsNullOrWhiteSpace($PSCommandPath) -or -not (Test-Path -LiteralPath $PSCommandPath -PathType Leaf)) {
        throw "Installer must execute from its signed script file"
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $PSCommandPath
    if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or $null -eq $signature.SignerCertificate) {
        throw "Installer Authenticode signature is not Valid"
    }
    return $signature.SignerCertificate.Thumbprint.ToUpperInvariant()
}

function Assert-DetachedCmsSignature {
    param([string]$ContentPath, [string]$SignaturePath, [string]$ExpectedThumbprint)
    if (-not (Test-Path -LiteralPath $ContentPath -PathType Leaf)) {
        throw "Missing signing provenance"
    }
    if (-not (Test-Path -LiteralPath $SignaturePath -PathType Leaf)) {
        throw "Missing detached signing provenance signature"
    }
    $contentFile = Get-Item -LiteralPath $ContentPath -Force
    $signatureFile = Get-Item -LiteralPath $SignaturePath -Force
    if ($contentFile.Length -le 0 -or $contentFile.Length -gt $MaximumSigningProvenanceBytes) {
        throw "Signing provenance is empty or exceeds the bounded manifest size"
    }
    if ($signatureFile.Length -le 0 -or
        $signatureFile.Length -gt $MaximumDetachedProvenanceSignatureBytes) {
        throw "Detached signing provenance signature exceeds the bounded signature size"
    }
    try {
        Add-Type -AssemblyName System.Security
        $contentBytes = [IO.File]::ReadAllBytes($ContentPath)
        $contentInfo = New-Object System.Security.Cryptography.Pkcs.ContentInfo -ArgumentList (,$contentBytes)
        $signedCms = New-Object System.Security.Cryptography.Pkcs.SignedCms -ArgumentList @($contentInfo, $true)
        $signedCms.Decode([IO.File]::ReadAllBytes($SignaturePath))
        $signedCms.CheckSignature($true)
    } catch {
        throw "Detached signing provenance signature verification failed: $($_.Exception.Message)"
    }
    if ($signedCms.SignerInfos.Count -ne 1 -or
        $null -eq $signedCms.SignerInfos[0].Certificate -or
        $signedCms.SignerInfos[0].Certificate.Thumbprint.ToUpperInvariant() -ne $ExpectedThumbprint) {
        throw "Detached signing provenance publisher does not match the signed installer"
    }
}

function Assert-OrdinaryWindowsState {
    foreach ($name in $ProcessesThatMustBeStopped) {
        $process = Get-Process -Name $name -ErrorAction SilentlyContinue
        if ($null -ne $process) {
            throw "Close all HydraSeat release processes and return to ordinary Windows before install changes"
        }
    }
}

function Get-ValidatedPackage {
    param([string]$Root, [string]$ExpectedSigner)
    if ([string]::IsNullOrWhiteSpace($Root)) { throw "PackageRoot is required for Install/Repair/Validate" }
    $rootFull = [IO.Path]::GetFullPath($Root)
    if (-not (Test-Path -LiteralPath $rootFull -PathType Container)) { throw "Release package root is missing" }
    Assert-OwnedDirectoryNotReparsePoint -Path $rootFull -Label "HydraSeat release package root"
    $architectureRoot = Resolve-UnderRoot -Root $rootFull -Child $Architecture
    if (-not (Test-Path -LiteralPath $architectureRoot -PathType Container)) { throw "Release architecture directory is missing" }
    Assert-OwnedDirectoryNotReparsePoint -Path $architectureRoot -Label "HydraSeat release architecture directory"
    $provenancePath = Resolve-UnderRoot -Root $rootFull -Child "signing-provenance.json"
    Assert-OwnedLeafNotReparsePoint -Path $provenancePath -Label "HydraSeat signing provenance"
    $provenanceSignaturePath = Resolve-UnderRoot -Root $rootFull -Child "signing-provenance.json.p7s"
    Assert-OwnedLeafNotReparsePoint -Path $provenanceSignaturePath -Label "HydraSeat signing provenance signature"
    Assert-DetachedCmsSignature -ContentPath $provenancePath -SignaturePath $provenanceSignaturePath `
        -ExpectedThumbprint $ExpectedSigner
    $provenance = Get-Content -LiteralPath $provenancePath -Raw -Encoding UTF8 | ConvertFrom-Json
    $expectedProvenanceFields = @(
        "schemaVersion", "releaseVersion", "releaseRevision", "commitSha",
        "signingManifest", "timestampUrl", "artifacts",
        "thirdPartyRedistributables"
    )
    $actualProvenanceFields = @($provenance.PSObject.Properties.Name)
    if ($actualProvenanceFields.Count -ne $expectedProvenanceFields.Count) {
        throw "Release signing provenance contains unknown or missing fields"
    }
    foreach ($field in $expectedProvenanceFields) {
        if ($actualProvenanceFields -notcontains $field) {
            throw "Release signing provenance contains unknown or missing fields"
        }
    }
    if ($provenance.schemaVersion -ne 2 -or
        [string]$provenance.releaseVersion -notmatch "^[A-Za-z0-9._+-]{1,64}$" -or
        [UInt64]$provenance.releaseRevision -eq 0 -or
        [string]$provenance.commitSha -notmatch "^[A-Fa-f0-9]{40}$" -or
        [string]$provenance.signingManifest -ne "release-signing-manifest.json" -or
        [string]$provenance.timestampUrl -notmatch "^https://") {
        throw "Invalid release signing provenance"
    }

    $records = @($provenance.artifacts)
    if ($records.Count -ne $CoreOwnedFiles.Count) {
        throw "Release provenance does not contain the exact HydraSeat core artifact set"
    }
    $expectedRecordFields = @(
        "id", "kind", "target", "architecture", "fileName", "unsignedSha256",
        "signedSha256", "signerThumbprint", "signatureStatus"
    )
    $seen = @{}
    $validated = @()
    foreach ($record in $records) {
        $recordFields = @($record.PSObject.Properties.Name)
        if ($recordFields.Count -ne $expectedRecordFields.Count) {
            throw "Release artifact provenance contains unknown or missing fields"
        }
        foreach ($field in $expectedRecordFields) {
            if ($recordFields -notcontains $field) {
                throw "Release artifact provenance contains unknown or missing fields"
            }
        }
        $fileName = [string]$record.fileName
        Assert-SafeOwnedRelativePath -Child $fileName
        $folded = $fileName.ToUpperInvariant()
        if ($CoreOwnedFiles -notcontains $fileName -or $seen.ContainsKey($folded)) {
            throw "Unexpected or duplicate HydraSeat core release file"
        }
        $seen[$folded] = $true
        $expectedKind = if ($fileName.EndsWith(".ps1", [StringComparison]::OrdinalIgnoreCase)) {
            "powershell-script"
        } elseif ($fileName.EndsWith(".dll", [StringComparison]::OrdinalIgnoreCase)) {
            "cmake-shared-library"
        } else {
            "cmake-executable"
        }
        if ([string]$record.id -ne [string]$OwnedArtifactIds[$fileName] -or
            [string]$record.kind -ne $expectedKind -or
            [string]$record.architecture -ne $Architecture) {
            throw "Release artifact identity/type/architecture does not match the fixed HydraSeat product contract"
        }
        if ([string]$record.signerThumbprint -ne $ExpectedSigner -or [string]$record.signatureStatus -ne "Valid") {
            throw "Release provenance signer does not match the signed installer"
        }
        if ([string]$record.unsignedSha256 -notmatch "^[0-9a-f]{64}$" -or
            [string]$record.signedSha256 -notmatch "^[0-9a-f]{64}$") {
            throw "Invalid artifact SHA-256 in provenance"
        }
        $filePath = Resolve-UnderRoot -Root $architectureRoot -Child $fileName
        Assert-OwnedLeafNotReparsePoint -Path $filePath -Label "HydraSeat release file $fileName"
        $hash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne [string]$record.signedSha256) {
            throw "Release file hash mismatch: $fileName"
        }
        $signature = Get-AuthenticodeSignature -LiteralPath $filePath
        if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
            $null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.Thumbprint.ToUpperInvariant() -ne $ExpectedSigner) {
            throw "Release file publisher/signature mismatch: $fileName"
        }
        $validated += [ordered]@{
            fileName = $fileName
            sourcePath = $filePath
            sha256 = $hash
            productSigned = $true
        }
    }
    foreach ($fileName in $CoreOwnedFiles) {
        if (-not $seen.ContainsKey($fileName.ToUpperInvariant())) {
            throw "Release package missing core file: $fileName"
        }
    }

    $thirdPartyRecords = @($provenance.thirdPartyRedistributables)
    if ($thirdPartyRecords.Count -lt ($RequiredQtRuntimeFiles.Count + 1) -or
        ($thirdPartyRecords.Count + $CoreOwnedFiles.Count) -gt $MaximumOwnedFiles) {
        throw "Release package contains an invalid bounded third-party runtime file set"
    }
    $prerequisites = @()
    $expectedThirdPartyFields = @(
        "id", "provider", "version", "deploymentTool",
        "relativePath", "sha256", "bytes"
    )
    foreach ($record in $thirdPartyRecords) {
        $fields = @($record.PSObject.Properties.Name)
        if ($fields.Count -ne $expectedThirdPartyFields.Count) {
            throw "Third-party runtime provenance contains unknown or missing fields"
        }
        foreach ($field in $expectedThirdPartyFields) {
            if ($fields -notcontains $field) {
                throw "Third-party runtime provenance contains unknown or missing fields"
            }
        }
        if ([UInt64]$record.bytes -eq 0 -or
            [UInt64]$record.bytes -gt 1073741824 -or
            [string]$record.sha256 -notmatch "^[0-9a-f]{64}$") {
            throw "Third-party runtime provenance does not match the reviewed contract"
        }

        $relativePath = ([string]$record.relativePath).Replace('/', '\')
        Assert-SafeOwnedRelativePath -Child $relativePath
        $folded = $relativePath.ToUpperInvariant()
        if ($seen.ContainsKey($folded)) {
            throw "Release package contains a duplicate/colliding runtime path"
        }
        $seen[$folded] = $true

        $filePath = Resolve-UnderRoot -Root $architectureRoot -Child $relativePath
        Assert-OwnedLeafNotReparsePoint -Path $filePath -Label "Third-party runtime file $relativePath"
        $file = Get-Item -LiteralPath $filePath -Force
        if ([UInt64]$file.Length -ne [UInt64]$record.bytes) {
            throw "Third-party runtime size mismatch: $relativePath"
        }
        $hash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne [string]$record.sha256) {
            throw "Third-party runtime hash mismatch: $relativePath"
        }

        if ([string]$record.id -eq "qt-runtime") {
            if ([string]$record.provider -ne $QtRuntimeProvider -or
                [string]$record.version -ne $QtRuntimeVersion -or
                [string]$record.deploymentTool -ne $QtRuntimeDeploymentTool -or
                -not $relativePath.EndsWith(".dll", [StringComparison]::OrdinalIgnoreCase)) {
                throw "Third-party runtime provenance does not match the reviewed Qt contract"
            }

            $validated += [ordered]@{
                fileName = $relativePath
                sourcePath = $filePath
                sha256 = $hash
                productSigned = $false
            }
            continue
        }

        if ([string]$record.id -eq "msvc-runtime-installer") {
            if ([string]$record.provider -ne $VcRuntimeProvider -or
                [string]$record.version -ne $VcRuntimeVersion -or
                [string]$record.deploymentTool -ne $VcRuntimeDeploymentTool -or
                -not $relativePath.Equals($VcRuntimeInstallerFile, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Third-party runtime provenance does not match the reviewed Visual C++ prerequisite contract"
            }

            $signature = Get-AuthenticodeSignature -LiteralPath $filePath
            if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
                $null -eq $signature.SignerCertificate -or
                [string]$signature.SignerCertificate.Subject -notmatch
                    '(^|,\s*)CN=Microsoft Corporation(,|$)') {
                throw "Visual C++ Redistributable prerequisite Authenticode signature is invalid"
            }

            $prerequisites += [ordered]@{
                fileName = $relativePath
                sourcePath = $filePath
                sha256 = $hash
            }
            continue
        }

        throw "Release package contains an unreviewed third-party runtime identity"
    }

    foreach ($required in $RequiredQtRuntimeFiles) {
        if (-not $seen.ContainsKey($required.ToUpperInvariant())) {
            throw "Release package missing required Qt runtime file: $required"
        }
    }
    if (-not $seen.ContainsKey($VcRuntimeInstallerFile.ToUpperInvariant()) -or
        $prerequisites.Count -ne 1) {
        throw "Release package is missing the reviewed Visual C++ Redistributable prerequisite"
    }

    $actualFiles = @(
        Get-SafeRelativeFileInventory -Root $architectureRoot -MaximumFiles $MaximumOwnedFiles
    )
    if ($actualFiles.Count -ne $seen.Count) {
        throw "Release architecture directory does not contain the exact provenance-bound file set"
    }
    foreach ($actual in $actualFiles) {
        if (-not $seen.ContainsKey(([string]$actual).ToUpperInvariant())) {
            throw "Release architecture directory contains an unexpected file: $actual"
        }
    }

    return [ordered]@{
        releaseVersion = [string]$provenance.releaseVersion
        releaseRevision = [UInt64]$provenance.releaseRevision
        commitSha = [string]$provenance.commitSha
        architecture = $Architecture
        files = $validated
        prerequisites = $prerequisites
    }
}

function Convert-ToRuntimeVersion {
    param([string]$Value)
    if ([string]::IsNullOrWhiteSpace($Value)) { return $null }
    $match = [regex]::Match(
        $Value,
        '(?<v>\d+\.\d+\.\d+(?:\.\d+)?)')
    if (-not $match.Success) { return $null }
    try {
        return [version]$match.Groups["v"].Value
    } catch {
        return $null
    }
}

function Get-InstalledVcRuntimeVersion {
    $keys = @(
        "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64",
        "HKLM:\SOFTWARE\Wow6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"
    )
    $versions = @()
    foreach ($key in $keys) {
        if (-not (Test-Path -LiteralPath $key)) { continue }
        try {
            $runtime = Get-ItemProperty -LiteralPath $key -ErrorAction Stop
        } catch {
            continue
        }
        if ([int]$runtime.Installed -ne 1) { continue }
        $version = Convert-ToRuntimeVersion -Value ([string]$runtime.Version)
        if ($null -ne $version) {
            $versions += $version
        }
    }
    if ($versions.Count -eq 0) { return $null }
    return @($versions | Sort-Object -Descending)[0]
}

function Get-VcRedistPackageVersion {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Visual C++ Redistributable prerequisite must not be a reparse point"
    }
    $versionText =
        [Diagnostics.FileVersionInfo]::GetVersionInfo($Path).FileVersion
    $version = Convert-ToRuntimeVersion -Value ([string]$versionText)
    if ($null -eq $version -or $version.Major -lt 14) {
        throw "Visual C++ Redistributable prerequisite version is invalid"
    }
    return $version
}

function Ensure-VcRuntimePrerequisite {
    param($Package, [string]$StageRoot)

    $prerequisites = @($Package.prerequisites)
    if ($prerequisites.Count -ne 1) {
        throw "Release package must contain exactly one reviewed Visual C++ prerequisite"
    }
    $prerequisite = $prerequisites[0]
    if ([string]$prerequisite.fileName -ne $VcRuntimeInstallerFile) {
        throw "Release package Visual C++ prerequisite identity is invalid"
    }

    $stagedPath =
        Resolve-UnderRoot -Root $StageRoot -Child $VcRuntimeInstallerFile
    Copy-Item -LiteralPath ([string]$prerequisite.sourcePath) -Destination $stagedPath -Force
    try {
        Assert-OwnedLeafNotReparsePoint -Path $stagedPath -Label "Staged Visual C++ Redistributable prerequisite"
        $stagedHash =
            (Get-FileHash -LiteralPath $stagedPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($stagedHash -ne [string]$prerequisite.sha256) {
            throw "Staged Visual C++ Redistributable prerequisite hash verification failed"
        }

        $signature = Get-AuthenticodeSignature -LiteralPath $stagedPath
        if ($signature.Status -ne
                [Management.Automation.SignatureStatus]::Valid -or
            $null -eq $signature.SignerCertificate -or
            [string]$signature.SignerCertificate.Subject -notmatch
                '(^|,\s*)CN=Microsoft Corporation(,|$)') {
            throw "Staged Visual C++ Redistributable prerequisite is not validly signed by Microsoft"
        }

        $requiredVersion = Get-VcRedistPackageVersion -Path $stagedPath
        $installedVersion = Get-InstalledVcRuntimeVersion
        if ($null -ne $installedVersion -and
            $installedVersion -ge $requiredVersion) {
            return
        }

        $process = Start-Process -FilePath $stagedPath -ArgumentList @("/install", "/quiet", "/norestart") -Wait -PassThru
        $after = Get-InstalledVcRuntimeVersion
        if ($null -eq $after -or $after -lt $requiredVersion) {
            throw "Visual C++ Redistributable prerequisite did not reach the required version (exit=$($process.ExitCode))"
        }
    } finally {
        if (Test-Path -LiteralPath $stagedPath) {
            Remove-Item -LiteralPath $stagedPath -Force
        }
    }
}

function Read-InstallStateFile {
    param([string]$Path, [switch]$AllowMissing)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        if ($AllowMissing) { return $null }
        throw "HydraSeat install state file is missing"
    }
    Assert-OwnedLeafNotReparsePoint -Path $Path -Label "HydraSeat install state"
    $stateFile = Get-Item -LiteralPath $Path -Force
    if ($stateFile.Length -le 0 -or $stateFile.Length -gt 65536) {
        throw "Existing HydraSeat install state size is invalid"
    }
    $state = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    $expectedFields = @("schemaVersion", "releaseVersion", "releaseRevision", "commitSha", "architecture", "installRoot", "startupMode", "ownedFiles")
    $actualFields = @($state.PSObject.Properties.Name)
    if ($actualFields.Count -ne $expectedFields.Count) {
        throw "Existing HydraSeat install state contains unknown or missing fields"
    }
    foreach ($field in $expectedFields) {
        if ($actualFields -notcontains $field) {
            throw "Existing HydraSeat install state contains unknown or missing fields"
        }
    }
    $stateInstallRoot = [string]$state.installRoot
    if ($state.schemaVersion -ne 1 -or
        [string]$state.releaseVersion -notmatch "^[A-Za-z0-9._+-]{1,64}$" -or
        [UInt64]$state.releaseRevision -eq 0 -or
        [string]$state.commitSha -notmatch "^[A-Fa-f0-9]{40}$" -or
        [string]$state.architecture -ne $Architecture -or
        -not $stateInstallRoot.Equals($InstallRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [string]$state.startupMode -ne "Manual" -or
        @($state.ownedFiles).Count -lt $CoreOwnedFiles.Count -or
        @($state.ownedFiles).Count -gt $MaximumOwnedFiles) {
        throw "Existing HydraSeat install state is invalid; do not guess repair ownership"
    }

    $seenOwnedFiles = @{}
    foreach ($file in @($state.ownedFiles)) {
        $fileFields = @($file.PSObject.Properties.Name)
        $fileName = [string]$file.fileName
        Assert-SafeOwnedRelativePath -Child $fileName
        $folded = $fileName.ToUpperInvariant()
        if ($fileFields.Count -ne 2 -or
            $fileFields -notcontains "fileName" -or
            $fileFields -notcontains "sha256" -or
            $seenOwnedFiles.ContainsKey($folded) -or
            [string]$file.sha256 -notmatch "^[0-9a-f]{64}$") {
            throw "Existing install state contains unknown or duplicate owned file metadata"
        }
        $seenOwnedFiles[$folded] = $true
    }
    foreach ($fileName in $CoreOwnedFiles) {
        if (-not $seenOwnedFiles.ContainsKey($fileName.ToUpperInvariant())) {
            throw "Existing install state is missing a mandatory core file"
        }
    }
    return $state
}

function Read-InstallState {
    return Read-InstallStateFile -Path $StatePath -AllowMissing
}

function Write-InstallState {
    param($Package)
    New-Item -ItemType Directory -Force -Path $DataRoot | Out-Null
    $state = [ordered]@{
        schemaVersion = 1
        releaseVersion = $Package.releaseVersion
        releaseRevision = $Package.releaseRevision
        commitSha = $Package.commitSha
        architecture = $Package.architecture
        installRoot = $InstallRoot
        startupMode = "Manual"
        ownedFiles = @($Package.files | ForEach-Object { [ordered]@{ fileName = $_.fileName; sha256 = $_.sha256 } })
    }
    $temp = $StatePath + ".new"
    if (Test-Path -LiteralPath $temp) {
        Assert-OwnedLeafNotReparsePoint -Path $temp -Label "HydraSeat staged install state"
        Remove-Item -LiteralPath $temp -Force
    }
    if (Test-Path -LiteralPath $StatePath) {
        Assert-OwnedLeafNotReparsePoint -Path $StatePath -Label "HydraSeat install state destination"
    }
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temp -Encoding UTF8
    Assert-OwnedLeafNotReparsePoint -Path $temp -Label "HydraSeat staged install state"
    $tempFile = Get-Item -LiteralPath $temp -Force
    if ($tempFile.Length -le 0 -or $tempFile.Length -gt 65536) {
        Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue
        throw "HydraSeat install state exceeds its bounded size"
    }
    $stream = [IO.File]::Open($temp, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
    Move-Item -LiteralPath $temp -Destination $StatePath -Force
}

function Register-Uninstall {
    param([string]$Version)
    New-Item -Path $UninstallKey -Force | Out-Null
    $installedScript = Resolve-UnderRoot -Root $InstallRoot -Child "install_hydraseat.ps1"
    $uninstallCommand = 'powershell.exe -NoProfile -ExecutionPolicy AllSigned -File "' + $installedScript + '" -Mode Uninstall'
    New-ItemProperty -Path $UninstallKey -Name DisplayName -Value $ProductName -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name DisplayVersion -Value $Version -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name Publisher -Value "HydraSeat" -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name InstallLocation -Value $InstallRoot -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name DisplayIcon -Value (Join-Path $InstallRoot "HydraSeat.exe") -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name UninstallString -Value $uninstallCommand -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name NoRepair -Value 0 -PropertyType DWord -Force | Out-Null
}

function Remove-UninstallRegistration {
    if (Test-Path -LiteralPath $UninstallKey) {
        Remove-Item -LiteralPath $UninstallKey -Recurse -Force
    }
}

function Write-TransactionState {
    param(
        [string]$Root,
        [string]$TransactionId,
        [string]$Phase,
        [string]$Operation,
        [bool]$PreviousStatePresent,
        [string]$SnapshotIdentity
    )
    if ($TransactionId -notmatch "^[0-9a-f]{32}$" -or
        $Phase -notin @("snapshotting", "prepared", "staged", "committed") -or
        $Operation -notin @("Install", "Repair", "Uninstall")) {
        throw "Installer transaction state arguments are invalid"
    }
    if (($Phase -eq "snapshotting" -and -not [string]::IsNullOrEmpty($SnapshotIdentity)) -or
        ($Phase -ne "snapshotting" -and $SnapshotIdentity -notmatch "^[0-9a-f]{64}$")) {
        throw "Installer transaction snapshot identity is invalid"
    }
    $state = [ordered]@{
        schemaVersion = 1
        transactionId = $TransactionId
        phase = $Phase
        operation = $Operation
        previousStatePresent = $PreviousStatePresent
        snapshotIdentity = $SnapshotIdentity
    }
    $path = Resolve-UnderRoot -Root $Root -Child $TransactionStateFileName
    $temp = $path + ".new"
    if (Test-Path -LiteralPath $temp) {
        Assert-OwnedLeafNotReparsePoint -Path $temp -Label "HydraSeat staged transaction state"
        Remove-Item -LiteralPath $temp -Force
    }
    if (Test-Path -LiteralPath $path) {
        Assert-OwnedLeafNotReparsePoint -Path $path -Label "HydraSeat transaction state destination"
    }
    $state | ConvertTo-Json -Compress | Set-Content -LiteralPath $temp -Encoding UTF8
    Assert-OwnedLeafNotReparsePoint -Path $temp -Label "HydraSeat staged transaction state"
    $tempFile = Get-Item -LiteralPath $temp -Force
    if ($tempFile.Length -le 0 -or $tempFile.Length -gt $MaximumTransactionStateBytes) {
        Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue
        throw "Installer transaction state exceeds its bounded size"
    }
    $stream = [IO.File]::Open($temp, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
    Move-Item -LiteralPath $temp -Destination $path -Force
}

function Read-TransactionState {
    param([string]$Root)
    $transactionId = Split-Path -Leaf $Root
    if ($transactionId -notmatch "^[0-9a-f]{32}$") {
        throw "Installer transaction directory name is invalid"
    }
    Assert-OwnedDirectoryNotReparsePoint -Path $Root -Label "HydraSeat installer transaction"
    $path = Resolve-UnderRoot -Root $Root -Child $TransactionStateFileName
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Installer transaction state marker is missing; refusing to guess interrupted mutation state"
    }
    Assert-OwnedLeafNotReparsePoint -Path $path -Label "HydraSeat installer transaction state marker"
    $file = Get-Item -LiteralPath $path -Force
    if ($file.Length -le 0 -or $file.Length -gt $MaximumTransactionStateBytes) {
        throw "Installer transaction state marker size is invalid"
    }
    $state = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    $expectedFields = @(
        "schemaVersion", "transactionId", "phase", "operation", "previousStatePresent", "snapshotIdentity"
    )
    $actualFields = @($state.PSObject.Properties.Name)
    if ($actualFields.Count -ne $expectedFields.Count) {
        throw "Installer transaction state contains unknown or missing fields"
    }
    foreach ($field in $expectedFields) {
        if ($actualFields -notcontains $field) {
            throw "Installer transaction state contains unknown or missing fields"
        }
    }
    if ($state.schemaVersion -ne 1 -or [string]$state.transactionId -ne $transactionId -or
        [string]$state.phase -notin @("snapshotting", "prepared", "staged", "committed") -or
        [string]$state.operation -notin @("Install", "Repair", "Uninstall") -or
        $state.previousStatePresent -isnot [bool]) {
        throw "Installer transaction state marker is invalid"
    }
    $snapshotIdentity = [string]$state.snapshotIdentity
    if (([string]$state.phase -eq "snapshotting" -and -not [string]::IsNullOrEmpty($snapshotIdentity)) -or
        ([string]$state.phase -ne "snapshotting" -and $snapshotIdentity -notmatch "^[0-9a-f]{64}$")) {
        throw "Installer transaction snapshot identity is invalid"
    }
    return $state
}

function Get-TransactionSnapshotIdentity {
    param([string]$Backup, [bool]$PreviousStatePresent)
    Assert-OwnedDirectoryNotReparsePoint -Path $Backup -Label "HydraSeat installer transaction backup"
    $parts = @("HydraSeatInstallerSnapshotV2")
    $inventory = @(
        Get-SafeRelativeFileInventory -Root $Backup -MaximumFiles ($MaximumOwnedFiles + 1)
    )
    $statePresent = $inventory -contains "install-state.json"
    if ($statePresent -ne $PreviousStatePresent) {
        throw "Installer transaction backup state presence changed"
    }

    foreach ($relativePath in $inventory) {
        $backupFile = Resolve-UnderRoot -Root $Backup -Child $relativePath
        Assert-OwnedLeafNotReparsePoint -Path $backupFile -Label "HydraSeat transaction backup $relativePath"
        $hash = (Get-FileHash -LiteralPath $backupFile -Algorithm SHA256).Hash.ToLowerInvariant()
        $parts += "$relativePath|$hash"
    }
    $canonical = [Text.Encoding]::UTF8.GetBytes(($parts -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($canonical))).Replace("-", "").ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function New-TransactionSnapshot {
    param($PreviousState, [string]$Operation)
    New-Item -ItemType Directory -Force -Path $TransactionRoot | Out-Null
    Assert-OwnedDirectoryNotReparsePoint -Path $TransactionRoot -Label "HydraSeat installer transaction root"
    $id = [Guid]::NewGuid().ToString("N")
    $root = Resolve-UnderRoot -Root $TransactionRoot -Child $id
    $backup = Join-Path $root "backup"
    $stage = Join-Path $root "stage"
    New-Item -ItemType Directory -Force -Path $backup | Out-Null
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Assert-OwnedDirectoryNotReparsePoint -Path $root -Label "HydraSeat installer transaction"
    Assert-OwnedDirectoryNotReparsePoint -Path $backup -Label "HydraSeat installer transaction backup"
    Assert-OwnedDirectoryNotReparsePoint -Path $stage -Label "HydraSeat installer transaction stage"
    $previousStatePresent = $null -ne $PreviousState
    Write-TransactionState -Root $root -TransactionId $id -Phase "snapshotting" `
        -Operation $Operation -PreviousStatePresent $previousStatePresent -SnapshotIdentity ""
    try {
        foreach ($fileName in $OwnedFiles) {
            $installed = Resolve-UnderRoot -Root $InstallRoot -Child $fileName
            if (Test-Path -LiteralPath $installed) {
                Assert-OwnedLeafNotReparsePoint -Path $installed -Label "Installed HydraSeat owned file $fileName"
                Ensure-OwnedParentDirectories -Root $backup -Child $fileName
                $backupDestination = Resolve-UnderRoot -Root $backup -Child $fileName
                Copy-Item -LiteralPath $installed -Destination $backupDestination -Force
            }
        }
        if (Test-Path -LiteralPath $StatePath) {
            Assert-OwnedLeafNotReparsePoint -Path $StatePath -Label "HydraSeat install state snapshot source"
            Copy-Item -LiteralPath $StatePath -Destination (Join-Path $backup "install-state.json") -Force
        }
        $snapshotIdentity = Get-TransactionSnapshotIdentity -Backup $backup `
            -PreviousStatePresent $previousStatePresent
        Write-TransactionState -Root $root -TransactionId $id -Phase "prepared" `
            -Operation $Operation -PreviousStatePresent $previousStatePresent -SnapshotIdentity $snapshotIdentity
    } catch {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
        throw
    }
    return [ordered]@{
        id = $id
        root = $root
        backup = $backup
        stage = $stage
        previousState = $PreviousState
        operation = $Operation
        snapshotIdentity = $snapshotIdentity
    }
}

function Restore-TransactionSnapshot {
    param($Snapshot)
    $actualSnapshotIdentity = Get-TransactionSnapshotIdentity -Backup $Snapshot.backup `
        -PreviousStatePresent ($null -ne $Snapshot.previousState)
    if ($actualSnapshotIdentity -ne [string]$Snapshot.snapshotIdentity) {
        throw "Installer transaction backup changed after its prepared snapshot was journaled"
    }
    New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
    foreach ($fileName in $OwnedFiles) {
        $installed = Resolve-UnderRoot -Root $InstallRoot -Child $fileName
        if (Test-Path -LiteralPath $installed) {
            Assert-OwnedLeafNotReparsePoint -Path $installed -Label "Installed HydraSeat owned file $fileName"
            Remove-Item -LiteralPath $installed -Force
            Remove-EmptyOwnedParentDirectories -Root $InstallRoot -Child $fileName
        }
        $backupFile = Resolve-UnderRoot -Root $Snapshot.backup -Child $fileName
        if (Test-Path -LiteralPath $backupFile) {
            Assert-OwnedLeafNotReparsePoint -Path $backupFile -Label "HydraSeat rollback backup file $fileName"
            Ensure-OwnedParentDirectories -Root $InstallRoot -Child $fileName
            Copy-Item -LiteralPath $backupFile -Destination $installed -Force
        }
    }
    $backupState = Resolve-UnderRoot -Root $Snapshot.backup -Child "install-state.json"
    if (Test-Path -LiteralPath $backupState) {
        Assert-OwnedLeafNotReparsePoint -Path $backupState -Label "HydraSeat rollback install-state backup"
        New-Item -ItemType Directory -Force -Path $DataRoot | Out-Null
        if (Test-Path -LiteralPath $StatePath) {
            Assert-OwnedLeafNotReparsePoint -Path $StatePath -Label "HydraSeat install state rollback destination"
        }
        Copy-Item -LiteralPath $backupState -Destination $StatePath -Force
        Register-Uninstall -Version ([string]$Snapshot.previousState.releaseVersion)
    } else {
        if (Test-Path -LiteralPath $StatePath) {
            Assert-OwnedLeafNotReparsePoint -Path $StatePath -Label "HydraSeat install state removal target"
            Remove-Item -LiteralPath $StatePath -Force
        }
        Remove-UninstallRegistration
    }
}

function Verify-RestoredTransactionSnapshot {
    param($Snapshot)
    foreach ($fileName in $OwnedFiles) {
        $backupFile = Resolve-UnderRoot -Root $Snapshot.backup -Child $fileName
        $installed = Resolve-UnderRoot -Root $InstallRoot -Child $fileName
        $backupExists = Test-Path -LiteralPath $backupFile
        $installedExists = Test-Path -LiteralPath $installed
        if ($backupExists -ne $installedExists) {
            throw "Installer rollback verification found an owned-file presence mismatch: $fileName"
        }
        if ($backupExists) {
            Assert-OwnedLeafNotReparsePoint -Path $backupFile -Label "HydraSeat rollback verification backup $fileName"
            Assert-OwnedLeafNotReparsePoint -Path $installed -Label "HydraSeat rollback verification installed file $fileName"
            $backupHash = (Get-FileHash -LiteralPath $backupFile -Algorithm SHA256).Hash
            $installedHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
            if (-not $backupHash.Equals($installedHash, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Installer rollback verification found an owned-file hash mismatch: $fileName"
            }
        }
    }

    $backupState = Resolve-UnderRoot -Root $Snapshot.backup -Child "install-state.json"
    if ($null -ne $Snapshot.previousState) {
        if (-not (Test-Path -LiteralPath $StatePath) -or
            -not (Test-Path -LiteralPath $UninstallKey)) {
            throw "Installer rollback verification did not restore install state/registration"
        }
        Assert-OwnedLeafNotReparsePoint -Path $backupState -Label "HydraSeat rollback verification install-state backup"
        $restoredState = Read-InstallStateFile -Path $StatePath
        if ([UInt64]$restoredState.releaseRevision -ne [UInt64]$Snapshot.previousState.releaseRevision -or
            [string]$restoredState.releaseVersion -ne [string]$Snapshot.previousState.releaseVersion -or
            [string]$restoredState.commitSha -ne [string]$Snapshot.previousState.commitSha) {
            throw "Installer rollback verification restored a different install identity"
        }
        $backupStateHash = (Get-FileHash -LiteralPath $backupState -Algorithm SHA256).Hash
        $stateHash = (Get-FileHash -LiteralPath $StatePath -Algorithm SHA256).Hash
        if (-not $backupStateHash.Equals($stateHash, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Installer rollback verification restored different install-state bytes"
        }
    } else {
        if (Test-Path -LiteralPath $StatePath -PathType Leaf) {
            throw "Installer rollback verification unexpectedly retained install state"
        }
        if (Test-Path -LiteralPath $UninstallKey) {
            throw "Installer rollback verification unexpectedly retained uninstall registration"
        }
    }
}

function Get-InstallerTransactionDirectories {
    if (-not (Test-Path -LiteralPath $TransactionRoot)) { return @() }
    if (-not (Test-Path -LiteralPath $TransactionRoot -PathType Container)) {
        throw "HydraSeat installer transaction root is not a directory"
    }
    Assert-OwnedDirectoryNotReparsePoint -Path $TransactionRoot -Label "HydraSeat installer transaction root"
    $entries = @(Get-ChildItem -LiteralPath $TransactionRoot -Force)
    if ($entries.Count -gt $MaximumInstallerTransactions) {
        throw "Too many installer transaction entries; refusing ambiguous recovery"
    }
    $directories = @()
    foreach ($entry in $entries) {
        if (-not $entry.PSIsContainer -or $entry.Name -notmatch "^[0-9a-f]{32}$" -or
            ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Installer transaction root contains an unknown or unsafe entry"
        }
        $directories += $entry
    }
    return @($directories | Sort-Object -Property Name)
}

function Assert-NoPendingInstallerTransactions {
    if (@(Get-InstallerTransactionDirectories).Count -ne 0) {
        throw "An interrupted installer transaction requires an elevated Install/Repair/Uninstall recovery run before read-only validation"
    }
}

function Recover-InterruptedTransactions {
    foreach ($entry in @(Get-InstallerTransactionDirectories)) {
        $root = [string]$entry.FullName
        $state = Read-TransactionState -Root $root
        if ([string]$state.phase -eq "snapshotting") {
            Remove-Item -LiteralPath $root -Recurse -Force
            if (Test-Path -LiteralPath $root) {
                throw "Installer could not remove incomplete pre-mutation transaction state"
            }
            continue
        }

        $backup = Resolve-UnderRoot -Root $root -Child "backup"
        $stage = Resolve-UnderRoot -Root $root -Child "stage"
        Assert-OwnedDirectoryNotReparsePoint -Path $backup -Label "HydraSeat installer transaction backup"
        Assert-OwnedDirectoryNotReparsePoint -Path $stage -Label "HydraSeat installer transaction stage"
        if (-not (Test-Path -LiteralPath $backup -PathType Container) -or
            -not (Test-Path -LiteralPath $stage -PathType Container)) {
            throw "Prepared installer transaction is missing its bounded backup/stage directories"
        }

        $backupStatePath = Resolve-UnderRoot -Root $backup -Child "install-state.json"
        $previousState = $null
        if ([bool]$state.previousStatePresent) {
            $previousState = Read-InstallStateFile -Path $backupStatePath
        } elseif (Test-Path -LiteralPath $backupStatePath) {
            throw "Prepared installer transaction contradicts previous-state ownership metadata"
        }

        $snapshot = [ordered]@{
            id = [string]$state.transactionId
            root = $root
            backup = $backup
            stage = $stage
            previousState = $previousState
            operation = [string]$state.operation
            snapshotIdentity = [string]$state.snapshotIdentity
        }
        $actualSnapshotIdentity = Get-TransactionSnapshotIdentity -Backup $backup `
            -PreviousStatePresent ([bool]$state.previousStatePresent)
        if ($actualSnapshotIdentity -ne [string]$state.snapshotIdentity) {
            throw "Interrupted installer transaction backup changed after its prepared snapshot was journaled"
        }
        if ([string]$state.phase -in @("prepared", "committed")) {
            // "prepared" means Program Files mutation never began. "committed"
            // means it already completed. In both cases the verified recovery
            // snapshot can simply be discarded.
            Remove-Item -LiteralPath $root -Recurse -Force
            if (Test-Path -LiteralPath $root) {
                throw "Installer could not remove non-mutating/completed transaction recovery state"
            }
            continue
        }

        if ($null -ne $previousState) {
            Set-ActiveOwnedFiles -Paths @(
                $previousState.ownedFiles | ForEach-Object { [string]$_.fileName }
            )
        } else {
            $stagedFiles = @(
                Get-SafeRelativeFileInventory -Root $stage -MaximumFiles $MaximumOwnedFiles
            )
            Set-ActiveOwnedFiles -Paths $stagedFiles
        }

        Restore-TransactionSnapshot -Snapshot $snapshot
        Verify-RestoredTransactionSnapshot -Snapshot $snapshot
        Remove-Item -LiteralPath $root -Recurse -Force
        if (Test-Path -LiteralPath $root) {
            throw "Installer rollback was verified but interrupted transaction cleanup failed"
        }
    }
}

function Enter-InstallerMutationLock {
    $mutex = New-Object System.Threading.Mutex -ArgumentList @($false, "Global\HydraSeatInstallerMutationV1")
    $acquired = $false
    try {
        try {
            $acquired = $mutex.WaitOne(0)
        } catch [System.Threading.AbandonedMutexException] {
            $acquired = $true
        }
        if (-not $acquired) {
            $mutex.Dispose()
            throw "Another HydraSeat installer mutation is already active"
        }
        return $mutex
    } catch {
        if (-not $acquired) { $mutex.Dispose() }
        throw
    }
}

function Verify-StagedPackage {
    param($Package, [string]$StageRoot, [string]$ExpectedSigner)
    Assert-OwnedDirectoryNotReparsePoint -Path $StageRoot -Label "HydraSeat installer staging root"
    $expected = @{}
    foreach ($file in @($Package.files)) {
        $fileName = [string]$file.fileName
        $folded = $fileName.ToUpperInvariant()
        if ($expected.ContainsKey($folded)) {
            throw "Staged release package contains duplicate owned file metadata"
        }
        $expected[$folded] = $true
        $staged = Resolve-UnderRoot -Root $StageRoot -Child $fileName
        if (-not (Test-Path -LiteralPath $staged -PathType Leaf)) {
            throw "Staged release file is missing: $fileName"
        }
        $item = Get-Item -LiteralPath $staged -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Staged release file must not be a reparse point: $fileName"
        }
        $hash = (Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne [string]$file.sha256) {
            throw "Staged release file hash verification failed: $fileName"
        }
        if ([bool]$file.productSigned) {
            $signature = Get-AuthenticodeSignature -LiteralPath $staged
            if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
                $null -eq $signature.SignerCertificate -or
                $signature.SignerCertificate.Thumbprint.ToUpperInvariant() -ne $ExpectedSigner) {
                throw "Staged HydraSeat core file Authenticode verification failed: $fileName"
            }
        }
    }
    $entries = @(
        Get-SafeRelativeFileInventory -Root $StageRoot -MaximumFiles $MaximumOwnedFiles
    )
    if ($entries.Count -ne $expected.Count) {
        throw "Installer staging root does not contain the exact verified owned file set"
    }
    foreach ($entry in $entries) {
        if (-not $expected.ContainsKey(([string]$entry).ToUpperInvariant())) {
            throw "Installer staging root contains an unexpected file: $entry"
        }
    }
}

function Verify-InstalledPackage {
    param($Package, [string]$ExpectedSigner)
    foreach ($file in @($Package.files)) {
        $destination = Resolve-UnderRoot -Root $InstallRoot -Child ([string]$file.fileName)
        if (-not (Test-Path -LiteralPath $destination)) {
            throw "Installed file missing after commit"
        }
        Assert-OwnedLeafNotReparsePoint -Path $destination -Label "Installed HydraSeat release file $($file.fileName)"
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne [string]$file.sha256) {
            throw "Installed file hash verification failed"
        }
        if ([bool]$file.productSigned) {
            $signature = Get-AuthenticodeSignature -LiteralPath $destination
            if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
                $null -eq $signature.SignerCertificate -or
                $signature.SignerCertificate.Thumbprint.ToUpperInvariant() -ne $ExpectedSigner) {
                throw "Installed HydraSeat core file Authenticode verification failed"
            }
        }
    }
    if (-not (Test-Path -LiteralPath $StatePath -PathType Leaf) -or -not (Test-Path -LiteralPath $UninstallKey)) {
        throw "Install state or uninstall registration missing after commit"
    }
    $installedState = Read-InstallStateFile -Path $StatePath
    if ([string]$installedState.releaseVersion -ne [string]$Package.releaseVersion -or
        [UInt64]$installedState.releaseRevision -ne [UInt64]$Package.releaseRevision -or
        -not ([string]$installedState.commitSha).Equals([string]$Package.commitSha, [StringComparison]::OrdinalIgnoreCase) -or
        [string]$installedState.architecture -ne [string]$Package.architecture) {
        throw "Installed state identity does not match the verified release package"
    }
    $registration = Get-ItemProperty -LiteralPath $UninstallKey
    $installedScript = Resolve-UnderRoot -Root $InstallRoot -Child "install_hydraseat.ps1"
    $expectedUninstallCommand = 'powershell.exe -NoProfile -ExecutionPolicy AllSigned -File "' + $installedScript + '" -Mode Uninstall'
    if ([string]$registration.DisplayName -ne $ProductName -or
        [string]$registration.DisplayVersion -ne [string]$Package.releaseVersion -or
        [string]$registration.Publisher -ne "HydraSeat" -or
        -not ([string]$registration.InstallLocation).Equals($InstallRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [string]$registration.UninstallString -ne $expectedUninstallCommand -or
        [int]$registration.NoModify -ne 1 -or [int]$registration.NoRepair -ne 0) {
        throw "Uninstall registration does not match the verified installed release"
    }
}

function Verify-UninstalledOwnedState {
    foreach ($fileName in $OwnedFiles) {
        $installed = Resolve-UnderRoot -Root $InstallRoot -Child $fileName
        if (Test-Path -LiteralPath $installed) {
            throw "Uninstall verification found a remaining HydraSeat-owned file: $fileName"
        }
    }
    if (Test-Path -LiteralPath $StatePath -PathType Leaf) {
        throw "Uninstall verification found remaining HydraSeat install state"
    }
    if (Test-Path -LiteralPath $UninstallKey) {
        throw "Uninstall verification found remaining uninstall registration"
    }
}

function Remove-EmptyMachineDataRoots {
    if (Test-Path -LiteralPath $TransactionRoot) {
        Assert-OwnedDirectoryNotReparsePoint -Path $TransactionRoot -Label "HydraSeat installer transaction root"
        $transactions = @(Get-ChildItem -LiteralPath $TransactionRoot -Force)
        if ($transactions.Count -eq 0) {
            Remove-Item -LiteralPath $TransactionRoot -Force
        }
    }
    if (Test-Path -LiteralPath $DataRoot) {
        Assert-OwnedDirectoryNotReparsePoint -Path $DataRoot -Label "HydraSeat machine-data root"
        $remaining = @(Get-ChildItem -LiteralPath $DataRoot -Force)
        if ($remaining.Count -eq 0) {
            Remove-Item -LiteralPath $DataRoot -Force
        }
    }
}

Assert-OwnedDirectoryNotReparsePoint -Path $InstallRoot -Label "HydraSeat install root"
Assert-OwnedDirectoryNotReparsePoint -Path $DataRoot -Label "HydraSeat machine-data root"

if ($Mode -eq "Validate") {
    Assert-NoPendingInstallerTransactions
    $OwnSigner = Get-OwnSignerThumbprint
    Assert-OrdinaryWindowsState
    $package = Get-ValidatedPackage -Root $PackageRoot -ExpectedSigner $OwnSigner
    Write-Host "HydraSeat release package valid: $($package.releaseVersion) rev $($package.releaseRevision) $Architecture"
    exit 0
}

Assert-Administrator
$MutationLock = Enter-InstallerMutationLock
$OwnSigner = Get-OwnSignerThumbprint
Assert-OrdinaryWindowsState
Recover-InterruptedTransactions

if ($Mode -eq "Uninstall") {
    $previous = Read-InstallState
    if ($null -eq $previous) {
        throw "HydraSeat install state not found; refusing broad cleanup"
    }
    Set-ActiveOwnedFiles -Paths @(
        $previous.ownedFiles | ForEach-Object { [string]$_.fileName }
    )
    $snapshot = New-TransactionSnapshot -PreviousState $previous -Operation "Uninstall"
    $cleanupSnapshot = $false
    try {
        Write-TransactionState -Root $snapshot.root -TransactionId $snapshot.id -Phase "staged" `
            -Operation $snapshot.operation -PreviousStatePresent $true `
            -SnapshotIdentity $snapshot.snapshotIdentity
        Remove-UninstallRegistration
        Assert-OwnedDirectoryNotReparsePoint -Path $InstallRoot -Label "HydraSeat install root"
        foreach ($fileName in $OwnedFiles) {
            $installed = Resolve-UnderRoot -Root $InstallRoot -Child $fileName
            if (Test-Path -LiteralPath $installed) {
                Assert-OwnedLeafNotReparsePoint -Path $installed -Label "HydraSeat uninstall owned file $fileName"
                Remove-Item -LiteralPath $installed -Force
                Remove-EmptyOwnedParentDirectories -Root $InstallRoot -Child $fileName
            }
        }
        if (Test-Path -LiteralPath $StatePath) {
            Assert-OwnedLeafNotReparsePoint -Path $StatePath -Label "HydraSeat uninstall install state"
            Remove-Item -LiteralPath $StatePath -Force
        }
        if (Test-Path -LiteralPath $InstallRoot) {
            Assert-OwnedDirectoryNotReparsePoint -Path $InstallRoot -Label "HydraSeat install root"
            $remaining = @(Get-ChildItem -LiteralPath $InstallRoot -Force)
            if ($remaining.Count -eq 0) {
                Remove-Item -LiteralPath $InstallRoot -Force
            } else {
                Write-Warning "Unknown files remain in HydraSeat install root; they were not deleted."
            }
        }
        Verify-UninstalledOwnedState
        Write-TransactionState -Root $snapshot.root -TransactionId $snapshot.id -Phase "committed" `
            -Operation $snapshot.operation -PreviousStatePresent $true `
            -SnapshotIdentity $snapshot.snapshotIdentity
        $cleanupSnapshot = $true
    } catch {
        $originalFailure = $_
        try {
            Restore-TransactionSnapshot -Snapshot $snapshot
            Verify-RestoredTransactionSnapshot -Snapshot $snapshot
            $cleanupSnapshot = $true
        } catch {
            throw "HydraSeat uninstall failed and rollback could not be verified; recovery snapshot retained at $($snapshot.root). Original: $($originalFailure.Exception.Message). Rollback: $($_.Exception.Message)"
        }
        throw $originalFailure
    } finally {
        if ($cleanupSnapshot -and (Test-Path -LiteralPath $snapshot.root)) {
            Remove-Item -LiteralPath $snapshot.root -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $snapshot.root) {
                throw "Installer transaction completed but its recovery snapshot could not be removed"
            }
        }
    }

    Remove-EmptyMachineDataRoots

    $userDataRetained = $true
    if ($RemoveHydraSeatUserData) {
        try {
            Assert-OwnedDirectoryNotReparsePoint -Path $UserDataRoot -Label "HydraSeat per-user data root"
            if (Test-Path -LiteralPath $UserDataRoot) {
                Remove-Item -LiteralPath $UserDataRoot -Recurse -Force
            }
            $userDataRetained = $false
        } catch {
            Write-Warning "HydraSeat was uninstalled, but per-user data was retained because safe removal failed: $($_.Exception.Message)"
        }
    }
    Write-Host "HydraSeat uninstall completed. User data retained: $userDataRetained"
    exit 0
}

$package = Get-ValidatedPackage -Root $PackageRoot -ExpectedSigner $OwnSigner
Set-ActiveOwnedFiles -Paths @(
    $package.files | ForEach-Object { [string]$_.fileName }
)
$previous = Read-InstallState
if ($Mode -eq "Install" -and $null -ne $previous) {
    throw "HydraSeat is already installed; use Repair or the approved update flow"
}
if ($Mode -eq "Repair" -and $null -eq $previous) {
    throw "Repair requires an existing HydraSeat install state"
}
if ($Mode -eq "Repair" -and
    ([UInt64]$package.releaseRevision -ne [UInt64]$previous.releaseRevision -or
     [string]$package.releaseVersion -ne [string]$previous.releaseVersion -or
     -not ([string]$package.commitSha).Equals([string]$previous.commitSha, [StringComparison]::OrdinalIgnoreCase))) {
    throw "Repair requires the exact installed release identity; use the approved update/rollback flow for a different release"
}
if ($Mode -eq "Repair") {
    $previousPaths = @(
        $previous.ownedFiles |
            ForEach-Object { ([string]$_.fileName).ToUpperInvariant() } |
            Sort-Object
    )
    $packagePaths = @(
        $OwnedFiles |
            ForEach-Object { ([string]$_).ToUpperInvariant() } |
            Sort-Object
    )
    if ($previousPaths.Count -ne $packagePaths.Count -or
        @(Compare-Object -ReferenceObject $previousPaths -DifferenceObject $packagePaths).Count -ne 0) {
        throw "Repair package file ownership differs from the installed exact release"
    }
}

$snapshot = New-TransactionSnapshot -PreviousState $previous -Operation $Mode
$cleanupSnapshot = $false
try {
    # The Microsoft runtime is a prerequisite, not HydraSeat-owned state. It is
    # verified and installed before Program Files mutation and is intentionally
    # not removed on HydraSeat uninstall.
    Ensure-VcRuntimePrerequisite -Package $package -StageRoot $snapshot.stage

    foreach ($file in @($package.files)) {
        $relativePath = [string]$file.fileName
        Ensure-OwnedParentDirectories -Root $snapshot.stage -Child $relativePath
        $stagedDestination =
            Resolve-UnderRoot -Root $snapshot.stage -Child $relativePath
        Copy-Item -LiteralPath ([string]$file.sourcePath) -Destination $stagedDestination -Force
    }
    Verify-StagedPackage -Package $package -StageRoot $snapshot.stage -ExpectedSigner $OwnSigner
    Write-TransactionState -Root $snapshot.root -TransactionId $snapshot.id -Phase "staged" `
        -Operation $snapshot.operation -PreviousStatePresent ($null -ne $snapshot.previousState) `
        -SnapshotIdentity $snapshot.snapshotIdentity
    New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
    Assert-OwnedDirectoryNotReparsePoint -Path $InstallRoot -Label "HydraSeat install root"
    foreach ($file in @($package.files)) {
        $source = Resolve-UnderRoot -Root $snapshot.stage -Child ([string]$file.fileName)
        Assert-OwnedLeafNotReparsePoint -Path $source -Label "HydraSeat staged release file $($file.fileName)"
        $relativePath = [string]$file.fileName
        Ensure-OwnedParentDirectories -Root $InstallRoot -Child $relativePath
        $destination = Resolve-UnderRoot -Root $InstallRoot -Child $relativePath
        if (Test-Path -LiteralPath $destination) {
            Assert-OwnedLeafNotReparsePoint -Path $destination -Label "HydraSeat install destination $($file.fileName)"
        }
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
    Write-InstallState -Package $package
    Register-Uninstall -Version $package.releaseVersion
    Verify-InstalledPackage -Package $package -ExpectedSigner $OwnSigner
    Write-TransactionState -Root $snapshot.root -TransactionId $snapshot.id -Phase "committed" `
        -Operation $snapshot.operation -PreviousStatePresent ($null -ne $snapshot.previousState) `
        -SnapshotIdentity $snapshot.snapshotIdentity
    $cleanupSnapshot = $true
    Write-Host "HydraSeat $Mode completed: $($package.releaseVersion) rev $($package.releaseRevision)"
    if ($Mode -eq "Install") {
        Write-Host "Startup mode remains Manual. Launch HydraSeat to configure optional Seats; Set later is supported."
    }
} catch {
    $originalFailure = $_
    try {
        Restore-TransactionSnapshot -Snapshot $snapshot
        Verify-RestoredTransactionSnapshot -Snapshot $snapshot
        $cleanupSnapshot = $true
    } catch {
        throw "HydraSeat $Mode failed and rollback could not be verified; recovery snapshot retained at $($snapshot.root). Original: $($originalFailure.Exception.Message). Rollback: $($_.Exception.Message)"
    }
    throw $originalFailure
} finally {
    if ($cleanupSnapshot -and (Test-Path -LiteralPath $snapshot.root)) {
        Remove-Item -LiteralPath $snapshot.root -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $snapshot.root) {
            throw "Installer transaction completed but its recovery snapshot could not be removed"
        }
    }
}

if ($Mode -eq "Install" -and $LaunchAfterInstall) {
    Start-Process -FilePath (Join-Path $InstallRoot "HydraSeat.exe")
}
