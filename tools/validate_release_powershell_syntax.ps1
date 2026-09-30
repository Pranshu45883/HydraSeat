#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$RepositoryRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent $PSScriptRoot
}
$root = [IO.Path]::GetFullPath($RepositoryRoot)
$scripts = @(
    (Join-Path $root "tools\sign_release_artifacts.ps1"),
    (Join-Path $root "tools\install_hydraseat.ps1")
)

foreach ($script in $scripts) {
    if (-not (Test-Path -LiteralPath $script -PathType Leaf)) {
        throw "Missing release PowerShell script: $script"
    }
    $tokens = $null
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile(
        $script,
        [ref]$tokens,
        [ref]$errors) | Out-Null
    if ($errors.Count -ne 0) {
        $messages = @($errors | ForEach-Object { $_.Message }) -join "; "
        throw "PowerShell syntax validation failed for ${script}: $messages"
    }
}

Write-Output "Release PowerShell syntax validation passed."
