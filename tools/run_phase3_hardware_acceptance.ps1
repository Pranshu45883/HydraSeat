[CmdletBinding()]
param(
    [string]$ProfilePath = "workspace_config.json",
    [string]$BuildRoot = "build-x64",
    [ValidateSet("All", "GateA", "GateB", "GateC", "Summarize")]
    [string]$Stage = "All",
    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",
    [string]$SessionDir,
    [string]$Resume,
    [string]$SharedTestDeviceId,
    [int]$MinimumSoakMinutes = 10,
    [string]$HardwareNotes = "",
    [switch]$SensitiveKeyLogging,
    [switch]$AcknowledgeSensitiveKeyLogging,
    [switch]$NonInteractive,
    [switch]$ForceStage,
    [switch]$SelfTest
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:ManifestName = "phase3-hardware-manifest.json"
$script:ReportName = "phase3-hardware-report.json"
$script:OwnedProcess = $null
$script:EvidenceValiditySeconds = 24 * 60 * 60

$GateAChecks = @(
    "two_keyboards_distinct",
    "two_pointing_devices_distinct",
    "key_down_up_transitions",
    "composite_child_removal",
    "unplug_replug_identity",
    "soak_minimum_duration",
    "drop_counter_reviewed"
)
$GateBChecks = @(
    "seat1_exclusive_routing",
    "seat2_exclusive_routing",
    "unassigned_fails_closed",
    "shared_ambiguous_fails_closed",
    "missing_target_explicit_failure",
    "trace_seat_target_reviewed"
)
$GateCChecks = @(
    "two_controlled_targets_visible",
    "seat1_changes_only_target1",
    "seat2_changes_only_target2",
    "unassigned_shared_fail_closed",
    "normal_windows_input_not_claimed_suppressed",
    "cleanup_no_owned_child_left",
    "metrics_reviewed"
)

function Get-UtcString {
    return [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ")
}

function Resolve-ExistingPath {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    $resolved = Resolve-Path -LiteralPath $PathValue -ErrorAction Stop
    return $resolved.Path
}

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    $stream = [System.IO.File]::Open(
        $PathValue,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read
    )
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $bytes = $sha.ComputeHash($stream)
            return ([System.BitConverter]::ToString($bytes)).Replace("-", "").ToLowerInvariant()
        } finally {
            if ($null -ne $sha) { $sha.Dispose() }
        }
    } finally {
        $stream.Dispose()
    }
}

