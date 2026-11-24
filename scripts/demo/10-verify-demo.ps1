param([string]$Demo='Faculty')
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
$session = Get-Demo $Demo
$Api = Connect-Demo $session
$engineStatus = Confirm-Engine $Api
$health = Invoke-WebRequest -UseBasicParsing -Uri "$Api/health" -TimeoutSec 5
Assert-Demo ($health.StatusCode -eq 200) 'Backend liveness probe did not pass.'
foreach ($id in 'array0','array1','array5','array6') {
    Assert-Demo ((Invoke-DemoApi GET "raids/$id/status").health -ceq 'HEALTHY') "$id is not healthy."
}
foreach ($id in 'pool1','pool5') {
    Assert-Demo ((Invoke-DemoApi GET "pools/$id").health -ceq 'HEALTHY') "$id is not ready."
}
foreach ($id in 'lun1','lun5','lun6') { Assert-Demo ((Invoke-DemoApi GET "luns/$id").lun_id -ceq $id) "$id is missing." }
foreach ($name in 'setup.json','rebuild-evidence.json','thin-evidence.json','file-evidence.json') {
    Assert-Demo (Test-Path -LiteralPath (Join-Path $session.Path $name) -PathType Leaf) "Missing demonstration evidence: $name"
}
$rebuilt = Read-DemoJson (Join-Path $session.Path 'rebuild-evidence.json')
$jobs = @( (Invoke-DemoApi GET 'rebuilds').jobs )
Assert-Demo ($jobs.Count -eq 1 -and $jobs[0].job_id -ceq $rebuilt.job_id -and $jobs[0].scheduling.policy -ceq $rebuilt.policy -and $jobs[0].status -ceq 'COMPLETED' -and $jobs[0].completed_blocks -eq $jobs[0].total_blocks) 'A persisted rebuild job is missing, active or failed.'
$records = @((Invoke-DemoApi GET 'luns/lun6/files/verify').files)
Assert-Demo ($records.Count -eq 3 -and @($records | Where-Object { -not $_.passed }).Count -eq 0) 'Stored file verification did not pass.'
foreach ($record in $records) {
    $export = Join-Path $session.Path "exports/$($record.name)"
    Assert-Demo ((Get-FileHash -Algorithm SHA256 -LiteralPath $export).Hash -eq $record.expected_sha256) "Export is missing or differs: $($record.name)"
}
foreach ($lba in 0,1,16) {
    Assert-Demo ((Invoke-DemoApi GET "luns/lun5/blocks/$lba").data_hex -ceq $Pattern) "Thin LUN data mismatch at $lba."
}
Assert-Demo ((Invoke-DemoApi GET 'luns/lun1/blocks/0').data_hex -ceq $Pattern) 'Recovered reference bytes changed.'
[pscustomobject]@{backend=$Api;engine_pid=$engineStatus.process_id;healthy_arrays=4;pools=2;luns=3;files_verified=$records.Count;completed_rebuild=$jobs[0].job_id;policy=$jobs[0].scheduling.policy;storage=$session.Path} | ConvertTo-Json

