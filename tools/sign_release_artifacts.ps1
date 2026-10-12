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

function Get-ReviewedWinDeployQt {
    param([string]$BuildRoot)

    $cachePath = Resolve-UnderRoot -Root $BuildRoot -Child "CMakeCache.txt"
    $qtPrefix = "Qt6_DIR:PATH="
    $qtLines = @(Get-Content -LiteralPath $cachePath -Encoding UTF8 | Where-Object {
        $_.StartsWith($qtPrefix, [System.StringComparison]::Ordinal)
    })
    if ($qtLines.Count -ne 1) {
        throw "Release CMake cache must identify exactly one Qt6_DIR"
    }

    $qtConfigDirectory = [System.IO.Path]::GetFullPath(
        $qtLines[0].Substring($qtPrefix.Length).Trim())
    $qtCmakeDirectory = Split-Path -Parent $qtConfigDirectory
    $qtLibDirectory = Split-Path -Parent $qtCmakeDirectory
    $qtRoot = Split-Path -Parent $qtLibDirectory
    $expected = [System.IO.Path]::GetFullPath(
        (Join-Path $qtRoot "bin\windeployqt.exe"))
    if (-not (Test-Path -LiteralPath $expected -PathType Leaf)) {
        throw "The Qt installation used by CMake does not contain windeployqt.exe"
    }

    $command = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "windeployqt.exe not found on PATH; Qt 6.8.3 runtime cannot be frozen for the release package"
    }
    $actual = [System.IO.Path]::GetFullPath([string]$command.Source)
    if (-not $actual.Equals(
            $expected,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Release windeployqt must come from the exact Qt installation bound in CMakeCache.txt"
    }

    $versionText = (& $actual --version 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or
        $versionText -notmatch "(^|[^0-9])6\.8\.3([^0-9]|$)") {
        throw "Release packaging requires windeployqt from exact Qt 6.8.3"
    }
    return $actual
}

function Get-ReviewedVcRedist {
    param([string]$BuildRoot)

    $cachePath = Resolve-UnderRoot -Root $BuildRoot -Child "CMakeCache.txt"
    $compilerLines = @(
        Get-Content -LiteralPath $cachePath -Encoding UTF8 |
            Where-Object {
                $_ -match '^CMAKE_CXX_COMPILER(?::[^=]+)?=(.+)$'
            }
    )
    if ($compilerLines.Count -ne 1) {
        throw "Release CMake cache must identify exactly one C++ compiler"
    }

    $compilerText = [regex]::Match(
        [string]$compilerLines[0],
        '^CMAKE_CXX_COMPILER(?::[^=]+)?=(.+)$'
    ).Groups[1].Value.Trim()
    if ([string]::IsNullOrWhiteSpace($compilerText)) {
        throw "Release CMake cache contains an empty C++ compiler path"
    }

    $compilerPath = $compilerText
    if (-not [IO.Path]::IsPathRooted($compilerPath)) {
        $compilerCommand =
            Get-Command $compilerPath -ErrorAction SilentlyContinue
        if ($null -eq $compilerCommand) {
            throw "Release C++ compiler could not be resolved"
        }
        $compilerPath = [string]$compilerCommand.Source
    }
    $compilerPath = [IO.Path]::GetFullPath($compilerPath)

    $marker = "\VC\Tools\MSVC\"
    $markerIndex = $compilerPath.IndexOf(
        $marker,
        [StringComparison]::OrdinalIgnoreCase)
    if ($markerIndex -le 0) {
        throw "Release compiler is not from a reviewed Visual Studio MSVC toolchain"
    }

    $visualStudioRoot = $compilerPath.Substring(0, $markerIndex)
    $redistRoot = [IO.Path]::GetFullPath(
        (Join-Path $visualStudioRoot "VC\Redist\MSVC"))
    if (-not (Test-Path -LiteralPath $redistRoot -PathType Container)) {
        throw "Visual Studio MSVC redistributable root is missing"
    }

    $candidates = @(
        Get-ChildItem -LiteralPath $redistRoot -Filter "vc_redist.x64.exe" -File -Recurse -ErrorAction Stop |
            Where-Object {
                ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0
            }
    )
    if ($candidates.Count -lt 1 -or $candidates.Count -gt 16) {
        throw "Visual Studio must expose a bounded x64 VC Redistributable set"
    }

    $reviewed = @()
    foreach ($candidate in $candidates) {
        $signature =
            Get-AuthenticodeSignature -LiteralPath $candidate.FullName
        if ($signature.Status -ne
                [System.Management.Automation.SignatureStatus]::Valid -or
            $null -eq $signature.SignerCertificate -or
            [string]$signature.SignerCertificate.Subject -notmatch
                '(^|,\s*)CN=Microsoft Corporation(,|$)') {
            continue
        }

        $versionText = [Diagnostics.FileVersionInfo]::GetVersionInfo(
            $candidate.FullName).FileVersion
        $match = [regex]::Match(
            [string]$versionText,
            '(?<v>\d+\.\d+\.\d+(?:\.\d+)?)')
        if (-not $match.Success) {
            continue
        }
        try {
            $version = [version]$match.Groups["v"].Value
        } catch {
            continue
        }

        $reviewed += [pscustomobject]@{
            Path = [string]$candidate.FullName
            Version = $version
        }
    }

    if ($reviewed.Count -lt 1) {
        throw "No Microsoft-signed x64 VC Redistributable was found in the release compiler installation"
    }

    $selected = @(
        $reviewed |
            Sort-Object -Property Version -Descending
    )[0]
    return [string]$selected.Path
}

