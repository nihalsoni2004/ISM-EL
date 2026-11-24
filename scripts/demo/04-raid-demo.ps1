param([string]$Demo='Faculty')
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
$session = Get-Demo $Demo
$Api = Connect-Demo $session
$owners = @{}
foreach ($entry in (Invoke-DemoApi GET 'raids').arrays) {
    foreach ($member in $entry.members) { $owners[$member] = $entry.array_id }
}
foreach ($spare in (Invoke-DemoApi GET 'spares').spares) {
    if (-not $owners.ContainsKey($spare.disk_id)) { $owners[$spare.disk_id] = $spare.state }
}
(Invoke-DemoApi GET 'disks').disks |
    Select-Object disk_id,capacity_bytes,block_size,block_count,@{n='catalog_owner';e={
        if ($owners.ContainsKey($_.disk_id)) { $owners[$_.disk_id] } else { 'Unassigned in active membership' }
    }} | Format-Table
foreach ($id in 'array0','array1','array5','array6') {
    $array = Invoke-DemoApi GET "raids/$id"
    $members = (Invoke-DemoApi GET "raids/$id/members").members
    $health = Invoke-DemoApi GET "raids/$id/status"
    Assert-Demo ($health.health -ceq 'HEALTHY' -and (Invoke-DemoApi POST "raids/$id/verify").passed) "Verification failed for $id."
    $array | Select-Object array_id,raid_level,capacity_bytes,stripe_unit_blocks,layout,health | Format-List
    $members | Format-Table disk_id,state,available
    $positions = foreach ($lba in 0,1,2,6) {
        $mapping = Invoke-DemoApi GET "raids/$id/map/$lba"
        $roles = @($mapping.locations | ForEach-Object role)
        if ($id -eq 'array5') { Assert-Demo ($roles -contains 'p') 'Missing RAID 5 parity.' }
        if ($id -eq 'array6') { Assert-Demo ($roles -contains 'p' -and $roles -contains 'q') 'Missing RAID 6 P/Q.' }
        foreach ($location in $mapping.locations) {
            [pscustomobject]@{logical_lba=$lba;role=$location.role;disk_id=$location.disk_id;physical_lba=$location.physical_lba}
        }
    }
    $positions | Format-Table
    if ($id -in 'array5','array6') {
        Assert-Demo (@($positions | Where-Object role -eq 'p' | Select-Object -ExpandProperty disk_id -Unique).Count -gt 1) "Parity rotation was not demonstrated for $id."
    }
}

