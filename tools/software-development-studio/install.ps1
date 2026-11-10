[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Destination,
    [switch]$IncludeLumen,
    [switch]$IncludeTesting,
    [switch]$IncludeWorkflow,
    [switch]$Update,
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
$sourceRoot = Join-Path $PSScriptRoot '../../.agents/skills'
if (-not $Destination) {
    $agentsRoot = if ($env:DSH_AGENTS_HOME) { $env:DSH_AGENTS_HOME } else { Join-Path $HOME '.agents' }
    $Destination = Join-Path $agentsRoot 'skills'
}
$sourceRoot = [IO.Path]::GetFullPath($sourceRoot)
$Destination = [IO.Path]::GetFullPath($Destination)
$names = @('software-development-studio', 'software-studio-review')
if ($IncludeLumen) { $names += 'lumen-development-studio' }
if ($IncludeTesting -or $IncludeWorkflow) { $names += @('lumen-test-agent', 'lumen-script-runner', 'lumen-test-analyst') }
if ($IncludeWorkflow) {
    $names += @('lumen-development-studio', 'lumen-workflow-orchestrator', 'lumen-gated-change',
        'lumen-planner', 'lumen-architect', 'lumen-researcher', 'lumen-implementer',
        'lumen-reviewer', 'lumen-tester', 'lumen-qa', 'lumen-gatekeeper',
        'lumen-doc-writer', 'lumen-ci-manager')
}
$names = @($names | Select-Object -Unique)
# Preflight every file before creating directories or copying anything.
$copies = @()
foreach ($name in $names) {
    $bundle = Join-Path $sourceRoot $name
    if (-not (Test-Path -LiteralPath (Join-Path $bundle 'SKILL.md') -PathType Leaf)) {
        throw "Missing source bundle: $bundle"
    }
    foreach ($file in Get-ChildItem -LiteralPath $bundle -Recurse -File |
                 Where-Object { $_.FullName -notmatch '[\\/]__pycache__([\\/]|$)' -and $_.Extension -ne '.pyc' }) {
        $relative = [IO.Path]::GetRelativePath($sourceRoot, $file.FullName)
        $target = Join-Path $Destination $relative
        if ($file.FullName -eq $target) { continue }
        if ((Test-Path -LiteralPath $target) -and -not (Test-Path -LiteralPath $target -PathType Leaf)) {
            throw "Target is not a regular file: $target"
        }
        $ancestor = Split-Path -Parent $target
        while ($ancestor) {
            if ((Test-Path -LiteralPath $ancestor) -and -not (Test-Path -LiteralPath $ancestor -PathType Container)) {
                throw "Target parent is not a directory: $ancestor"
            }
            $parent = Split-Path -Parent $ancestor
            if ($parent -eq $ancestor) { break }
            $ancestor = $parent
        }
        $exists = Test-Path -LiteralPath $target -PathType Leaf
        $different = $exists -and ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $target).Hash)
        if ($different -and -not $Update) { throw "Existing file differs: $target. Review it and use -Update to replace studio-owned files." }
        if (-not $exists -or $different) { $copies += [pscustomobject]@{ Source = $file.FullName; Target = $target } }
    }
}
if ($IncludeLumen -or $IncludeWorkflow) {
    $dependencies = if ($IncludeWorkflow) { @('lumen-build-run', 'lumen-decisions') } else { @('lumen-gated-change', 'lumen-reviewer', 'lumen-build-run', 'lumen-decisions') }
    foreach ($dependency in $dependencies) {
        if (-not (Test-Path -LiteralPath (Join-Path $Destination "$dependency/SKILL.md") -PathType Leaf)) {
            throw "Lumen prerequisite missing in destination: $dependency. Install the existing Lumen adaptations first, or omit -IncludeLumen to install the reusable core only."
        }
    }
}
if ($IncludeTesting) {
    if (-not (Test-Path -LiteralPath (Join-Path $Destination 'lumen-build-run/SKILL.md') -PathType Leaf)) {
        throw 'Testing prerequisite missing in destination: lumen-build-run. Install the existing adaptation first.'
    }
}
if ($Check) {
    if ($copies.Count) { throw "Installation differs: $($copies.Count) missing or changed studio files at $Destination" }
    Write-Output "PASS: studio files match at $Destination"
    return
}
foreach ($copy in $copies) {
    if ($PSCmdlet.ShouldProcess($copy.Target, 'Install studio file')) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $copy.Target) | Out-Null
        Copy-Item -LiteralPath $copy.Source -Destination $copy.Target
    }
}
if ($WhatIfPreference) { Write-Output "Preview: $($copies.Count) studio files would change at $Destination" }
else { Write-Output "Installed/updated $($copies.Count) studio files at $Destination. No unrelated skills were removed. Verify discovery by loading software-development-studio in DSH." }