function Assert-SafeRelativePackagePath {
    param([string]$RelativePath)
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        $RelativePath.Length -gt 512 -or
        [IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath.Contains(":") -or
        $RelativePath.Contains("*") -or
        $RelativePath.Contains("?") -or
        $RelativePath.Contains([char]0)) {
        throw "Third-party deployment path is unsafe"
    }
    $normalized = $RelativePath.Replace("\", "/").Trim("/")
    if ([string]::IsNullOrWhiteSpace($normalized)) {
        throw "Third-party deployment path is empty"
    }
    $segments = @($normalized.Split("/"))
    if ($segments.Count -lt 1 -or $segments.Count -gt 8) {
        throw "Third-party deployment path depth is outside the reviewed bound"
    }
    foreach ($segment in $segments) {
        if ($segment -in @(".", "..") -or
            $segment -notmatch "^[A-Za-z0-9._+-]{1,128}$") {
            throw "Third-party deployment path contains an unsafe segment"
        }
    }
    return ($segments -join "/")
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
        "xinput-adapter" = @{ kind = "cmake-shared-library"; target = "hydra_xinput_adapter"; fileName = "hydra_xinput_adapter.dll" }
        "gate-c-adapter" = @{ kind = "cmake-shared-library"; target = "hydra_gate_c_adapter"; fileName = "hydra_gate_c_adapter.dll" }
        "gate-c-shim" = @{ kind = "cmake-shared-library"; target = "hydra_gate_c_shim"; fileName = "hydra_gate_c_shim.dll" }
        "gate-c-external-bridge" = @{ kind = "cmake-shared-library"; target = "hydra_gate_c_external_bridge"; fileName = "hydra_gate_c_external_bridge.dll" }
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
        if ([string]$artifact.kind -in @("cmake-executable", "cmake-shared-library")) {
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

function Resolve-ReviewedBuildArtifact {
    param(
        [string]$BuildRoot,
        [string]$Configuration,
        [string]$FileName
    )
    $candidates = @(
        (Resolve-UnderRoot -Root $BuildRoot -Child $FileName),
        (Resolve-UnderRoot -Root $BuildRoot -Child (Join-Path $Configuration $FileName))
    ) | Select-Object -Unique
    $found = @($candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
    if ($found.Count -ne 1) {
        throw "Reviewed release artifact must resolve to exactly one single-config or multi-config build output: $FileName"
    }
    return [string]$found[0]
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
    [string]$_.kind -in @("cmake-executable", "cmake-shared-library")
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
$archOutput = Resolve-UnderRoot -Root $OutputDirectory -Child "x64"
if (Test-Path -LiteralPath $archOutput) {
    if (@(Get-ChildItem -LiteralPath $archOutput -Force).Count -ne 0) {
        throw "Release x64 output directory must be empty before signing/deployment"
    }
} else {
    New-Item -ItemType Directory -Path $archOutput | Out-Null
}
$records = @()

foreach ($artifact in $manifest.artifacts) {
    $kind = [string]$artifact.kind
    $fileName = [string]$artifact.fileName
    $sourceKind = ""
    $targetName = ""

    if ($kind -in @("cmake-executable", "cmake-shared-library")) {
        $extensionPattern = if ($kind -eq "cmake-shared-library") {
            '^[A-Za-z0-9._-]+\.dll$'
        } else {
            '^[A-Za-z0-9._-]+\.exe$'
        }
        Assert-SafeBasename -FileName $fileName -ExtensionPattern $extensionPattern
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
            $source = Resolve-ReviewedBuildArtifact -BuildRoot $BuildX64 -Configuration $Configuration -FileName $fileName
        } else {
            $source = Resolve-UnderRoot -Root $repositoryRoot -Child ([string]$artifact.sourcePath)
        }
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Missing release artifact: $($artifact.id) $architecture"
        }
        if ($kind -in @("cmake-executable", "cmake-shared-library")) {
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
        if ($kind -in @("cmake-executable", "cmake-shared-library")) {
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

$winDeployQt = Get-ReviewedWinDeployQt -BuildRoot $BuildX64
$signedUi = Resolve-UnderRoot -Root $archOutput -Child "HydraSeat.exe"
$deployArgs = @(
    "--release",
    "--no-translations",
    "--no-compiler-runtime",
    "--no-system-d3d-compiler",
    "--no-opengl-sw",
    "--dir",
    $archOutput,
    $signedUi
)
& $winDeployQt @deployArgs | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed while freezing the reviewed Qt 6.8.3 runtime"
}

$vcRedistSource = Get-ReviewedVcRedist -BuildRoot $BuildX64
$vcRedistDestination =
    Resolve-UnderRoot -Root $archOutput -Child "vc_redist.x64.exe"
Copy-Item -LiteralPath $vcRedistSource -Destination $vcRedistDestination -Force
$vcRedistSignature =
    Get-AuthenticodeSignature -LiteralPath $vcRedistDestination
if ($vcRedistSignature.Status -ne
        [System.Management.Automation.SignatureStatus]::Valid -or
    $null -eq $vcRedistSignature.SignerCertificate -or
    [string]$vcRedistSignature.SignerCertificate.Subject -notmatch
        '(^|,\s*)CN=Microsoft Corporation(,|$)') {
    throw "Copied Visual C++ Redistributable prerequisite is not validly signed by Microsoft"
}

$coreNames = @{}
foreach ($record in $records) {
    $coreNames[([string]$record.fileName).ToUpperInvariant()] = $true
}
$thirdPartyRecords = @()
$rootPrefix = [System.IO.Path]::GetFullPath($archOutput).TrimEnd('\') + '\'
$runtimeEntries = @(Get-ChildItem -LiteralPath $archOutput -Force -Recurse)
if ($runtimeEntries.Count -gt 128) {
    throw "Qt deployment produced an unbounded file/directory inventory"
}
foreach ($entry in $runtimeEntries) {
    if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Qt deployment produced a reparse point"
    }
    if ($entry.PSIsContainer) { continue }

    $fullPath = [System.IO.Path]::GetFullPath([string]$entry.FullName)
    if (-not $fullPath.StartsWith(
            $rootPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Qt deployment file escaped the x64 output root"
    }
    $relativePath = Assert-SafeRelativePackagePath -RelativePath (
        $fullPath.Substring($rootPrefix.Length)
    )
    if ($coreNames.ContainsKey($relativePath.ToUpperInvariant())) {
        continue
    }
    if ($relativePath.Equals(
            "vc_redist.x64.exe",
            [System.StringComparison]::OrdinalIgnoreCase)) {
        $vcSignature = Get-AuthenticodeSignature -LiteralPath $fullPath
        if ($vcSignature.Status -ne
                [System.Management.Automation.SignatureStatus]::Valid -or
            $null -eq $vcSignature.SignerCertificate) {
            throw "Packaged Visual C++ Redistributable prerequisite is invalid or unsigned"
        }

        $thirdPartyRecords += [ordered]@{
            id = "msvc-runtime-installer"
            provider = "Microsoft"
            version = "VS2022"
            deploymentTool = "visual-studio-redist"
            relativePath = $relativePath
            sha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
            bytes = [UInt64]$entry.Length
        }
        continue
    }

    if (-not $relativePath.EndsWith(
            ".dll",
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Reviewed Qt deployment produced an unreviewed non-DLL file: $relativePath"
    }

    $thirdPartyRecords += [ordered]@{
        id = "qt-runtime"
        provider = "Qt Project"
        version = "6.8.3"
        deploymentTool = "windeployqt"
        relativePath = $relativePath
        sha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
        bytes = [UInt64]$entry.Length
    }
}

if (($thirdPartyRecords.Count + $records.Count) -gt 64) {
    throw "Release package exceeds the reviewed 64-file bound"
}
$requiredQt = @(
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "platforms/qwindows.dll"
)
$deployedPaths = @{}
foreach ($record in $thirdPartyRecords) {
    $deployedPaths[([string]$record.relativePath).ToUpperInvariant()] = $true
}
foreach ($required in $requiredQt) {
    if (-not $deployedPaths.ContainsKey($required.ToUpperInvariant())) {
        throw "windeployqt did not produce required Qt runtime file: $required"
    }
}
if (-not $deployedPaths.ContainsKey("VC_REDIST.X64.EXE")) {
    throw "release signer did not package the required Visual C++ Redistributable prerequisite"
}
$thirdPartyRecords = @(
    $thirdPartyRecords | Sort-Object -Property relativePath
)

# windeployqt must not modify any signed HydraSeat-owned artifact.
foreach ($record in $records) {
    $path = Resolve-UnderRoot -Root $archOutput -Child ([string]$record.fileName)
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne [string]$record.signedSha256) {
        throw "Qt deployment modified a signed HydraSeat artifact: $($record.fileName)"
    }
}

$provenance = [ordered]@{
    schemaVersion = 2
    releaseVersion = $ReleaseVersion
    releaseRevision = $ReleaseRevision
    commitSha = $CommitSha
    signingManifest = [System.IO.Path]::GetFileName($ManifestPath)
    timestampUrl = $TimestampUrl
    artifacts = $records
    thirdPartyRedistributables = $thirdPartyRecords
}
$provenancePath = Join-Path $OutputDirectory "signing-provenance.json"
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $provenancePath -Encoding UTF8
$provenanceSignaturePath = $provenancePath + ".p7s"
Write-DetachedCmsSignature -ContentPath $provenancePath -SignaturePath $provenanceSignaturePath -Certificate $certificate
Assert-DetachedCmsSignature -ContentPath $provenancePath -SignaturePath $provenanceSignaturePath -ExpectedThumbprint $normalizedThumbprint
Write-Host "Signed and verified $($records.Count) fixed release artifacts."
Write-Host "Provenance: $provenancePath"
Write-Host "Provenance signature: $provenanceSignaturePath"
