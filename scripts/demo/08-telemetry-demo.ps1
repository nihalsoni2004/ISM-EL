param([string]$Demo='Faculty',[switch]$ConfirmReset)
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
$session = Get-Demo $Demo
$Api = Connect-Demo $session
$before = Invoke-DemoApi GET 'telemetry'
if ($ConfirmReset) {
    $reset = Invoke-DemoApi POST 'telemetry/reset'
    Assert-Demo ($reset.observation.generation -gt $before.observation.generation) 'Telemetry reset did not open a new generation.'
    $before = Invoke-DemoApi GET 'telemetry'
}
for ($i=0; $i -lt 12; $i++) {
    $read = Invoke-DemoApi GET 'luns/lun1/blocks/0'
    Assert-Demo ($read.data_hex -ceq $Pattern) 'Workload read returned different bytes.'
}
$measured = Invoke-DemoApi GET 'telemetry?include_samples=true&event_limit=24'
$startReads = ($before.categories | Where-Object category -eq 'RAID_LOGICAL_READ').completed
$finalReads = ($measured.categories | Where-Object category -eq 'RAID_LOGICAL_READ').completed
Assert-Demo ($measured.source -ceq 'MEASURED' -and $finalReads -ge $startReads + 12) 'Measured read counters did not increase as expected.'
Write-Host 'MEASURED - engine counters and retained latency samples'
$measured.categories | Select-Object category,completed,blocks,@{n='p95_ms';e={$_.latency.p95_ms}},@{n='p99_ms';e={$_.latency.p99_ms}} | Format-Table
$jobs = @((Invoke-DemoApi GET 'rebuilds').jobs)
foreach ($job in $jobs) {
    $view = Invoke-DemoApi GET "telemetry/rebuilds/$($job.job_id)"
    Assert-Demo ($view.source -ceq 'MEASURED' -and $view.job.completed_blocks -eq $job.completed_blocks) 'Rebuild telemetry progress mismatch.'
    $view.job | Select-Object job_id,status,completed_blocks,total_blocks | Format-Table
}
$profiles = Invoke-DemoApi GET 'model/profiles'
Assert-Demo ($profiles.source -ceq 'MODELED') 'Model profiles lack the MODELED label.'
$workload = @(
    @{device=0;arrival_ms=0;operation='READ';bytes=4096;access='RANDOM'}
    @{device=0;arrival_ms=0;operation='WRITE';bytes=8192;access='SEQUENTIAL';depends_on=@(0)}
)
$models = foreach ($name in 'HDD_7200','SATA_SSD','NVME_SSD') {
    $scenario = @{devices=1;profile=$name;workload=$workload}
    $model = Invoke-DemoApi POST 'model/simulate' $scenario
    $again = Invoke-DemoApi POST 'model/simulate' $scenario
    Assert-Demo ($model.source -ceq 'MODELED' -and ($model | ConvertTo-Json -Depth 20 -Compress) -ceq ($again | ConvertTo-Json -Depth 20 -Compress)) "Non-deterministic model scenario: $name"
    Write-Host "MODELED - $name"
    $model.requests | Format-Table index,start_ms,completion_ms,queue_wait_ms | Out-Host
    $model
}
$report = Join-Path $session.Path "logs/telemetry-$([guid]::NewGuid().ToString('N')).json"
New-DemoJson $report @{measured=$measured;modeled=@($models)}
Write-Host "Telemetry evidence: $report"