function Get-RelativeArtifactPath {
    param(
        [Parameter(Mandatory = $true)][string]$SessionPath,
        [Parameter(Mandatory = $true)][string]$ArtifactPath
    )
    $sessionUri = New-Object System.Uri(($SessionPath.TrimEnd('\') + '\'))
    $artifactUri = New-Object System.Uri($ArtifactPath)
    return [Uri]::UnescapeDataString($sessionUri.MakeRelativeUri($artifactUri).ToString()).Replace('/', '\')
}

function Write-JsonAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [Parameter(Mandatory = $true)]$Value
    )
    $temporary = $PathValue + ".tmp"
    $json = $Value | ConvertTo-Json -Depth 20
    [System.IO.File]::WriteAllText($temporary, $json + [Environment]::NewLine, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temporary -Destination $PathValue -Force
}

function Write-Manifest {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )
    $Manifest.updated_utc = Get-UtcString
    Write-JsonAtomic -PathValue $ManifestPath -Value $Manifest
}

function New-ManualChecks {
    param([Parameter(Mandatory = $true)][string[]]$Names)
    $ordered = [ordered]@{}
    foreach ($name in $Names) {
        $ordered[$name] = "PENDING"
    }
    return [pscustomobject]$ordered
}

function New-StageRecord {
    param([Parameter(Mandatory = $true)][string[]]$ManualChecks)
    return [pscustomobject][ordered]@{
        status = "PENDING"
        verdict = "PENDING"
        started_utc = $null
        ended_utc = $null
        duration_seconds = 0.0
        process_exit_code = $null
        trace = $null
        trace_sha256 = $null
        metrics_report = $null
        metrics_report_sha256 = $null
        auxiliary_traces = @()
        auxiliary_trace_sha256 = @()
        manual_checks = New-ManualChecks -Names $ManualChecks
        notes = ""
    }
}

function Get-WindowsEnvironmentSummary {
    $version = [Environment]::OSVersion.Version.ToString()
    $build = [Environment]::OSVersion.Version.Build.ToString()
    try {
        $os = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop
        if ($null -ne $os.Version) { $version = [string]$os.Version }
        if ($null -ne $os.BuildNumber) { $build = [string]$os.BuildNumber }
    } catch {
        # The fallback above is sufficient and avoids turning diagnostics into a blocker.
    }
    return [pscustomobject][ordered]@{
        windows_version = $version
        windows_build = $build
        architecture = [string]$env:PROCESSOR_ARCHITECTURE
        hardware_notes = $HardwareNotes
    }
}

function Get-ProfileSnapshot {
    param([Parameter(Mandatory = $true)][string]$ResolvedProfilePath)

    $profile = Get-Content -LiteralPath $ResolvedProfilePath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([int]$profile.schema_version -ne 2) {
        throw "P3-HW-01 requires workspace profile schema_version 2."
    }

    $activeSeats = @($profile.seats | Where-Object { $_.active -eq $true } | Sort-Object { [int]$_.id })
    if ($activeSeats.Count -lt 2) {
        throw "P3-HW-01 requires at least two active Seats in the profile."
    }

    $shareableLookup = @{}
    foreach ($resource in @($profile.shareable_resources)) {
        if ($null -eq $resource) { continue }
        $shareableLookup[([string]$resource.type + "|" + [string]$resource.id)] = $true
    }

    # Keep this as a bounded PowerShell array for Windows PowerShell 5.1
    # compatibility; the acceptance manifest caps ownership at 64 entries.
    $ownership = @()
    $seen = @{}
    foreach ($seat in $activeSeats) {
        foreach ($category in @("keyboard", "mouse")) {
            $property = if ($category -eq "keyboard") { "keyboards" } else { "mice" }
            foreach ($deviceId in @($seat.$property)) {
                if ([string]::IsNullOrWhiteSpace([string]$deviceId)) { continue }
                $key = $category + "|" + [string]$deviceId
                if ($shareableLookup.ContainsKey($key)) { continue }
                if ($seen.ContainsKey($key) -and [int]$seen[$key] -ne [int]$seat.id) {
                    throw "Non-shareable input '$deviceId' is assigned to multiple Seats. Fix the profile before physical acceptance."
                }
                $seen[$key] = [int]$seat.id
                $ownership += [pscustomobject][ordered]@{
                    device_id = [string]$deviceId
                    category = $category
                    seat_id = [int]$seat.id
                }
            }
        }
    }

    $firstTwo = @($activeSeats | Select-Object -First 2)
    foreach ($seat in $firstTwo) {
        $seatId = [int]$seat.id
        $keyboardCount = @($ownership | Where-Object { $_.seat_id -eq $seatId -and $_.category -eq "keyboard" }).Count
        $mouseCount = @($ownership | Where-Object { $_.seat_id -eq $seatId -and $_.category -eq "mouse" }).Count
        if ($keyboardCount -lt 1 -or $mouseCount -lt 1) {
            throw "Each of the first two active Seats needs at least one exclusive keyboard and one exclusive mouse/touchpad before P3-HW-01."
        }
    }

    $firstTwoIds = @($firstTwo | ForEach-Object { [int]$_.id })
    $nativeScope = @()
    foreach ($owned in @($ownership | Where-Object { $firstTwoIds -contains [int]$_.seat_id })) {
        $expectedPrefix = if ([string]$owned.category -eq "keyboard") { "Keyboard:" } else { "Mouse:" }
        $stableId = [string]$owned.device_id
        if (-not $stableId.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "P3-HW-01 cannot derive a native HidHide instance ID from '$stableId'; expected the stable $expectedPrefix prefix."
        }
        $instanceId = $stableId.Substring($expectedPrefix.Length)
        if ([string]::IsNullOrWhiteSpace($instanceId)) {
            throw "P3-HW-01 derived an empty native HidHide instance ID from '$stableId'."
        }
        $nativeScope += [pscustomobject][ordered]@{
            device_id = $stableId
            instance_id = $instanceId
            category = [string]$owned.category
            seat_id = [int]$owned.seat_id
        }
    }
    if ($nativeScope.Count -lt 4 -or $nativeScope.Count -gt 16) {
        throw "P3-HW-01 native HidHide scope must contain between four and sixteen exclusive keyboard/mouse identities across the first two active Seats."
    }

    return [pscustomobject]@{
        raw = $profile
        activeSeats = $activeSeats
        ownership = @($ownership)
        nativeScope = @($nativeScope)
        shareable = @($profile.shareable_resources)
    }
}

function Reset-FinalAcceptanceForStage {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)]$StageRecord
    )
    $StageRecord.verdict = "PENDING"
    $StageRecord.trace_sha256 = $null
    $StageRecord.metrics_report_sha256 = $null
    $StageRecord.auxiliary_trace_sha256 = @()
    $Manifest.manual_verdict = "PENDING"
    $Manifest.manual_verdict_note = ""
    $Manifest.manual_verdict_unix = $null
    $Manifest.evidence_valid_until_unix = $null
    $Manifest.state = "IN_PROGRESS"
}

function New-SharedCaseProfile {
    param(
        [Parameter(Mandatory = $true)]$ProfileSnapshot,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [string]$RequestedDeviceId
    )

    $clone = ($ProfileSnapshot.raw | ConvertTo-Json -Depth 20) | ConvertFrom-Json
    $activeSeats = @($clone.seats | Where-Object { $_.active -eq $true } | Sort-Object { [int]$_.id } | Select-Object -First 2)
    if ($activeSeats.Count -ne 2) {
        throw "Could not derive the two-Seat shared-case profile."
    }

    $seat1Id = [int]$activeSeats[0].id
    $candidates = @($ProfileSnapshot.ownership | Where-Object { $_.seat_id -eq $seat1Id })
    $candidate = $null
    if (-not [string]::IsNullOrWhiteSpace($RequestedDeviceId)) {
        $candidate = $candidates | Where-Object { $_.device_id -eq $RequestedDeviceId } | Select-Object -First 1
        if ($null -eq $candidate) {
            throw "-SharedTestDeviceId must name an exclusive keyboard/mouse owned by the first active Seat."
        }
    } else {
        $candidate = $candidates | Where-Object { $_.category -eq "keyboard" } | Select-Object -First 1
        if ($null -eq $candidate) {
            $candidate = $candidates | Select-Object -First 1
        }
    }
    if ($null -eq $candidate) {
        throw "No exclusive input device is available for the shared/ambiguous derived profile."
    }

    $property = if ($candidate.category -eq "keyboard") { "keyboards" } else { "mice" }
    $seat2Values = @($activeSeats[1].$property)
    if ($seat2Values -notcontains $candidate.device_id) {
        $activeSeats[1].$property = @($seat2Values + [string]$candidate.device_id)
    }

    $shareable = @($clone.shareable_resources)
    $alreadyShareable = @($shareable | Where-Object {
        [string]$_.type -eq [string]$candidate.category -and [string]$_.id -eq [string]$candidate.device_id
    }).Count -gt 0
    if (-not $alreadyShareable) {
        $clone.shareable_resources = @($shareable + [pscustomobject][ordered]@{
            type = [string]$candidate.category
            id = [string]$candidate.device_id
        })
    }

    Write-JsonAtomic -PathValue $DestinationPath -Value $clone
    return [pscustomobject][ordered]@{
        derived_profile = [System.IO.Path]::GetFileName($DestinationPath)
        sha256 = Get-Sha256Hex -PathValue $DestinationPath
        device_id = [string]$candidate.device_id
        category = [string]$candidate.category
    }
}

