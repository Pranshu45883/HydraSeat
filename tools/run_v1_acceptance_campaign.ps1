#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("New", "RunStage", "Resume", "Summarize", "SelfTest")]
    [string]$Action,
    [string]$SessionDir,
    [string]$CampaignCtlPath,
    [string]$ProbePath,
    [string]$HostCtlPath,
    [string]$InstallStatePath = "$env:ProgramData\HydraSeat\install-state.json",
    [ValidateSet("Preflight", "Phase3Physical", "DisplayReconnect", "DifferentGames",
        "SameTitle", "SeatIndependence", "ReturnToWindows", "InstallRepairUninstall",
        "RebootStartup", "UpdateRollback", "Offline", "FaultRecovery", "PerformanceSoak")]
    [string]$Stage,
    [string]$CampaignId,
    [string]$RcCommitSha,
    [string]$ReleaseArtifactSha256,
    [string]$ReleaseArtifactPath,
    [UInt64]$ReleaseRevision,
    [ValidateSet("x64", "x86", "arm64")]
    [string]$Architecture,
    [string]$ProfileSha256,
    [string]$WindowsBuild,
    [string]$TopologyFingerprintSha256,
    [string]$ScenarioIdentity,
    [string]$EvidencePath,
    [ValidateSet("Synthetic", "ControlledProcess", "Physical")]
    [string]$EvidenceOrigin = "ControlledProcess",
    [switch]$AutomatedPass,
    [ValidateSet("Pending", "Pass", "Fail")]
    [string]$ManualVerdict = "Pending",
    [string]$ManualNote = "",
    [int]$ProbeSamples = 1,
    [int]$ProbeIntervalMilliseconds = 1000,
    [switch]$DevelopmentUnsignedEvidence,
    [switch]$AcknowledgeDevelopmentEvidence
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:CampaignRoot = Join-Path $script:RepoRoot "out\v1-acceptance"
$script:CampaignFileName = "v1-acceptance-campaign.json"
$script:MaximumEvidenceBytes = 1024 * 1024
$script:EvidenceClassByStage = @{
    Preflight = "Controlled"
    Phase3Physical = "Physical"
    DisplayReconnect = "Physical"
    DifferentGames = "RealGame"
    SameTitle = "RealGame"
    SeatIndependence = "RealGame"
    ReturnToWindows = "RealGame"
    InstallRepairUninstall = "CleanMachineInstall"
    RebootStartup = "CleanMachineInstall"
    UpdateRollback = "CleanMachineInstall"
    Offline = "Controlled"
    FaultRecovery = "Manual"
    PerformanceSoak = "RealGame"
}

function Get-UnixNow {
    return [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
}

function Assert-Token {
    param([string]$Value, [string]$Label, [int]$Maximum = 128)
    if ([string]::IsNullOrWhiteSpace($Value) -or $Value.Length -gt $Maximum -or
        $Value -notmatch "^[A-Za-z0-9._:@+\-]+$") {
        throw "$Label is invalid or unbounded"
    }
}

function Assert-Hex {
    param([string]$Value, [int]$Length, [string]$Label)
    if ($null -eq $Value -or $Value -notmatch ("^[0-9a-f]{" + $Length + "}$")) {
        throw "$Label must be exactly $Length lowercase hexadecimal characters"
    }
}

function Assert-LeafNotReparse {
    param([string]$PathValue, [string]$Label)
    $item = Get-Item -LiteralPath $PathValue -Force -ErrorAction Stop
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Label must be a non-reparse regular file"
    }
    return $item.FullName
}

