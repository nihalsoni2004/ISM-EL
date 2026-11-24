param([switch]$LiveExperiments)
. "$PSScriptRoot/demo/common.ps1"
Set-Location -LiteralPath $Project
$root = Join-Path $Project 'build/final-validation'
[void](New-Item -ItemType Directory -Path $root -Force)
$runName = 'run-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$run = Join-Path $root $runName
[void](New-Item -ItemType Directory -Path $run)
$baseline = Join-Path $root 'protected-baseline.json'
$steps = [Collections.Generic.List[object]]::new()
$begin = [DateTime]::UtcNow
$PowerShellExe = (Get-Process -Id $PID).Path
$Frontend = Join-Path $Project 'frontend'
$helper = Join-Path $Project 'scripts/final_checks.py'

function Get-Counts([string]$Log,[string]$Kind) {
    if ($Kind -eq 'ctest' -and $Log -match '(\d+)\s+tests failed out of\s+(\d+)') {
        $failed = [int]$Matches[1]
        $total = [int]$Matches[2]
        $skipped = [regex]::Matches($Log,'(?m)^.*\*\*\*Skipped').Count
        return @{passed=$total-$failed-$skipped;failed=$failed;skipped=$skipped}
    }
    if ($Kind -in 'pytest','vitest') {
        $summary = if ($Kind -eq 'vitest') {
            ($Log -split '\r?\n' | Where-Object { $_ -match '^\s*Tests\s+' } | Select-Object -Last 1)
        } else { $Log }
        $result = @{passed=0;failed=0;skipped=0}
        foreach ($key in @($result.Keys)) {
            if ($summary -match "(\d+)\s+$key\b") { $result[$key] = [int]$Matches[1] }
        }
        if ($Kind -eq 'pytest' -and $summary -match '(\d+)\s+errors?\b') { $result.failed += [int]$Matches[1] }
        return $result
    }
    return @{passed=$null;failed=$null;skipped=$null}
}

function Invoke-Check([string]$Name,[string]$Program,[string[]]$Arguments,[string]$Folder=$Project,[string]$Kind='') {
    $log = Join-Path $run "$Name.log"
    $command = ((@($Program) + $Arguments) | ForEach-Object { "'" + $_.Replace("'","''") + "'" }) -join ' '
    $started = [DateTime]::UtcNow
    Write-Host "Running $Name"
    Push-Location -LiteralPath $Folder
    try {
        $original = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $global:LASTEXITCODE = 0
            & $Program @Arguments 2>&1 | Out-File -LiteralPath $log -Encoding UTF8
            $exitCode = $LASTEXITCODE
        } finally { $ErrorActionPreference = $original }
    } catch {
        $_ | Out-File -LiteralPath $log -Encoding UTF8 -Append
        $exitCode = 1
    } finally { Pop-Location }
    $contents = Get-Content -LiteralPath $log -Raw -Encoding UTF8
    $counts = Get-Counts $contents $Kind
    if ($Kind -and $exitCode -eq 0 -and (-not $counts.passed -or $counts.failed -or $counts.skipped)) { $exitCode = 1 }
    $steps.Add([ordered]@{
        name=$Name; command=$command; working_directory=$Folder
        started_utc=$started.ToString('o'); finished_utc=[DateTime]::UtcNow.ToString('o'); exit_code=$exitCode
        passed=$counts.passed; failed=$counts.failed; skipped=$counts.skipped; log=$log
        errors=if ($exitCode -ne 0) { (Get-Content -LiteralPath $log -Tail 15) -join [Environment]::NewLine } else { '' }
    })
    if ($exitCode -ne 0) { Write-Host "FAILED: $Name. Read $log" }
}