function Resolve-Binary {
    param([Parameter(Mandatory = $true)][string[]]$Candidates)
    foreach ($candidate in $Candidates) {
        $full = if ([System.IO.Path]::IsPathRooted($candidate)) { $candidate } else { Join-Path $script:RepoRoot $candidate }
        if (Test-Path -LiteralPath $full -PathType Leaf) {
            return (Resolve-Path -LiteralPath $full).Path
        }
    }
    throw "Required executable was not found. Tried: $($Candidates -join ', ')"
}

function Quote-ProcessArgument {
    param([Parameter(Mandatory = $true)][string]$Value)
    if ($Value -notmatch '[\s"]') { return $Value }
    if ($Value.Contains('"')) { throw "Process argument contains an unsupported quote character." }
    return '"' + $Value + '"'
}

function Invoke-OwnedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $argumentLine = (($Arguments | ForEach-Object { Quote-ProcessArgument -Value ([string]$_) }) -join ' ')
    $started = Get-Date
    $process = Start-Process -FilePath $FilePath -ArgumentList $argumentLine -PassThru
    $script:OwnedProcess = $process
    try {
        $process.WaitForExit()
        $process.Refresh()
        return [pscustomobject]@{
            exit_code = [int]$process.ExitCode
            duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
            pid = [int]$process.Id
        }
    } finally {
        try {
            if ($null -ne $process -and -not $process.HasExited) {
                & taskkill.exe /PID $process.Id /T /F 2>$null | Out-Null
            }
        } catch {
            Write-Warning "Could not fully clean the owned process tree for PID $($process.Id): $($_.Exception.Message)"
        }
        $script:OwnedProcess = $null
    }
}

function Read-ManualCheck {
    param(
        [Parameter(Mandatory = $true)][string]$Prompt,
        [switch]$AllowNotApplicable
    )
    if ($NonInteractive) { return "PENDING" }
    while ($true) {
        $suffix = if ($AllowNotApplicable) { "[P]ass/[F]ail/[N]ot applicable/[S]kip" } else { "[P]ass/[F]ail/[S]kip" }
        $answer = (Read-Host "$Prompt $suffix").Trim().ToUpperInvariant()
        switch ($answer) {
            "P" { return "PASS" }
            "PASS" { return "PASS" }
            "F" { return "FAIL" }
            "FAIL" { return "FAIL" }
            "S" { return "PENDING" }
            "SKIP" { return "PENDING" }
            "N" { if ($AllowNotApplicable) { return "NOT_APPLICABLE" } }
            "NA" { if ($AllowNotApplicable) { return "NOT_APPLICABLE" } }
        }
    }
}