function Resolve-FixedTool {
    param([string]$PathValue, [string]$ExpectedName, [string]$Label)
    if ([string]::IsNullOrWhiteSpace($PathValue)) { throw "$Label path is required" }
    $resolved = Assert-LeafNotReparse -PathValue $PathValue -Label $Label
    if (-not ([IO.Path]::GetFileName($resolved)).Equals($ExpectedName,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must be the fixed $ExpectedName executable"
    }
    return $resolved
}

function Resolve-CampaignSession {
    param([string]$PathValue, [bool]$Create)
    $root = [IO.Path]::GetFullPath($script:CampaignRoot).TrimEnd('\') + '\'
    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        if (-not $Create) { throw "SessionDir is required" }
        $PathValue = Join-Path $script:CampaignRoot (
            "v1-" + [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss") + "-" +
            [Guid]::NewGuid().ToString("N").Substring(0, 8))
    }
    $full = [IO.Path]::GetFullPath($PathValue).TrimEnd('\')
    if (-not ($full + '\').StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Acceptance session must remain under out\v1-acceptance"
    }
    if ($Create) {
        New-Item -ItemType Directory -Path $full -Force | Out-Null
    } elseif (-not (Test-Path -LiteralPath $full -PathType Container)) {
        throw "Acceptance session does not exist"
    }
    $item = Get-Item -LiteralPath $full -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Acceptance session root must not be a reparse point"
    }
    return $full
}

function Invoke-Fixed {
    param([string]$Executable, [string[]]$Arguments, [switch]$AllowFailure)
    # Windows PowerShell 5 can promote native stderr in a merged pipeline to a
    # NativeCommandError when the script-wide preference is Stop. Intentional
    # fail-closed probes need the native exit code/output instead of an unrelated
    # PowerShell exception, so suppress promotion only around this fixed tool call.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = @(& $Executable @Arguments 2>&1 | ForEach-Object { [string]$_ })
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "Fixed tool failed ($exitCode): $Executable`n$($output -join [Environment]::NewLine)"
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Lines = $output }
}

function Remove-FileVerified {
    param([string]$PathValue, [string]$Label)
    if (Test-Path -LiteralPath $PathValue) {
        Remove-Item -LiteralPath $PathValue -Force
    }
    if (Test-Path -LiteralPath $PathValue) {
        throw "$Label cleanup could not be verified"
    }
}

function Write-TextAtomic {
    param([string]$PathValue, [string]$Text)
    $temporary = $PathValue + ".new"
    Remove-FileVerified -PathValue $temporary -Label "Stale staging file"
    try {
        [IO.File]::WriteAllText($temporary, $Text,
            (New-Object Text.UTF8Encoding($false)))
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::Open,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
        try { $stream.Flush($true) } finally { $stream.Dispose() }
        Move-Item -LiteralPath $temporary -Destination $PathValue -Force
    } finally {
        Remove-FileVerified -PathValue $temporary -Label "Sensitive staging file"
    }
}

function Assert-ExactJsonFields {
    param($Object, [string[]]$Expected, [string]$Label)
    if ($null -eq $Object) { throw "$Label is missing" }
    $actual = @($Object.PSObject.Properties.Name | Sort-Object)
    $wanted = @($Expected | Sort-Object)
    if (($actual -join "`n") -ne ($wanted -join "`n")) {
        throw "$Label has missing or unknown fields"
    }
}

function Import-Evidence {
    param([string]$Source, [string]$Destination, [string]$SelectedStage,
          $CampaignIdentity, [bool]$RequestedAutomatedPass, [string]$RequestedOrigin,
          [string]$RequestedClass)
    $resolved = Assert-LeafNotReparse -PathValue $Source -Label "Evidence"
    $item = Get-Item -LiteralPath $resolved
    if ($item.Length -le 0 -or $item.Length -gt $script:MaximumEvidenceBytes) {
        throw "Evidence is empty or exceeds the 1 MiB campaign bound"
    }
    $document = Get-Content -LiteralPath $resolved -Raw -Encoding UTF8 | ConvertFrom-Json
    $now = Get-UnixNow
    $created = $now
    $testName = $SelectedStage
    $automated = $RequestedAutomatedPass
    if ($SelectedStage -eq "Phase3Physical") {
        if ($document.schema_version -ne 1 -or [string]$document.state -ne "MANUAL_PASS" -or
            [string]$document.manual_verdict -ne "PASS" -or
            [Int64]$document.evidence_valid_until_unix -lt $now -or
            [Int64]$document.manual_verdict_unix -le 0 -or
            ($now - [Int64]$document.manual_verdict_unix) -gt (7 * 24 * 60 * 60)) {
            throw "Phase 3 child evidence is not a fresh explicit MANUAL_PASS"
        }
        if ($RequestedClass -ne "Physical" -or $RequestedOrigin -ne "Physical") {
            throw "Phase 3 evidence must remain the exact Physical class and origin"
        }
        if ([string]$document.environment.architecture -ne [string]$CampaignIdentity.architecture -or
            [string]$document.environment.windows_build -ne [string]$CampaignIdentity.windows_build -or
            [string]$document.profile.sha256 -ne [string]$CampaignIdentity.profile_sha256 -or
            [string]$document.session_id -ne [string]$CampaignIdentity.session_run_id) {
            throw "Phase 3 evidence build/architecture/profile/session does not match the campaign"
        }
        $created = [Int64]$document.manual_verdict_unix
        $automated = $true
        if (-not $RequestedAutomatedPass) {
            throw "Phase 3 MANUAL_PASS evidence requires AutomatedPass acknowledgement"
        }
    } else {
        $binding = $document.release_binding
        Assert-ExactJsonFields -Object $binding -Expected @(
            "schema_version", "campaign_schema_version", "campaign_id", "session_run_id",
            "stage", "test_name", "origin", "evidence_class", "created_unix", "rc_commit_sha",
            "release_artifact_sha256", "release_artifact_name", "release_revision", "architecture",
            "profile_sha256", "install_state_sha256", "windows_build",
            "topology_fingerprint_sha256", "scenario_identity", "automated_passed") `
            -Label "Evidence release_binding"
        if ($binding.schema_version -ne 2 -or $binding.campaign_schema_version -ne 2 -or
            [string]$binding.stage -ne $SelectedStage -or
            [string]$binding.origin -ne $RequestedOrigin -or
            [string]$binding.evidence_class -ne $RequestedClass) {
            throw "Evidence release_binding schema/stage/origin/class mismatch"
        }
        Assert-Token ([string]$binding.test_name) "Evidence test_name"
        $created = [Int64]$binding.created_unix
        if ($created -le 0 -or $created -gt ($now + 300) -or ($now - [Math]::Min($now, $created)) -gt (7 * 24 * 60 * 60)) {
            throw "Evidence release_binding timestamp is stale or implausible"
        }
        if ([string]$binding.campaign_id -ne [string]$CampaignIdentity.campaign_id -or
            [string]$binding.session_run_id -ne [string]$CampaignIdentity.session_run_id -or
            [string]$binding.rc_commit_sha -ne [string]$CampaignIdentity.rc_commit_sha -or
            [string]$binding.release_artifact_sha256 -ne [string]$CampaignIdentity.release_artifact_sha256 -or
            [string]$binding.release_artifact_name -ne [string]$CampaignIdentity.release_artifact_name -or
            [UInt64]$binding.release_revision -ne [UInt64]$CampaignIdentity.release_revision -or
            [string]$binding.architecture -ne [string]$CampaignIdentity.architecture -or
            [string]$binding.profile_sha256 -ne [string]$CampaignIdentity.profile_sha256 -or
            [string]$binding.install_state_sha256 -ne [string]$CampaignIdentity.install_state_sha256 -or
            [string]$binding.windows_build -ne [string]$CampaignIdentity.windows_build -or
            [string]$binding.topology_fingerprint_sha256 -ne [string]$CampaignIdentity.topology_fingerprint_sha256 -or
            [string]$binding.scenario_identity -ne [string]$CampaignIdentity.scenario_identity) {
            throw "Evidence release_binding does not match the exact campaign/session RC/build/profile/input artifacts"
        }
        if ($binding.automated_passed -isnot [bool] -or
            [bool]$binding.automated_passed -ne $RequestedAutomatedPass) {
            throw "Evidence automated verdict does not match the requested campaign verdict"
        }
        $testName = [string]$binding.test_name
        $automated = [bool]$binding.automated_passed
    }
    $sourceHash = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
    Copy-Item -LiteralPath $resolved -Destination $Destination -Force
    $copied = Assert-LeafNotReparse -PathValue $Destination -Label "Imported evidence"
    $copiedHash = (Get-FileHash -LiteralPath $copied -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($copiedHash -ne $sourceHash) { throw "Imported evidence bytes changed during staging" }
    return [pscustomobject]@{
        Hash = $copiedHash
        CreatedUnix = $created
        TestName = $testName
        AutomatedPassed = $automated
    }
}

function Assert-CampaignArtifacts {
    param([string]$CampaignPath)
    $resolvedCampaign = Assert-LeafNotReparse -PathValue $CampaignPath -Label "Campaign"
    if (Test-Path -LiteralPath ($resolvedCampaign + ".new")) {
        throw "Stale campaign staging file exists"
    }
    $document = Get-Content -LiteralPath $resolvedCampaign -Raw -Encoding UTF8 | ConvertFrom-Json
    $sessionLeaf = [IO.Path]::GetFileName((Split-Path -Parent $resolvedCampaign))
    if ($document.schema_version -ne 2 -or
        [string]$document.identity.campaign_id -ne $sessionLeaf -or
        [string]$document.identity.session_run_id -ne $sessionLeaf) {
        throw "Campaign schema/session binding does not match its authoritative session directory"
    }
    $artifactRoot = Join-Path (Split-Path -Parent $resolvedCampaign) "artifacts"
    $seenArtifacts = @{}
    $seenTests = @{}
    foreach ($record in $document.stages) {
        foreach ($evidence in $record.evidence) {
            $expectedClass = [string]$script:EvidenceClassByStage[[string]$record.stage]
            $expectedOrigin = if ($expectedClass -eq "Controlled") { "ControlledProcess" } else { "Physical" }
            if ([string]$evidence.evidence_class -ne $expectedClass -or
                [string]$evidence.origin -ne $expectedOrigin -or
                [UInt64]$evidence.campaign_schema_version -ne 2 -or
                [string]$evidence.campaign_id -ne [string]$document.identity.campaign_id -or
                [string]$evidence.session_run_id -ne [string]$document.identity.session_run_id) {
                throw "Campaign evidence origin/class/schema/session binding is inconsistent"
            }
            $name = [string]$evidence.evidence_artifact_name
            Assert-Token $name "Evidence artifact name"
            if ([IO.Path]::GetFileName($name) -ne $name) {
                throw "Evidence artifact must be a leaf name"
            }
            $testName = [string]$evidence.test_name
            Assert-Token $testName "Evidence test name"
            if ($seenArtifacts.ContainsKey($name) -or $seenTests.ContainsKey($testName)) {
                throw "Duplicate evidence artifact or test identity"
            }
            $seenArtifacts[$name] = $true
            $seenTests[$testName] = $true
            $artifact = Assert-LeafNotReparse -PathValue (Join-Path $artifactRoot $name) `
                -Label "Evidence artifact"
            $item = Get-Item -LiteralPath $artifact
            if ($item.Length -le 0 -or $item.Length -gt $script:MaximumEvidenceBytes) {
                throw "Evidence artifact is empty or oversized"
            }
            $actual = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne [string]$evidence.content_sha256) {
                throw "Evidence artifact bytes no longer match campaign hash: $name"
            }
        }
    }
    return $document
}

