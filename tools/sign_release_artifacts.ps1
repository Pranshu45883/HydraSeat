[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ManifestPath,
    [Parameter(Mandatory = $true)][string]$BuildX64,
    [string]$BuildX86 = "",
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$CertificateThumbprint,
    [Parameter(Mandatory = $true)][string]$TimestampUrl,
    [Parameter(Mandatory = $true)][string]$ReleaseVersion,
    [Parameter(Mandatory = $true)][UInt64]$ReleaseRevision,
    [string]$Configuration = "Release",
    [string]$CommitSha = "unknown"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-UnderRoot {
    param([string]$Root, [string]$Child)
    $rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $candidate = [System.IO.Path]::GetFullPath((Join-Path $Root $Child))
    if (-not $candidate.StartsWith($rootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Resolved path escapes staging/build root"
    }
    return $candidate
}

function Get-SignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($null -eq $command) { throw "signtool.exe not found on PATH" }
    return $command.Source
}

function Get-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($null -eq $command) { throw "cmake.exe not found on PATH; reviewed release targets cannot be rebuilt" }
    return $command.Source
}

function Assert-SafeBasename {
    param([string]$FileName, [string]$ExtensionPattern)
    if ([string]::IsNullOrWhiteSpace($FileName) -or
        $FileName -notmatch $ExtensionPattern -or
        $FileName.Contains('..') -or $FileName.Contains('\') -or
        $FileName.Contains('/') -or $FileName.Contains('*') -or
        $FileName.Contains('?')) {
        throw "Signing manifest contains unsafe fileName"
    }
}

function Assert-ReviewedSigningManifest {
    param($Manifest)

    $expected = @{
        "main-ui" = @{ kind = "cmake-executable"; target = "HydraSeat"; fileName = "HydraSeat.exe" }
        "host" = @{ kind = "cmake-executable"; target = "hydra_host"; fileName = "hydra_host.exe" }
        "seat-ui" = @{ kind = "cmake-executable"; target = "hydra_seat_ui"; fileName = "hydra_seat_ui.exe" }
        "watchdog" = @{ kind = "cmake-executable"; target = "hydra_watchdog"; fileName = "hydra_watchdog.exe" }
        "reset" = @{ kind = "cmake-executable"; target = "hydra_reset"; fileName = "hydra_reset.exe" }
        "profile-cli" = @{ kind = "cmake-executable"; target = "hydraseat_profilectl"; fileName = "hydraseat_profilectl.exe" }
        "community-validator" = @{ kind = "cmake-executable"; target = "hydraseat_community_validate"; fileName = "hydraseat_community_validate.exe" }
        "installer-script" = @{ kind = "powershell-script"; sourcePath = "tools/install_hydraseat.ps1"; fileName = "install_hydraseat.ps1" }
    }

    $artifacts = @($Manifest.artifacts)
    if ($artifacts.Count -ne $expected.Count) {
        throw "Signing manifest does not contain the exact reviewed release artifact set"
    }

    $seen = @{}
    foreach ($artifact in $artifacts) {
        $id = [string]$artifact.id
        if (-not $expected.ContainsKey($id) -or $seen.ContainsKey($id)) {
            throw "Signing manifest contains an unreviewed or duplicate artifact id"
        }
        $reviewed = $expected[$id]
        if ([string]$artifact.kind -ne [string]$reviewed.kind -or
            [string]$artifact.fileName -ne [string]$reviewed.fileName) {
            throw "Signing manifest artifact kind/fileName differs from the reviewed allowlist"
        }
        if ([string]$artifact.kind -eq "cmake-executable") {
            if ([string]$artifact.target -ne [string]$reviewed.target) {
                throw "Signing manifest CMake target differs from the reviewed allowlist"
            }
        } else {
            if ([string]$artifact.sourcePath -ne [string]$reviewed.sourcePath) {
                throw "Signing manifest script source differs from the reviewed allowlist"
            }
        }
        $seen[$id] = $true
    }
}

function Assert-ReviewedBuildRoot {
    param([string]$BuildRoot, [string]$RepositoryRoot)
    if (-not (Test-Path -LiteralPath $BuildRoot -PathType Container)) {
        throw "Release build root does not exist"
    }
    $cachePath = Resolve-UnderRoot -Root $BuildRoot -Child "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        throw "Release build root is missing CMakeCache.txt"
    }
    $cacheFile = Get-Item -LiteralPath $cachePath -Force
    if ($cacheFile.Length -le 0 -or $cacheFile.Length -gt 4194304) {
        throw "Release CMake cache size is invalid"
    }
    $prefix = "CMAKE_HOME_DIRECTORY:INTERNAL="
    $homeLines = @(Get-Content -LiteralPath $cachePath -Encoding UTF8 | Where-Object {
        $_.StartsWith($prefix, [System.StringComparison]::Ordinal)
    })
    if ($homeLines.Count -ne 1) {
        throw "Release build cache must identify exactly one CMAKE_HOME_DIRECTORY"
    }
    $configuredSourceText = $homeLines[0].Substring($prefix.Length).Trim()
    if ([string]::IsNullOrWhiteSpace($configuredSourceText)) {
        throw "Release build cache contains an empty CMAKE_HOME_DIRECTORY"
    }
    $configuredSource = [System.IO.Path]::GetFullPath($configuredSourceText)
    $expectedSource = [System.IO.Path]::GetFullPath($RepositoryRoot)
    if (-not $configuredSource.Equals($expectedSource, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Release build root was not configured from the reviewed repository checkout"
    }
}

function Invoke-ReviewedTargetBuild {
    param([string]$CMakePath, [string]$BuildRoot, [string]$Configuration, [string[]]$TargetNames)
    if ($null -eq $TargetNames -or $TargetNames.Count -lt 1 -or $TargetNames.Count -gt 16) {
        throw "Reviewed release target set is empty or unbounded"
    }
    foreach ($targetName in $TargetNames) {
        if ($targetName -notmatch '^[A-Za-z0-9_]{1,96}$') {
            throw "Reviewed release target set contains an invalid target name"
        }
    }
    $buildArguments = @("--build", $BuildRoot, "--config", $Configuration, "--clean-first", "--target") + @($TargetNames)
    & $CMakePath @buildArguments | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to clean and rebuild the reviewed release target set"
    }
}