function Set-ManualChecks {
    param(
        [Parameter(Mandatory = $true)][string]$StageName,
        [Parameter(Mandatory = $true)]$StageRecord
    )

    $prompts = @{}
    if ($StageName -eq "gate_a") {
        $prompts = @{
            two_keyboards_distinct = "Did both physical keyboards appear as distinct stable identities?"
            two_pointing_devices_distinct = "Did both mice / mouse+touchpad appear as distinct stable identities?"
            key_down_up_transitions = "Did held/released keys produce matching down/up behavior in the trace?"
            composite_child_removal = "If a composite HID was available, did removing one child keep the physical identity online until the final child disappeared?"
            unplug_replug_identity = "Did repeated unplug/replug return the device under the expected stable identity without a crash?"
            drop_counter_reviewed = "Did you review the visible dropped-event counter and confirm any drops are explicitly accounted for?"
        }
        $StageRecord.manual_checks.two_keyboards_distinct = Read-ManualCheck $prompts.two_keyboards_distinct
        $StageRecord.manual_checks.two_pointing_devices_distinct = Read-ManualCheck $prompts.two_pointing_devices_distinct
        $StageRecord.manual_checks.key_down_up_transitions = Read-ManualCheck $prompts.key_down_up_transitions
        $StageRecord.manual_checks.composite_child_removal = Read-ManualCheck $prompts.composite_child_removal -AllowNotApplicable
        $StageRecord.manual_checks.unplug_replug_identity = Read-ManualCheck $prompts.unplug_replug_identity
        $StageRecord.manual_checks.drop_counter_reviewed = Read-ManualCheck $prompts.drop_counter_reviewed
    } elseif ($StageName -eq "gate_b") {
        $StageRecord.manual_checks.seat1_exclusive_routing = Read-ManualCheck "Did Seat 1's exclusive keyboard/mouse change only Seat 1 routed/notification counters?"
        $StageRecord.manual_checks.seat2_exclusive_routing = Read-ManualCheck "Did Seat 2's exclusive keyboard/mouse change only Seat 2 routed/notification counters?"
        $StageRecord.manual_checks.unassigned_fails_closed = Read-ManualCheck "Did an unassigned physical device increment UnassignedDevice without routing to either Seat?"
        $StageRecord.manual_checks.shared_ambiguous_fails_closed = Read-ManualCheck "In the derived shared-case run, did the selected shared device fail closed as ambiguous?"
        $StageRecord.manual_checks.missing_target_explicit_failure = Read-ManualCheck "After closing one target window, was later input reported as a missing-target/dispatch failure rather than rerouted?"
        $StageRecord.manual_checks.trace_seat_target_reviewed = Read-ManualCheck "Did you review the Gate B trace and confirm routed device/Seat/target identity matches the profile?"
    } elseif ($StageName -eq "gate_c") {
        $StageRecord.manual_checks.two_controlled_targets_visible = Read-ManualCheck "Did exactly two HydraSeat-owned controlled target windows appear?"
        $StageRecord.manual_checks.seat1_changes_only_target1 = Read-ManualCheck "Did Seat 1 physical input change only Target 1's virtual state?"
        $StageRecord.manual_checks.seat2_changes_only_target2 = Read-ManualCheck "Did Seat 2 physical input change only Target 2's virtual state?"
        $StageRecord.manual_checks.unassigned_shared_fail_closed = Read-ManualCheck "Did unassigned/shared input remain fail-closed rather than guessed into a target?"
        $StageRecord.manual_checks.normal_windows_input_not_claimed_suppressed = Read-ManualCheck "Did you confirm this Gate C run still makes no physical/native Windows suppression claim?"
        $StageRecord.manual_checks.cleanup_no_owned_child_left = Read-ManualCheck "After stopping the host, were its two controlled targets cleaned up with no owned child left running?"
        $StageRecord.manual_checks.metrics_reviewed = Read-ManualCheck "Did you review queue/drop/latency/resource metrics and any missing-receiver-evidence warning?"
    }
}

function Show-StageInstructions {
    param([Parameter(Mandatory = $true)][string]$StageName)
    Write-Host ""
    Write-Host "=== HydraSeat P3-HW-01 $StageName ===" -ForegroundColor Cyan
    if ($StageName -eq "Gate A") {
        Write-Host "Use two keyboards and two pointing devices. Generate independent input, hold/release keys, unplug/replug one device repeatedly, and test a composite HID child if available."
        Write-Host "Keep the observer lab open for at least $MinimumSoakMinutes minute(s). The trace does not prove isolation; it proves physical observation/hot-plug behavior."
    } elseif ($StageName -eq "Gate B") {
        Write-Host "First run: exercise both Seats' exclusive devices, one unassigned device, and close one target window to observe explicit failure."
        Write-Host "Second run: use the generated shared-case profile and exercise the selected shared device; it must be ambiguous and route to neither Seat."
    } elseif ($StageName -eq "Gate C") {
        Write-Host "Exercise Seat 1 and Seat 2 devices independently against the two controlled targets. Normal Windows input is NOT suppressed in this gate."
        Write-Host "Stop with Ctrl+C or by closing a controlled target; the host must clean its owned process tree."
    }
    if (-not $NonInteractive) {
        [void](Read-Host "Press Enter when the physical setup is ready")
    }
}

function Invoke-GateA {
    param($Manifest, [string]$ManifestPath, [string]$SessionPath, [string]$InputLab)
    $record = $Manifest.stages.gate_a
    if ($record.status -eq "RECORDED" -and -not $ForceStage) {
        Write-Host "Gate A already recorded; use -ForceStage to rerun." -ForegroundColor Yellow
        return
    }
    Reset-FinalAcceptanceForStage -Manifest $Manifest -StageRecord $record
    Show-StageInstructions "Gate A"
    $tracePath = Join-Path $SessionPath "gate-a.jsonl"
    $record.status = "RUNNING"
    $record.started_utc = Get-UtcString
    $record.trace = Get-RelativeArtifactPath $SessionPath $tracePath
    Write-Manifest $Manifest $ManifestPath

    $arguments = @("--no-profile", "--trace", $tracePath)
    if ($Manifest.privacy.sensitive_key_ids_enabled) { $arguments += "--trace-sensitive-keys" }
    $result = Invoke-OwnedProcess -FilePath $InputLab -Arguments $arguments
    $record.ended_utc = Get-UtcString
    $record.duration_seconds = $result.duration_seconds
    $record.process_exit_code = $result.exit_code
    $record.status = if ($result.exit_code -eq 0 -and (Test-Path -LiteralPath $tracePath)) { "RECORDED" } else { "FAILED" }
    $record.trace_sha256 = if (Test-Path -LiteralPath $tracePath -PathType Leaf) { Get-Sha256Hex -PathValue $tracePath } else { $null }
    $record.manual_checks.soak_minimum_duration = if ($result.duration_seconds -ge ($MinimumSoakMinutes * 60)) { "PASS" } else { "FAIL" }
    Set-ManualChecks -StageName "gate_a" -StageRecord $record
    Write-Manifest $Manifest $ManifestPath
}