function Invoke-SelfTest {
    $ctl = Resolve-FixedTool -PathValue $CampaignCtlPath `
        -ExpectedName "hydraseat_acceptance_campaignctl.exe" -Label "Campaign control"
    $root = Join-Path $env:TEMP ("hydraseat-v1-acceptance-selftest-" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $root | Out-Null
    try {
        $session = Join-Path $root "selftest"
        New-Item -ItemType Directory -Path $session | Out-Null
        $file = Join-Path $session $script:CampaignFileName
        $now = Get-UnixNow
        Invoke-Fixed $ctl @("new", $file, "selftest", ("a" * 40), ("b" * 64),
            "HydraSeat-x64.zip", "7", "x64", ("c" * 64), ("d" * 64),
            "10.0.26100", ("e" * 64), "selftest-scenario", [string]$now) | Out-Null
        [void](Assert-CampaignArtifacts -CampaignPath $file)
        Invoke-Fixed $ctl @("start", $file, "Preflight", [string]($now + 1)) | Out-Null
        Invoke-Fixed $ctl @("attach", $file, "selftest-preflight", "Preflight",
            "ControlledProcess", [string]($now + 1), ("f" * 64),
            "Preflight-selftest.json", "Preflight-selftest", "true",
            "bounded-selftest", [string]($now + 1)) | Out-Null
        Invoke-Fixed $ctl @("start", $file, "Phase3Physical", [string]($now + 2)) | Out-Null
        $rejected = Invoke-Fixed $ctl @("attach", $file, "forged-physical", "Phase3Physical",
            "Synthetic", [string]($now + 2), ("a" * 64),
            "Phase3-forged.json", "Phase3-forged", "true",
            "must-not-pass", [string]($now + 2)) -AllowFailure
        if ($rejected.ExitCode -eq 0 -or ($rejected.Lines -join "`n") -notmatch "PhysicalEvidenceRequired") {
            throw "Self-test failed: synthetic evidence was not rejected for a physical stage"
        }
        Write-Host "V1 acceptance campaign runner self-test passed."
    } finally {
        if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    }
}