if (-not (Test-Path -LiteralPath $baseline)) {
    Invoke-Check 'protection-baseline' $Python @($helper,'capture',$baseline)
    Assert-Demo (Test-Path -LiteralPath $baseline -PathType Leaf) 'Historical baseline failed. No test has started.'
}
Invoke-Check 'environment' $PowerShellExe @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'demo/00-check-environment.ps1'),'-AllowOccupiedPort','-BeforeBuild')
Invoke-Check 'cmake-configure' 'cmake.exe' @('--preset','msvc-debug')
Invoke-Check 'debug-build' 'cmake.exe' @('--build','--preset','msvc-debug','--parallel','4')
Invoke-Check 'debug-ctest' 'ctest.exe' @('--preset','msvc-debug','--output-on-failure') $Project 'ctest'
Invoke-Check 'release-build' 'cmake.exe' @('--build','--preset','msvc-release','--parallel','4')
Invoke-Check 'release-ctest' 'ctest.exe' @('--preset','msvc-release','--output-on-failure') $Project 'ctest'
Invoke-Check 'python-version' $Python @('--version')
Invoke-Check 'python-pip-check' $Python @('-m','pip','check')
Invoke-Check 'backend-pytest' $Python @('-m','pytest','backend/tests','-v') $Project 'pytest'
Invoke-Check 'analysis-pytest' $Python @('-m','pytest','benchmarks/scripts','-q') $Project 'pytest'
$npmCache = Join-Path $root 'npm-lock-cache'
Invoke-Check 'frontend-npm-ci' 'npm.cmd' @('ci','--cache',$npmCache,'--prefer-offline','--fetch-retries=0','--fetch-timeout=8000') $Frontend
Invoke-Check 'frontend-vitest' 'npm.cmd' @('test') $Frontend 'vitest'
Invoke-Check 'frontend-build' 'npm.cmd' @('run','build') $Frontend
$originalArtifactRoot = $env:RAID_BROWSER_ARTIFACT_PARENT
try {
    $env:RAID_BROWSER_ARTIFACT_PARENT = $run
    Invoke-Check 'browser-smoke' 'npm.cmd' @('run','test:browser') $Frontend
} finally { $env:RAID_BROWSER_ARTIFACT_PARENT = $originalArtifactRoot }
Invoke-Check 'archive-research' $Python @('scripts/demo/audit_research.py','--output',(Join-Path $run 'research'))
if ($LiveExperiments) {
    Invoke-Check 'optional-live-trials' $PowerShellExe @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'demo/09-research-results-demo.ps1'),'-LiveTrial','-LegacyTrial','-SkipFigureRegeneration')
}
Invoke-Check 'documentation-hygiene' $Python @($helper,'hygiene')
$preservation = Join-Path $run 'preservation.json'
Invoke-Check 'historical-preservation' $Python @($helper,'verify',$baseline,$preservation)

$failed = @($steps | Where-Object exit_code -ne 0)
$status = if ($failed.Count) { 'FAILED' } else { 'PASSED' }
$summary = [ordered]@{
    status=$status; run=$runName; started_utc=$begin.ToString('o'); finished_utc=[DateTime]::UtcNow.ToString('o')
    environment=[ordered]@{powershell=$PSVersionTable.PSVersion.ToString(); os=[Environment]::OSVersion.VersionString
        python_path=$Python; cmake_generator='Visual Studio 17 2022 x64'; environment_log=(Join-Path $run 'environment.log')}
    baseline=$baseline; preservation=$preservation; optional_live_trials=[bool]$LiveExperiments; steps=@($steps.ToArray())
}
New-DemoJson (Join-Path $run 'validation-summary.json') $summary
$summary | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $root 'validation-summary.json') -Encoding UTF8
$report = @(
    '# Final validation report'
    ''
    "Validation status **$status**. UTC interval $($summary.started_utc) through $($summary.finished_utc)."
    ''
    '[Validation summary](../build/final-validation/validation-summary.json) · [Faculty guide](FACULTY_DEMO_GUIDE.md) · [Research evidence](FINAL_RESEARCH_RESULTS.md)'
    ''
    'Debug and Release use the shared msvc-debug configuration preset and their respective build and test presets.'
    ''
    '| Check | Passed | Failed | Skipped | Exit |'
    '|---|---:|---:|---:|---:|'
)
foreach ($step in $steps) {
    $label = "[$($step.name)](../build/final-validation/$runName/$($step.name).log)"
    $counts = foreach ($key in 'passed','failed','skipped') { if ($null -eq $step[$key]) { '-' } else { $step[$key] } }
    $report += "| $label | $($counts -join ' | ') | $($step.exit_code) |"
}
if (Test-Path -LiteralPath $preservation) {
    $hashes = Read-DemoJson $preservation
    $report += '',"Historical SHA-256 matched **$($hashes.matched)/$($hashes.before)** protected files. [Preservation report](../build/final-validation/$runName/preservation.json)"
}
$report += '',"Separate optional live experiment flag was $([bool]$LiveExperiments). The archived research audit ran independently."
$report | Set-Content -LiteralPath (Join-Path $Project 'docs/FINAL_VALIDATION_REPORT.md') -Encoding UTF8
Write-Host "Validation $status. Commands, logs and results: $root"
if ($failed.Count) { exit 1 }