function Invoke-GateB {
    param(
        $Manifest,
        [string]$ManifestPath,
        [string]$SessionPath,
        [string]$InputLab,
        [string]$ResolvedProfilePath
    )
    $record = $Manifest.stages.gate_b
    if ($record.status -eq "RECORDED" -and -not $ForceStage) {
        Write-Host "Gate B already recorded; use -ForceStage to rerun." -ForegroundColor Yellow
        return
    }
    Reset-FinalAcceptanceForStage -Manifest $Manifest -StageRecord $record
    Show-StageInstructions "Gate B"
    $tracePath = Join-Path $SessionPath "gate-b-exclusive.jsonl"
    $sharedTracePath = Join-Path $SessionPath "gate-b-shared.jsonl"
    $sharedProfilePath = Join-Path $SessionPath $Manifest.profile.shared_case.derived_profile
    $record.status = "RUNNING"
    $record.started_utc = Get-UtcString
    $record.trace = Get-RelativeArtifactPath $SessionPath $tracePath
    $record.auxiliary_traces = @(Get-RelativeArtifactPath $SessionPath $sharedTracePath)
    Write-Manifest $Manifest $ManifestPath

    $commonSensitive = @()
    if ($Manifest.privacy.sensitive_key_ids_enabled) { $commonSensitive += "--trace-sensitive-keys" }
    $firstArgs = @("--profile", $ResolvedProfilePath, "--trace", $tracePath) + $commonSensitive
    $first = Invoke-OwnedProcess -FilePath $InputLab -Arguments $firstArgs

    if (-not $NonInteractive) {
        Write-Host "Starting the derived shared/ambiguous profile run for device: $($Manifest.profile.shared_case.device_id)" -ForegroundColor Cyan
        [void](Read-Host "Press Enter when ready")
    }
    $secondArgs = @("--profile", $sharedProfilePath, "--trace", $sharedTracePath) + $commonSensitive
    $second = Invoke-OwnedProcess -FilePath $InputLab -Arguments $secondArgs

    $record.ended_utc = Get-UtcString
    $record.duration_seconds = [Math]::Round($first.duration_seconds + $second.duration_seconds, 3)
    $record.process_exit_code = if ($first.exit_code -ne 0) { $first.exit_code } else { $second.exit_code }
    $evidencePresent = (Test-Path -LiteralPath $tracePath) -and (Test-Path -LiteralPath $sharedTracePath)
    $record.status = if ($record.process_exit_code -eq 0 -and $evidencePresent) { "RECORDED" } else { "FAILED" }
    $record.trace_sha256 = if (Test-Path -LiteralPath $tracePath -PathType Leaf) { Get-Sha256Hex -PathValue $tracePath } else { $null }
    $record.auxiliary_trace_sha256 = if (Test-Path -LiteralPath $sharedTracePath -PathType Leaf) { @(Get-Sha256Hex -PathValue $sharedTracePath) } else { @() }
    Set-ManualChecks -StageName "gate_b" -StageRecord $record
    Write-Manifest $Manifest $ManifestPath
}

function Invoke-GateC {
    param(
        $Manifest,
        [string]$ManifestPath,
        [string]$SessionPath,
        [string]$GateCHost,
        [string]$GateCTarget,
        [string]$ResolvedProfilePath
    )
    $record = $Manifest.stages.gate_c
    if ($record.status -eq "RECORDED" -and -not $ForceStage) {
        Write-Host "Gate C already recorded; use -ForceStage to rerun." -ForegroundColor Yellow
        return
    }
    Reset-FinalAcceptanceForStage -Manifest $Manifest -StageRecord $record
    Show-StageInstructions "Gate C"
    $tracePath = Join-Path $SessionPath "gate-c.jsonl"
    $metricsPath = Join-Path $SessionPath "gate-c-metrics.json"
    $record.status = "RUNNING"
    $record.started_utc = Get-UtcString
    $record.trace = Get-RelativeArtifactPath $SessionPath $tracePath
    $record.metrics_report = Get-RelativeArtifactPath $SessionPath $metricsPath
    Write-Manifest $Manifest $ManifestPath

    $arguments = @(
        "--profile", $ResolvedProfilePath,
        "--trace", $tracePath,
        "--metrics-report", $metricsPath,
        "--target", $GateCTarget
    )
    if ($Manifest.privacy.sensitive_key_ids_enabled) { $arguments += "--trace-sensitive-keys" }
    $result = Invoke-OwnedProcess -FilePath $GateCHost -Arguments $arguments
    $record.ended_utc = Get-UtcString
    $record.duration_seconds = $result.duration_seconds
    $record.process_exit_code = $result.exit_code
    $evidencePresent = (Test-Path -LiteralPath $tracePath) -and (Test-Path -LiteralPath $metricsPath)
    $record.status = if ($result.exit_code -eq 0 -and $evidencePresent) { "RECORDED" } else { "FAILED" }
    $record.trace_sha256 = if (Test-Path -LiteralPath $tracePath -PathType Leaf) { Get-Sha256Hex -PathValue $tracePath } else { $null }
    $record.metrics_report_sha256 = if (Test-Path -LiteralPath $metricsPath -PathType Leaf) { Get-Sha256Hex -PathValue $metricsPath } else { $null }
    Set-ManualChecks -StageName "gate_c" -StageRecord $record
    Write-Manifest $Manifest $ManifestPath
}

