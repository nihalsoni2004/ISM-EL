param([string]$Demo='Faculty',[ValidateSet('FULL_SPEED','FIXED_THROTTLE','ADAPTIVE')][string]$Policy='ADAPTIVE',[switch]$ConfirmFailure,[switch]$PauseForInspection)
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
$session = Get-Demo $Demo
$Api = Connect-Demo $session
Assert-Demo $ConfirmFailure 'This persists a member failure in the selected demo. Add -ConfirmFailure after checking the demo name.'
Assert-Demo (Test-Path -LiteralPath (Join-Path $session.Path 'setup.json')) 'Run the demo storage setup first.'
$before = (Invoke-DemoApi GET 'raids/array1/status').health
Assert-Demo ($before -ceq 'HEALTHY') "Expected HEALTHY before failure. Current state: $before"
$previous = @((Invoke-DemoApi GET 'rebuilds').jobs | Where-Object array_id -eq 'array1')
Assert-Demo ($previous.Count -eq 0) 'This demo already has a recovery job. Use a fresh demo name to repeat failure.'
Assert-Demo ((Invoke-DemoApi GET 'luns/lun1/blocks/0').data_hex -ceq $Pattern) 'Reference block differs before failure.'
$started = [DateTime]::UtcNow
[void](Invoke-DemoApi POST 'raids/array1/fail' @{disk_id='disk11'})
$degraded = (Invoke-DemoApi GET 'raids/array1/status').health
Assert-Demo ($degraded -ceq 'DEGRADED') 'RAID 1 did not enter DEGRADED.'
$degradedBytes = (Invoke-DemoApi GET 'luns/lun1/blocks/0').data_hex
Assert-Demo ($degradedBytes -ceq $Pattern) 'Degraded read did not recover the reference bytes.'
$spare = Invoke-DemoApi POST 'spares' @{disk_id='disk13'}
Assert-Demo ($spare.available -and $spare.capacity_bytes -eq 16MB) 'Replacement spare is unavailable.'
$settings = @{policy=$Policy.ToUpperInvariant()}
if ($settings.policy -eq 'FIXED_THROTTLE') { $settings.fixed_delay_us = 12700 }
$job = Invoke-DemoApi POST 'rebuilds' @{array_id='array1';failed_disk_id='disk11';spare_disk_id='disk13';scheduling=$settings}
$jobId = $job.job_id
Assert-Demo ($job.status -ceq 'PREPARED' -and $job.scheduling.policy -ceq $settings.policy) 'Job policy was not persisted.'
Write-Host "array1: $before -> $degraded. The degraded 4096-byte read matched. $jobId uses $($settings.policy)."
[void](Invoke-DemoApi POST "rebuilds/$jobId/start" @{checkpoint_limit=2})
$paused = Wait-DemoJob $jobId 'PAUSED'
Assert-Demo ($paused.completed_blocks -eq 32 -and $paused.completed_blocks -lt $paused.total_blocks) 'Expected two durable 16-block checkpoints.'
$audited = Invoke-DemoApi POST "rebuilds/$jobId/pause"
Assert-Demo ($audited.status -ceq 'PAUSED') 'Pause status could not be confirmed.'
$decisions = Invoke-DemoApi GET "rebuilds/$jobId/scheduler"
Assert-Demo ($decisions.completed_blocks -eq $paused.completed_blocks -and @($decisions.decisions).Count -gt 0) 'Scheduler trace is missing.'
$paused | Select-Object job_id,status,completed_blocks,total_blocks,progress_percent | Format-Table
if ($PauseForInspection) {
    [void](Read-Host 'Open Rebuild Monitor and inspect the durable PAUSED checkpoint. Press Enter to resume')
}
[void](Invoke-DemoApi POST "rebuilds/$jobId/resume")
$lastBlock = -1
$deadline = [datetime]::UtcNow.AddSeconds(75)
do {
    $progress = Invoke-DemoApi GET "rebuilds/$jobId"
    Assert-Demo ($progress.completed_blocks -ge $lastBlock -and $progress.status -notin 'FAILED','INTERRUPTED') 'Rebuild progress became invalid.'
    if ($lastBlock -ne $progress.completed_blocks) { Write-Host "$($progress.status): $($progress.completed_blocks)/$($progress.total_blocks) blocks" }
    $lastBlock = $progress.completed_blocks
    if ($progress.status -ceq 'COMPLETED' -and -not $progress.worker_active) { break }
    Start-Sleep -Milliseconds 250
} while ([datetime]::UtcNow -lt $deadline)
$after = (Invoke-DemoApi GET 'raids/array1/status').health
$members = (Invoke-DemoApi GET 'raids/array1/members').members
Assert-Demo ($progress.status -ceq 'COMPLETED' -and $progress.completed_blocks -eq $progress.total_blocks -and $after -ceq 'HEALTHY') 'Verified rebuild did not finish.'
Assert-Demo (@($members | Where-Object { $_.disk_id -ceq 'disk13' -and $_.state -ceq 'ONLINE' }).Count -eq 1) 'Replacement spare was not promoted.'
Assert-Demo ((Invoke-DemoApi GET 'luns/lun1/blocks/0').data_hex -ceq $Pattern -and (Invoke-DemoApi POST 'raids/array1/verify').passed) 'Rebuilt mirror data mismatch.'
New-DemoJson (Join-Path $session.Path 'rebuild-evidence.json') ([ordered]@{
    job_id=$jobId; policy=$settings.policy; initial_health=$before; degraded_health=$degraded; final_health=$after
    degraded_read_matches=$true; paused_blocks=$paused.completed_blocks; completed_blocks=$progress.completed_blocks
    replacement='disk13'; elapsed_demo_seconds=([DateTime]::UtcNow-$started).TotalSeconds; decisions=$decisions.decisions
})
Write-Host "Verified $jobId; array1 restored to HEALTHY and disk13 is active."