if ($Action -eq "SelfTest") {
    Invoke-SelfTest
    exit 0
}

$ctl = Resolve-FixedTool -PathValue $CampaignCtlPath `
    -ExpectedName "hydraseat_acceptance_campaignctl.exe" -Label "Campaign control"

if ($Action -eq "New") {
    Assert-Token $CampaignId "CampaignId"
    Assert-Hex $RcCommitSha 40 "RcCommitSha"
    Assert-Hex $ReleaseArtifactSha256 64 "ReleaseArtifactSha256"
    if ($ReleaseRevision -eq 0) { throw "ReleaseRevision must be non-zero" }
    if ([string]::IsNullOrWhiteSpace($Architecture)) { throw "Architecture is required" }
    Assert-Hex $ProfileSha256 64 "ProfileSha256"
    Assert-Token $WindowsBuild "WindowsBuild"
    Assert-Hex $TopologyFingerprintSha256 64 "TopologyFingerprintSha256"
    Assert-Token $ScenarioIdentity "ScenarioIdentity"

    $releaseArtifact = Assert-LeafNotReparse -PathValue $ReleaseArtifactPath -Label "Release artifact"
    $releaseArtifactName = [IO.Path]::GetFileName($releaseArtifact)
    Assert-Token $releaseArtifactName "Release artifact name"
    $actualArtifactHash = (Get-FileHash -LiteralPath $releaseArtifact -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualArtifactHash -ne $ReleaseArtifactSha256) {
        throw "Release artifact bytes do not match ReleaseArtifactSha256"
    }

    $installedState = Assert-LeafNotReparse -PathValue $InstallStatePath -Label "Install state"
    $installStateHash = (Get-FileHash -LiteralPath $installedState -Algorithm SHA256).Hash.ToLowerInvariant()
    $installState = Get-Content -LiteralPath $installedState -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$installState.commitSha -ne $RcCommitSha -or
        [UInt64]$installState.releaseRevision -ne $ReleaseRevision -or
        [string]$installState.architecture -ne $Architecture) {
        throw "Install state does not match the exact RC commit/revision/architecture"
    }

    if ([string]::IsNullOrWhiteSpace($SessionDir)) {
        $SessionDir = Join-Path $script:CampaignRoot $CampaignId
    }
    $session = Resolve-CampaignSession -PathValue $SessionDir -Create $true
    if (-not ([IO.Path]::GetFileName($session)).Equals($CampaignId,
            [StringComparison]::Ordinal)) {
        throw "CampaignId must exactly match the acceptance session directory name"
    }
    $campaign = Join-Path $session $script:CampaignFileName
    if (Test-Path -LiteralPath $campaign) { throw "Campaign already exists" }
    Invoke-Fixed $ctl @("new", $campaign, $CampaignId, $RcCommitSha,
        $ReleaseArtifactSha256, $releaseArtifactName, [string]$ReleaseRevision, $Architecture,
        $ProfileSha256, $installStateHash, $WindowsBuild, $TopologyFingerprintSha256,
        $ScenarioIdentity, [string](Get-UnixNow)) | Out-Null
    [void](Assert-CampaignArtifacts -CampaignPath $campaign)
    Write-Host "Created acceptance campaign: $session"
    exit 0
}

$session = Resolve-CampaignSession -PathValue $SessionDir -Create $false
$campaign = Join-Path $session $script:CampaignFileName
if (-not (Test-Path -LiteralPath $campaign -PathType Leaf)) { throw "Campaign file is missing" }
$campaignDocument = Assert-CampaignArtifacts -CampaignPath $campaign

if ($Action -eq "Summarize") {
    Invoke-Fixed $ctl @("summary", $campaign, [string](Get-UnixNow)) | ForEach-Object {
        $_.Lines | ForEach-Object { Write-Host $_ }
    }
    exit 0
}

if ($Action -eq "Resume") {
    Invoke-Fixed $ctl @("recover", $campaign, [string](Get-UnixNow)) | Out-Null
    Invoke-Fixed $ctl @("summary", $campaign, [string](Get-UnixNow)) | ForEach-Object {
        $_.Lines | ForEach-Object { Write-Host $_ }
    }
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Stage)) { throw "RunStage requires Stage" }
if ($DevelopmentUnsignedEvidence -ne $AcknowledgeDevelopmentEvidence) {
    throw "Development evidence requires both explicit switches"
}
$expectedEvidenceClass = [string]$script:EvidenceClassByStage[$Stage]
if ([string]::IsNullOrWhiteSpace($expectedEvidenceClass)) {
    throw "$Stage has no reviewed v2 evidence-class contract"
}
$expectedEvidenceOrigin = if ($expectedEvidenceClass -eq "Controlled") { "ControlledProcess" } else { "Physical" }
if ($EvidenceOrigin -ne $expectedEvidenceOrigin) {
    throw "$Stage requires evidence class $expectedEvidenceClass with origin $expectedEvidenceOrigin"
}

if ([string]::IsNullOrWhiteSpace($EvidencePath)) {
    if ($Stage -ne "Preflight") {
        throw "$Stage requires explicit externally-bound evidence; generated probes cannot become physical/manual evidence"
    }
    if ($EvidenceOrigin -ne "ControlledProcess") {
        throw "Generated Preflight evidence must remain ControlledProcess"
    }
}

$now = Get-UnixNow
Invoke-Fixed $ctl @("start", $campaign, $Stage, [string]$now) | Out-Null
$artifactDir = Join-Path $session "artifacts"
New-Item -ItemType Directory -Path $artifactDir -Force | Out-Null
$evidenceFile = Join-Path $artifactDir ($Stage + "-" + [Guid]::NewGuid().ToString("N") + ".json")
$evidenceArtifactName = [IO.Path]::GetFileName($evidenceFile)
$passed = $AutomatedPass.IsPresent
$evidenceCreated = $now
$testName = $Stage
$attached = $false

try {
    if (-not [string]::IsNullOrWhiteSpace($EvidencePath)) {
        $imported = Import-Evidence -Source $EvidencePath -Destination $evidenceFile `
            -SelectedStage $Stage -CampaignIdentity $campaignDocument.identity `
            -RequestedAutomatedPass $passed -RequestedOrigin $EvidenceOrigin `
            -RequestedClass $expectedEvidenceClass
        $evidenceHash = [string]$imported.Hash
        $evidenceCreated = [Int64]$imported.CreatedUnix
        $testName = [string]$imported.TestName
        $passed = [bool]$imported.AutomatedPassed
    } else {
        $probe = Resolve-FixedTool -PathValue $ProbePath `
            -ExpectedName "hydraseat_acceptance_probe.exe" -Label "Acceptance probe"
        $probeArguments = @("--state", $InstallStatePath, "--samples", [string]$ProbeSamples,
            "--interval-ms", [string]$ProbeIntervalMilliseconds)
        if ($DevelopmentUnsignedEvidence) {
            $probeArguments += @("--allow-unsigned-development", "--acknowledge-development-evidence")
        }
        $probeResult = Invoke-Fixed $probe $probeArguments -AllowFailure
        if ($probeResult.ExitCode -ne 0) {
            throw "Acceptance probe failed; raw diagnostic output is not retained as release evidence"
        }
        $probeText = $probeResult.Lines -join [Environment]::NewLine
        $probeDocument = $probeText | ConvertFrom-Json
        if ($probeDocument.schema_version -ne 1 -or
            [string]$probeDocument.commit_sha -ne [string]$campaignDocument.identity.rc_commit_sha -or
            [UInt64]$probeDocument.release_revision -ne [UInt64]$campaignDocument.identity.release_revision -or
            [string]$probeDocument.architecture -ne [string]$campaignDocument.identity.architecture -or
            [string]$probeDocument.install_state_sha256 -ne [string]$campaignDocument.identity.install_state_sha256 -or
            $probeDocument.all_owned_files_verified -ne $true) {
            throw "Acceptance probe output is not bound to the exact installed RC build"
        }
        if ([bool]$probeDocument.development_unsigned_allowed -ne $DevelopmentUnsignedEvidence.IsPresent) {
            throw "Acceptance probe development-signing state does not match the requested evidence mode"
        }
        Write-TextAtomic -PathValue $evidenceFile -Text ($probeText + [Environment]::NewLine)
        $evidenceHash = (Get-FileHash -LiteralPath $evidenceFile -Algorithm SHA256).Hash.ToLowerInvariant()
        $passed = $true
        $testName = "acceptance-probe-Preflight"
    }

    Assert-Token $testName "Evidence test name"
    $evidenceId = ($Stage.ToLowerInvariant() + "-" + [Guid]::NewGuid().ToString("N"))
    $attachNote = if ($passed) { "automated checks passed; manual review remains authoritative" } `
                  else { "automated checks failed; manual review cannot override" }
    Invoke-Fixed $ctl @("attach", $campaign, $evidenceId, $Stage, $EvidenceOrigin,
        [string]$evidenceCreated, $evidenceHash, $evidenceArtifactName, $testName,
        $passed.ToString().ToLowerInvariant(), $attachNote, [string](Get-UnixNow)) | Out-Null
    $attached = $true

    if ($ManualVerdict -ne "Pending") {
        if ([string]::IsNullOrWhiteSpace($ManualNote)) {
            throw "Manual Pass/Fail requires a non-empty ManualNote"
        }
        Invoke-Fixed $ctl @("verdict", $campaign, $Stage, $ManualVerdict,
            $ManualNote, [string](Get-UnixNow)) | Out-Null
    }
    $campaignDocument = Assert-CampaignArtifacts -CampaignPath $campaign
} catch {
    if (-not $attached -and (Test-Path -LiteralPath $evidenceFile)) {
        Remove-FileVerified -PathValue $evidenceFile -Label "Unattached evidence"
    }
    throw
}

Invoke-Fixed $ctl @("summary", $campaign, [string](Get-UnixNow)) | ForEach-Object {
    $_.Lines | ForEach-Object { Write-Host $_ }
}