function Find-PythonCommand {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($null -ne $python) {
        return [pscustomobject]@{ command = $python.Source; prefix = @() }
    }
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($null -ne $py) {
        return [pscustomobject]@{ command = $py.Source; prefix = @("-3") }
    }
    throw "Python 3 is required only for the P3-HW-01 evidence summarizer. Install/enable Python or run tools/summarize_phase3_trace.py from the development environment."
}

function Invoke-Summarizer {
    param($Manifest, [string]$ManifestPath, [string]$SessionPath)
    $python = Find-PythonCommand
    $scriptPath = Join-Path $PSScriptRoot "summarize_phase3_trace.py"
    $reportPath = Join-Path $SessionPath $script:ReportName
    $args = @($python.prefix + @($scriptPath, "--manifest", $ManifestPath, "--output", $reportPath))
    & $python.command @args
    $exitCode = $LASTEXITCODE
    if (-not (Test-Path -LiteralPath $reportPath)) {
        throw "The P3-HW-01 summarizer did not produce $reportPath"
    }
    $report = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Write-Host "P3-HW-01 evidence report: $reportPath" -ForegroundColor Cyan
    Write-Host "Guarded HidHide activation verdict: $($report.native_hidhide_activation_verdict)" -ForegroundColor $(if ($report.native_hidhide_activation_verdict -eq "PASS") { "Green" } elseif ($report.native_hidhide_activation_verdict -eq "FAIL") { "Red" } else { "Yellow" })
    Write-Host "Physical isolation / release verdict: $($report.physical_isolation_release_verdict)" -ForegroundColor $(if ($report.physical_isolation_release_verdict -eq "FAIL") { "Red" } else { "Yellow" })
    Write-Host "Final verdict: $($report.final_verdict)" -ForegroundColor $(if ($report.final_verdict -eq "FAIL") { "Red" } else { "Yellow" })
    foreach ($item in @($report.errors)) { Write-Host "ERROR: $item" -ForegroundColor Red }
    foreach ($item in @($report.warnings)) { Write-Host "WARN:  $item" -ForegroundColor Yellow }
    if ($exitCode -ne 0 -and $report.final_verdict -ne "FAIL") {
        throw "Summarizer exited with unexpected code $exitCode"
    }
    return $report
}

function Set-StageVerdictsFromReport {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)]$Report
    )
    foreach ($name in @("gate_a", "gate_b", "gate_c")) {
        $computed = [string]$Report.stages.$name.verdict
        if ($computed -notin @("PENDING", "PASS", "FAIL")) {
            throw "Summarizer returned an invalid $name verdict: $computed"
        }
        $Manifest.stages.$name.verdict = $computed
    }
}

function Read-FinalManualVerdict {
    param($Manifest)
    if ($NonInteractive) { return }
    Write-Host ""
    Write-Host "A process exit code or clean automatic summary is NOT physical acceptance." -ForegroundColor Yellow
    Write-Host "Type PASS only if you personally certify the required physical observations; type FAIL for a failed run; otherwise press Enter to keep PENDING."
    $answer = (Read-Host "Manual verdict").Trim().ToUpperInvariant()
    $verdictUnix = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    if ($answer -eq "PASS") {
        $Manifest.manual_verdict = "PASS"
        $Manifest.manual_verdict_unix = $verdictUnix
        $Manifest.evidence_valid_until_unix = $verdictUnix + $script:EvidenceValiditySeconds
        $Manifest.state = "MANUAL_PASS"
    } elseif ($answer -eq "FAIL") {
        $Manifest.manual_verdict = "FAIL"
        $Manifest.manual_verdict_unix = $verdictUnix
        $Manifest.evidence_valid_until_unix = $null
        $Manifest.state = "MANUAL_FAIL"
    } else {
        $Manifest.manual_verdict = "PENDING"
        $Manifest.manual_verdict_unix = $null
        $Manifest.evidence_valid_until_unix = $null
        $Manifest.state = "READY_FOR_REVIEW"
    }
    $Manifest.manual_verdict_note = Read-Host "Optional verdict note"
}