function Assert-X64PortableExecutable {
    param([string]$Path)
    $stream = $null
    $reader = $null
    try {
        $stream = [System.IO.File]::Open(
            $Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::Read)
        if ($stream.Length -lt 70) {
            throw "Release executable is too small to contain a valid PE header"
        }
        $reader = New-Object System.IO.BinaryReader -ArgumentList $stream
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "Release executable is missing the MZ header"
        }
        [void]$stream.Seek(0x3c, [System.IO.SeekOrigin]::Begin)
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0x40 -or ([Int64]$peOffset + 6) -gt $stream.Length) {
            throw "Release executable contains an invalid PE header offset"
        }
        [void]$stream.Seek($peOffset, [System.IO.SeekOrigin]::Begin)
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Release executable is missing the PE signature"
        }
        if ($reader.ReadUInt16() -ne 0x8664) {
            throw "Release executable is not an AMD64/x64 PE image"
        }
    } finally {
        if ($null -ne $reader) {
            $reader.Dispose()
        } elseif ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

function Write-DetachedCmsSignature {
    param([string]$ContentPath, [string]$SignaturePath, $Certificate)
    Add-Type -AssemblyName System.Security
    $contentBytes = [System.IO.File]::ReadAllBytes($ContentPath)
    $contentInfo = New-Object System.Security.Cryptography.Pkcs.ContentInfo -ArgumentList (,$contentBytes)
    $signedCms = New-Object System.Security.Cryptography.Pkcs.SignedCms -ArgumentList @($contentInfo, $true)
    $cmsSigner = New-Object System.Security.Cryptography.Pkcs.CmsSigner -ArgumentList $Certificate
    $cmsSigner.IncludeOption = [System.Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
    $signedCms.ComputeSignature($cmsSigner)
    [System.IO.File]::WriteAllBytes($SignaturePath, $signedCms.Encode())
}

function Assert-DetachedCmsSignature {
    param([string]$ContentPath, [string]$SignaturePath, [string]$ExpectedThumbprint)
    Add-Type -AssemblyName System.Security
    $contentBytes = [System.IO.File]::ReadAllBytes($ContentPath)
    $contentInfo = New-Object System.Security.Cryptography.Pkcs.ContentInfo -ArgumentList (,$contentBytes)
    $signedCms = New-Object System.Security.Cryptography.Pkcs.SignedCms -ArgumentList @($contentInfo, $true)
    $signedCms.Decode([System.IO.File]::ReadAllBytes($SignaturePath))
    $signedCms.CheckSignature($true)
    if ($signedCms.SignerInfos.Count -ne 1 -or
        $null -eq $signedCms.SignerInfos[0].Certificate -or
        $signedCms.SignerInfos[0].Certificate.Thumbprint.ToUpperInvariant() -ne $ExpectedThumbprint) {
        throw "Detached provenance signature does not match the selected signing identity"
    }
}

if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) { throw "Signing manifest not found" }
$ManifestPath = [System.IO.Path]::GetFullPath($ManifestPath)
$manifestDirectory = Split-Path -Parent $ManifestPath
$repositoryRoot = Split-Path -Parent $manifestDirectory
$expectedManifestPath = [System.IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "config\release-signing-manifest.json"))
if (-not $ManifestPath.Equals($expectedManifestPath, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Signing manifest must be the reviewed repository config/release-signing-manifest.json"
}
if ([string]::IsNullOrWhiteSpace($CertificateThumbprint) -or
    $CertificateThumbprint -notmatch '^[A-Fa-f0-9]{40}$') {
    throw "CertificateThumbprint must be a 40-hex SHA-1 certificate thumbprint"
}
if ($TimestampUrl -notmatch '^https://') { throw "TimestampUrl must use HTTPS" }
if ($ReleaseVersion -notmatch '^[A-Za-z0-9._+-]{1,64}$') { throw "ReleaseVersion is invalid" }
if ($ReleaseRevision -eq 0) { throw "ReleaseRevision must be nonzero" }
if ($Configuration -ne "Release") { throw "Release signing requires CMake configuration Release" }
if ($CommitSha -notmatch '^[A-Fa-f0-9]{40}$') {
    throw "CommitSha must be the exact 40-hex reviewed Git commit"
}

$git = Get-Command git.exe -ErrorAction SilentlyContinue
if ($null -eq $git) { throw "git.exe not found on PATH; exact source commit cannot be verified" }
$actualCommit = (& $git.Source -C $repositoryRoot rev-parse HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -notmatch '^[A-Fa-f0-9]{40}$') {
    throw "Unable to resolve the exact repository HEAD commit"
}
if (-not $actualCommit.Equals($CommitSha, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "CommitSha does not match the checked-out repository HEAD"
}
$dirtyState = (& $git.Source -C $repositoryRoot status --porcelain | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw "Unable to verify repository cleanliness before signing" }
if (-not [string]::IsNullOrWhiteSpace($dirtyState)) {
    throw "Release signing requires a clean exact-commit repository checkout"
}
$CommitSha = $actualCommit.ToLowerInvariant()

$manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1) { throw "Unsupported signing manifest version" }
if ($manifest.artifacts.Count -lt 1 -or $manifest.artifacts.Count -gt 32) {
    throw "Invalid signing artifact count"
}
Assert-ReviewedSigningManifest -Manifest $manifest

$BuildX64 = [System.IO.Path]::GetFullPath($BuildX64)
Assert-ReviewedBuildRoot -BuildRoot $BuildX64 -RepositoryRoot $repositoryRoot
$cmake = Get-CMake
$reviewedTargets = @($manifest.artifacts | Where-Object {
    [string]$_.kind -eq "cmake-executable"
} | ForEach-Object {
    [string]$_.target
})
Invoke-ReviewedTargetBuild -CMakePath $cmake -BuildRoot $BuildX64 `
    -Configuration $Configuration -TargetNames $reviewedTargets
$dirtyStateAfterBuild = (& $git.Source -C $repositoryRoot status --porcelain | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw "Unable to verify repository cleanliness after release rebuild" }
if (-not [string]::IsNullOrWhiteSpace($dirtyStateAfterBuild)) {
    throw "Release rebuild changed the reviewed source checkout; refusing to sign"
}

$signTool = Get-SignTool
$normalizedThumbprint = $CertificateThumbprint.ToUpperInvariant()
$certificate = Get-Item -LiteralPath ("Cert:\CurrentUser\My\" + $normalizedThumbprint) -ErrorAction Stop
if (-not $certificate.HasPrivateKey) {
    throw "Signing certificate does not expose a private key in CurrentUser\\My"
}

$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$records = @()

foreach ($artifact in $manifest.artifacts) {
    $kind = [string]$artifact.kind
    $fileName = [string]$artifact.fileName
    $sourceKind = ""
    $targetName = ""

    if ($kind -eq "cmake-executable") {
        Assert-SafeBasename -FileName $fileName -ExtensionPattern '^[A-Za-z0-9._-]+\.exe$'
        if (-not ($artifact.PSObject.Properties.Name -contains "target")) {
            throw "CMake signing artifact is missing target"
        }
        $targetName = [string]$artifact.target
        if ($targetName -notmatch '^[A-Za-z0-9_]{1,96}$') {
            throw "CMake signing target is invalid"
        }
        $sourceKind = "build"
    } elseif ($kind -eq "powershell-script") {
        Assert-SafeBasename -FileName $fileName -ExtensionPattern '^[A-Za-z0-9._-]+\.ps1$'
        if ([string]$artifact.id -ne "installer-script" -or
            [string]$artifact.sourcePath -ne "tools/install_hydraseat.ps1" -or
            $fileName -ne "install_hydraseat.ps1") {
            throw "Only the reviewed HydraSeat installer script may enter the release signing set"
        }
        $sourceKind = "repository"
    } else {
        throw "Signing manifest contains an unsupported artifact kind"
    }

    $architectures = @($artifact.architectures)
    if ($architectures.Count -ne 1 -or $architectures[0] -ne "x64") {
        throw "Release artifact must explicitly target the reviewed x64 host architecture exactly once"
    }

    foreach ($architecture in $architectures) {
        if ($architecture -ne "x64") {
            throw "Unsupported signing architecture"
        }

        if ($sourceKind -eq "build") {
            $source = Resolve-UnderRoot -Root $BuildX64 -Child (Join-Path $Configuration $fileName)
        } else {
            $source = Resolve-UnderRoot -Root $repositoryRoot -Child ([string]$artifact.sourcePath)
        }
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Missing release artifact: $($artifact.id) $architecture"
        }
        if ($kind -eq "cmake-executable") {
            Assert-X64PortableExecutable -Path $source
            $sourceSignature = Get-AuthenticodeSignature -LiteralPath $source
            if ($sourceSignature.Status -ne [System.Management.Automation.SignatureStatus]::NotSigned) {
                throw "Reviewed CMake release output must be unsigned before release signing"
            }
        }

        $archOutput = Resolve-UnderRoot -Root $OutputDirectory -Child $architecture
        New-Item -ItemType Directory -Force -Path $archOutput | Out-Null
        $destination = Resolve-UnderRoot -Root $OutputDirectory -Child (Join-Path $architecture $fileName)
        Copy-Item -LiteralPath $source -Destination $destination -Force

        $unsignedHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($kind -eq "cmake-executable") {
            & $signTool sign /fd SHA256 /sha1 $normalizedThumbprint /tr $TimestampUrl /td SHA256 $destination | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "signtool sign failed for $($artifact.id) $architecture"
            }
            & $signTool verify /pa /all $destination | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "signtool verify failed for $($artifact.id) $architecture"
            }
        } else {
            $scriptSignature = Set-AuthenticodeSignature -LiteralPath $destination `
                -Certificate $certificate -HashAlgorithm SHA256 -TimestampServer $TimestampUrl
            if ($null -eq $scriptSignature -or
                $scriptSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
                throw "PowerShell Authenticode signing failed for $($artifact.id) $architecture"
            }
        }

        $signature = Get-AuthenticodeSignature -LiteralPath $destination
        if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
            throw "Authenticode verification was not Valid for $($artifact.id) $architecture"
        }
        if ($null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.Thumbprint.ToUpperInvariant() -ne $normalizedThumbprint) {
            throw "Signed artifact publisher certificate does not match the selected signing identity"
        }

        $signedHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        $records += [ordered]@{
            id = [string]$artifact.id
            kind = $kind
            target = $targetName
            architecture = [string]$architecture
            fileName = $fileName
            unsignedSha256 = $unsignedHash
            signedSha256 = $signedHash
            signerThumbprint = $normalizedThumbprint
            signatureStatus = [string]$signature.Status
        }
    }
}

$provenance = [ordered]@{
    schemaVersion = 1
    releaseVersion = $ReleaseVersion
    releaseRevision = $ReleaseRevision
    commitSha = $CommitSha
    signingManifest = [System.IO.Path]::GetFileName($ManifestPath)
    timestampUrl = $TimestampUrl
    artifacts = $records
}
$provenancePath = Join-Path $OutputDirectory "signing-provenance.json"
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $provenancePath -Encoding UTF8
$provenanceSignaturePath = $provenancePath + ".p7s"
Write-DetachedCmsSignature -ContentPath $provenancePath -SignaturePath $provenanceSignaturePath -Certificate $certificate
Assert-DetachedCmsSignature -ContentPath $provenancePath -SignaturePath $provenanceSignaturePath -ExpectedThumbprint $normalizedThumbprint
Write-Host "Signed and verified $($records.Count) fixed release artifacts."
Write-Host "Provenance: $provenancePath"
Write-Host "Provenance signature: $provenanceSignaturePath"
