param([string]$Demo='Faculty')
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
$session = Get-Demo $Demo
$Api = Connect-Demo $session
$marker = Join-Path $session.Path 'thin-evidence.json'
if (Test-Path -LiteralPath $marker) {
    $prior = Read-DemoJson $marker
    foreach ($lba in 0,1,16) {
        $read = Invoke-DemoApi GET "luns/lun5/blocks/$lba"
        Assert-Demo ($read.data_hex -ceq $Pattern -and $read.mapped) "Persisted block mismatch at $lba."
    }
    $prior | Format-List
    return
}
$lun = Invoke-DemoApi GET 'luns/lun5'
$initial = Invoke-DemoApi GET 'pools/pool5'
Assert-Demo ($lun.allocated_chunks -eq 0 -and $initial.allocated_bytes -eq 0 -and $lun.capacity_bytes -gt $initial.physical_capacity_bytes) 'Start with an unallocated fixture and run thin provisioning before the imports.'
$read = Invoke-DemoApi GET 'luns/lun5/blocks/0'
Assert-Demo (-not $read.mapped -and $read.data_hex -ceq $Zeros) 'Unallocated read was not zero.'
$write1 = Invoke-DemoApi PUT 'luns/lun5/blocks/0' @{data_hex=$Pattern}
$after1 = Invoke-DemoApi GET 'pools/pool5'
$write2 = Invoke-DemoApi PUT 'luns/lun5/blocks/1' @{data_hex=$Pattern}
$after2 = Invoke-DemoApi GET 'pools/pool5'
$write3 = Invoke-DemoApi PUT 'luns/lun5/blocks/16' @{data_hex=$Pattern}
$after3 = Invoke-DemoApi GET 'pools/pool5'
Assert-Demo ($after1.allocated_bytes -eq 64KB -and $after2.allocated_bytes -eq 64KB -and $after3.allocated_bytes -eq 128KB) 'Physical allocation was not 0 -> 64 -> 64 -> 128 KiB.'
Assert-Demo ($write1.new_chunks -eq 1 -and $write2.new_chunks -eq 0 -and $write3.new_chunks -eq 1 -and $write3.allocated_chunks -eq 2) 'Unexpected allocation counts.'
$mappings = foreach ($lba in 0,1,16) { Invoke-DemoApi GET "luns/lun5/map/$lba" }
Assert-Demo ($mappings[0].physical_chunk -eq $mappings[1].physical_chunk -and $mappings[0].physical_chunk -ne $mappings[2].physical_chunk) 'LUN mapping does not reflect chunk allocation.'
$evidence = [ordered]@{
    lun=$lun.lun_id; logical_bytes=$lun.capacity_bytes; physical_pool_bytes=$initial.physical_capacity_bytes
    pool_logical_bytes=$after3.provisioned_bytes; pool_overprovisioning_ratio=$after3.provisioned_bytes / $initial.physical_capacity_bytes
    zero_read=$true; allocation_bytes=@(0,$after1.allocated_bytes,$after2.allocated_bytes,$after3.allocated_bytes)
    mappings=$mappings
}
New-DemoJson $marker $evidence
[pscustomobject]$evidence | Format-List
$mappings | Format-Table
Write-Host 'Allocated bytes describe RAID address space.'