function Test-RunnerSelfTest {
    $required = @(
        "schemas\phase3_hardware_acceptance_manifest_v1.schema.json",
        "tools\summarize_phase3_trace.py"
    )
    foreach ($relative in $required) {
        $path = Join-Path $script:RepoRoot $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Runner self-test missing required file: $relative"
        }
    }
    $schemaPath = Join-Path $script:RepoRoot "schemas\phase3_hardware_acceptance_manifest_v1.schema.json"
    $schema = Get-Content -LiteralPath $schemaPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($schema.title -ne "HydraSeat Phase 3 Hardware Acceptance Manifest v1") {
        throw "Runner self-test could not parse the manifest schema."
    }
    $selfTestRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("hydraseat-p3-hw-" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $selfTestRoot -Force | Out-Null
    try {
        $profilePath = Join-Path $selfTestRoot "workspace_config.json"
        $derivedPath = Join-Path $selfTestRoot "shared-case-profile.json"
        $fixture = [pscustomobject][ordered]@{
            schema_version = 2
            shareable_resources = @()
            seats = @(
                [pscustomobject][ordered]@{
                    id = 1; name = "Seat 1"; active = $true; target_hwnd = 0
                    displays = @(); primary_display = $null
                    keyboards = @("Keyboard:A"); mice = @("Mouse:A")
                    controllers = @(); audio_output = $null; audio_input = $null
                },
                [pscustomobject][ordered]@{
                    id = 2; name = "Seat 2"; active = $true; target_hwnd = 0
                    displays = @(); primary_display = $null
                    keyboards = @("Keyboard:B"); mice = @("Mouse:B")
                    controllers = @(); audio_output = $null; audio_input = $null
                }
            )
        }
        Write-JsonAtomic -PathValue $profilePath -Value $fixture
        $beforeHash = Get-Sha256Hex -PathValue $profilePath
        $snapshot = Get-ProfileSnapshot -ResolvedProfilePath $profilePath
        if (@($snapshot.ownership).Count -ne 4) {
            throw "Runner self-test expected four exclusive fixture input identities."
        }
        if (@($snapshot.nativeScope).Count -ne 4) {
            throw "Runner self-test expected the first two Seats to produce a four-device native HidHide scope."
        }
        $shared = New-SharedCaseProfile -ProfileSnapshot $snapshot -DestinationPath $derivedPath -RequestedDeviceId "Keyboard:A"
        $afterHash = Get-Sha256Hex -PathValue $profilePath
        if ($beforeHash -ne $afterHash) {
            throw "Runner self-test modified the source profile while deriving shared-case evidence."
        }
        if ($shared.device_id -ne "Keyboard:A" -or $shared.category -ne "keyboard") {
            throw "Runner self-test selected the wrong shared-case fixture identity."
        }
        $derived = Get-Content -LiteralPath $derivedPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $seat2 = @($derived.seats | Where-Object { [int]$_.id -eq 2 })[0]
        $sharedRows = @($derived.shareable_resources | Where-Object {
            [string]$_.type -eq "keyboard" -and [string]$_.id -eq "Keyboard:A"
        })
        if (@($seat2.keyboards) -notcontains "Keyboard:A" -or $sharedRows.Count -ne 1) {
            throw "Runner self-test did not derive the expected ambiguous shared-device profile."
        }
    } finally {
        Remove-Item -LiteralPath $selfTestRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "P3-HW-01 PowerShell runner self-test passed."
}

if ($SelfTest) {
    Test-RunnerSelfTest
    exit 0
}

if ($MinimumSoakMinutes -lt 1) {
    throw "-MinimumSoakMinutes must be at least 1."
}

if ($SensitiveKeyLogging) {
    Write-Host "WARNING: Sensitive key logging is enabled. Virtual-key identifiers may reveal typed key codes in acceptance traces." -ForegroundColor Red
    if (-not $AcknowledgeSensitiveKeyLogging) {
        if ($NonInteractive) {
            throw "Use -AcknowledgeSensitiveKeyLogging with -SensitiveKeyLogging in non-interactive mode."
        }
        $ack = Read-Host "Type I UNDERSTAND to enable sensitive key identifiers"
        if ($ack -ne "I UNDERSTAND") {
            throw "Sensitive key logging was not acknowledged."
        }
    }
}

$resolvedProfile = $null
$profileSnapshot = $null
$manifestPath = $null
$sessionPath = $null
$manifest = $null
if (-not [string]::IsNullOrWhiteSpace($Resume)) {
    $resumePath = Resolve-ExistingPath $Resume
    if (Test-Path -LiteralPath $resumePath -PathType Container) {
        $manifestPath = Join-Path $resumePath $script:ManifestName
        $sessionPath = $resumePath
    } else {
        $manifestPath = $resumePath
        $sessionPath = Split-Path -Parent $resumePath
    }
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Resume manifest was not found: $manifestPath"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $resolvedProfile = Resolve-ExistingPath ([string]$manifest.profile.source_path)
    $profileSnapshot = Get-ProfileSnapshot -ResolvedProfilePath $resolvedProfile
    $currentHash = Get-Sha256Hex -PathValue $resolvedProfile
    if ($currentHash -ne [string]$manifest.profile.sha256) {
        throw "The profile changed since this session started. Start a new acceptance session instead of mixing evidence."
    }
    if ([bool]$manifest.privacy.sensitive_key_ids_enabled) {
        Write-Host "WARNING: this resumed session was created with sensitive key-ID logging enabled." -ForegroundColor Red
    }
    Write-Host "Resuming P3-HW-01 session $($manifest.session_id)" -ForegroundColor Cyan
} else {
    $resolvedProfile = Resolve-ExistingPath $ProfilePath
    $profileSnapshot = Get-ProfileSnapshot -ResolvedProfilePath $resolvedProfile
    if ([string]::IsNullOrWhiteSpace($SessionDir)) {
        $sessionId = "p3-hw-" + [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss")
        $sessionPath = Join-Path $script:RepoRoot ("out\phase3-hardware-acceptance\" + $sessionId)
    } else {
        $sessionPath = [System.IO.Path]::GetFullPath($SessionDir)
        $sessionId = Split-Path -Leaf $sessionPath
    }
    New-Item -ItemType Directory -Path $sessionPath -Force | Out-Null
    $manifestPath = Join-Path $sessionPath $script:ManifestName
    if (Test-Path -LiteralPath $manifestPath) {
        throw "Session manifest already exists. Use -Resume or choose another -SessionDir."
    }

    $sharedProfilePath = Join-Path $sessionPath "shared-case-profile.json"
    $sharedCase = New-SharedCaseProfile -ProfileSnapshot $profileSnapshot -DestinationPath $sharedProfilePath -RequestedDeviceId $SharedTestDeviceId
    $profileHash = Get-Sha256Hex -PathValue $resolvedProfile
    $now = Get-UtcString
    $manifest = [pscustomobject][ordered]@{
        schema_version = 1
        session_id = $sessionId
        created_utc = $now
        updated_utc = $now
        state = "IN_PROGRESS"
        privacy = [pscustomobject][ordered]@{
            sensitive_key_ids_enabled = [bool]$SensitiveKeyLogging
            notice_acknowledged = [bool]((-not $SensitiveKeyLogging) -or $AcknowledgeSensitiveKeyLogging -or (-not $NonInteractive))
        }
        environment = Get-WindowsEnvironmentSummary
        profile = [pscustomobject][ordered]@{
            source_path = $resolvedProfile
            sha256 = $profileHash
            schema_version = 2
            expected_ownership = @($profileSnapshot.ownership)
            native_hidhide_scope = @($profileSnapshot.nativeScope)
            shareable_resources = @($profileSnapshot.shareable)
            shared_case = $sharedCase
        }
        stages = [pscustomobject][ordered]@{
            gate_a = New-StageRecord -ManualChecks $GateAChecks
            gate_b = New-StageRecord -ManualChecks $GateBChecks
            gate_c = New-StageRecord -ManualChecks $GateCChecks
        }
        manual_verdict = "PENDING"
        manual_verdict_note = ""
        manual_verdict_unix = $null
        evidence_valid_until_unix = $null
    }
    Write-Manifest $manifest $manifestPath
    Write-Host "Created P3-HW-01 session: $sessionPath" -ForegroundColor Cyan
    Write-Host "Profile SHA-256: $profileHash"
    Write-Host "Shared-case device: $($sharedCase.category) $($sharedCase.device_id)"
}

if ([bool]$manifest.privacy.sensitive_key_ids_enabled -ne [bool]$SensitiveKeyLogging -and -not [string]::IsNullOrWhiteSpace($Resume)) {
    Write-Host "Resume uses the manifest's original trace privacy mode: sensitive_key_ids_enabled=$($manifest.privacy.sensitive_key_ids_enabled)" -ForegroundColor Yellow
}

$buildRootFull = if ([System.IO.Path]::IsPathRooted($BuildRoot)) { $BuildRoot } else { Join-Path $script:RepoRoot $BuildRoot }

try {
    if ($Stage -eq "All" -or $Stage -eq "GateA") {
        $inputLab = Resolve-Binary @(
            (Join-Path $buildRootFull "Release\hydra_input_lab.exe"),
            (Join-Path $buildRootFull "hydra_input_lab.exe")
        )
        Invoke-GateA -Manifest $manifest -ManifestPath $manifestPath -SessionPath $sessionPath -InputLab $inputLab
    }
    if ($Stage -eq "All" -or $Stage -eq "GateB") {
        $inputLab = Resolve-Binary @(
            (Join-Path $buildRootFull "Release\hydra_input_lab.exe"),
            (Join-Path $buildRootFull "hydra_input_lab.exe")
        )
        Invoke-GateB -Manifest $manifest -ManifestPath $manifestPath -SessionPath $sessionPath -InputLab $inputLab -ResolvedProfilePath $resolvedProfile
    }
    if ($Stage -eq "All" -or $Stage -eq "GateC") {
        $gateCHost = Resolve-Binary @(
            (Join-Path $buildRootFull ("gate-c\" + $Architecture + "\hydra_gate_c_host.exe")),
            (Join-Path $buildRootFull "Release\hydra_gate_c_host.exe")
        )
        $gateCTarget = Resolve-Binary @(
            (Join-Path $buildRootFull ("gate-c\" + $Architecture + "\hydra_gate_c_target.exe")),
            (Join-Path $buildRootFull "Release\hydra_gate_c_target.exe")
        )
        Invoke-GateC -Manifest $manifest -ManifestPath $manifestPath -SessionPath $sessionPath -GateCHost $gateCHost -GateCTarget $gateCTarget -ResolvedProfilePath $resolvedProfile
    }

    if ($Stage -eq "All" -or $Stage -eq "Summarize") {
        Read-FinalManualVerdict -Manifest $manifest
        if ($manifest.manual_verdict -eq "PENDING") { $manifest.state = "READY_FOR_REVIEW" }
        Write-Manifest $manifest $manifestPath
        $firstReport = Invoke-Summarizer -Manifest $manifest -ManifestPath $manifestPath -SessionPath $sessionPath
        Set-StageVerdictsFromReport -Manifest $manifest -Report $firstReport
        Write-Manifest $manifest $manifestPath
        [void](Invoke-Summarizer -Manifest $manifest -ManifestPath $manifestPath -SessionPath $sessionPath)
    } else {
        Write-Host "Stage recorded. Resume later with:" -ForegroundColor Cyan
        Write-Host "  powershell -ExecutionPolicy Bypass -File tools\run_phase3_hardware_acceptance.ps1 -Resume `"$sessionPath`" -Stage All"
    }
} finally {
    if ($null -ne $script:OwnedProcess) {
        try {
            if (-not $script:OwnedProcess.HasExited) {
                & taskkill.exe /PID $script:OwnedProcess.Id /T /F 2>$null | Out-Null
            }
        } catch {
            Write-Warning "Final owned-process cleanup failed: $($_.Exception.Message)"
        }
    }
}
