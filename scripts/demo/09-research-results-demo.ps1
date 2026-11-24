param([switch]$OpenPlots,[switch]$LiveTrial,[switch]$LegacyTrial,[switch]$SkipFigureRegeneration)
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
$archiveArgs = @()
if ($SkipFigureRegeneration) { $archiveArgs += '--skip-figures' }
& $Python scripts/demo/audit_research.py @archiveArgs
Assert-Demo ($LASTEXITCODE -eq 0) 'Archived research validation failed.'
if ($OpenPlots) {
    foreach ($plot in 'foreground-p95.png','latency-time-tradeoff.png','adaptive-variable-load.png') {
        Start-Process -FilePath (Join-Path $Project "benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/$plot")
    }
}
if ($LiveTrial) {
    $result = & $ReleaseEngine benchmark rebuild --config scripts/demo/rebuild-live.json
    Assert-Demo ($LASTEXITCODE -eq 0) 'Short isolated scheduler experiment failed.'
    $live = $result | ConvertFrom-Json
    Assert-Demo ($live.status -ceq 'COMPLETED') 'Short live run failed validation.'
    & $Python benchmarks/scripts/analyze_rebuild.py $live.directory
    Assert-Demo ($LASTEXITCODE -eq 0) 'Short live run analysis failed.'
    $live
}
if ($LegacyTrial) {
    $config = 'scripts/demo/raid-live.json'
    $group = 'final-demo-' + [guid]::NewGuid().ToString('N').Substring(0,12)
    & $ReleaseEngine benchmark validate --config $config
    Assert-Demo ($LASTEXITCODE -eq 0) 'Benchmark configuration validation failed.'
    & $ReleaseEngine benchmark dry-run --config $config --output-dir $group
    Assert-Demo ($LASTEXITCODE -eq 0) 'Benchmark dry run failed.'
    $result = & $ReleaseEngine benchmark run --config $config --output-dir $group
    Assert-Demo ($LASTEXITCODE -eq 0) 'Short RAID benchmark failed.'
    $live = $result | ConvertFrom-Json
    Assert-Demo ($live.status -ceq 'COMPLETED') 'Short RAID run failed validation.'
    & $ReleaseEngine benchmark info $live.run_id --output-dir $group
    Assert-Demo ($LASTEXITCODE -eq 0) 'Benchmark manifest inspection failed.'
    & $Python benchmarks/scripts/analyze.py "benchmarks/results/$group"
    Assert-Demo ($LASTEXITCODE -eq 0) 'Short RAID run analysis failed.'
}

