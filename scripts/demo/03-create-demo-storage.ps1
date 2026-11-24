param([string]$Demo='Faculty')
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
$session = Get-Demo $Demo
$Api = Connect-Demo $session
$marker = Join-Path $session.Path 'setup.json'
if (Test-Path -LiteralPath $marker) {
    $saved = Read-DemoJson $marker
    Assert-Demo ($saved.demo_token -ceq $session.Token) 'Unexpected setup identity.'
    foreach ($id in $saved.arrays) { [void](Invoke-DemoApi GET "raids/$id") }
    foreach ($id in $saved.luns) { [void](Invoke-DemoApi GET "luns/$id") }
    Write-Host 'Prepared catalog found. Existing resources and artifacts were preserved.'
    return
}
foreach ($group in 'disks','pools','luns') {
    Assert-Demo (@((Invoke-DemoApi GET $group).$group).Count -eq 0) 'Partial or pre-existing setup found. Choose a fresh demo name.'
}
Assert-Demo (@((Invoke-DemoApi GET 'raids').arrays).Count -eq 0) 'Demo array registry must be empty.'
$layout = @(
    @{id='array0'; level=0; size=2; members=@('disk1','disk2')}
    @{id='array1'; level=1; size=16; members=@('disk11','disk12')}
    @{id='array5'; level=5; size=4; members=@('disk51','disk52','disk53','disk54')}
    @{id='array6'; level=6; size=4; members=@('disk61','disk62','disk63','disk64')}
)
foreach ($entry in $layout) {
    foreach ($id in $entry.members) {
        $disk = Invoke-DemoApi POST 'disks' @{disk_id=$id; capacity_mib=$entry.size}
        Assert-Demo ($disk.disk_id -ceq $id -and $disk.capacity_bytes -eq $entry.size * 1MB -and $disk.block_size -eq 4096) "Unexpected geometry for $id"
    }
    $array = Invoke-DemoApi POST 'raids' @{array_id=$entry.id; raid_level=$entry.level; members=$entry.members; stripe_unit_blocks=1}
    Assert-Demo ($array.health -ceq 'HEALTHY' -and $array.raid_level -eq $entry.level -and ($array.members -join ',') -ceq ($entry.members -join ',')) "Invalid geometry for $($entry.id)"
}
$spare = Invoke-DemoApi POST disks @{disk_id='disk13'; capacity_mib=16}
Assert-Demo ($spare.capacity_bytes -eq 16MB) 'Replacement candidate has incorrect capacity.'
foreach ($n in 1,5) {
    $pool = Invoke-DemoApi POST pools @{pool_id="pool$n";array_id="array$n";chunk_blocks=16;max_ratio=4}
    Assert-Demo ($pool.array_id -ceq "array$n" -and $pool.chunk_bytes -eq 64KB) "Invalid pool$n"
}
foreach ($definition in @(
    @{lun_id='lun1';pool_id='pool1';capacity_mib=2;mode='raw'}
    @{lun_id='lun5';pool_id='pool5';capacity_mib=24;mode='raw'}
    @{lun_id='lun6';pool_id='pool5';capacity_mib=2;mode='file'}
)) {
    $lun = Invoke-DemoApi POST luns $definition
    Assert-Demo ($lun.lun_id -ceq $definition.lun_id -and $lun.mode -ceq $definition.mode -and $lun.capacity_bytes -eq $definition.capacity_mib * 1MB) 'Unexpected LUN configuration.'
}
[void](Invoke-DemoApi PUT 'luns/lun1/blocks/0' @{data_hex=$Pattern})
Assert-Demo ((Invoke-DemoApi GET 'luns/lun1/blocks/0').data_hex -ceq $Pattern) 'Recovery reference block mismatch.'
$sampleDir = Join-Path $session.Path 'samples'
Write-DemoBytes (Join-Path $sampleDir 'faculty note.txt') ([Text.Encoding]::UTF8.GetBytes("CS374TFD storage simulator demo`r`n"))
foreach ($sample in @(@{name='faculty binary.bin';size=4096},@{name='faculty cross-chunk.bin';size=73729})) {
    $bytes = New-Object byte[] $sample.size
    for ($i=0; $i -lt $bytes.Length; $i++) { $bytes[$i] = ($i * 31 + 7) % 256 }
    Write-DemoBytes (Join-Path $sampleDir $sample.name) $bytes
}
New-DemoJson $marker @{demo_token=$session.Token; arrays=@('array0','array1','array5','array6'); luns=@('lun1','lun5','lun6')}
(Invoke-DemoApi GET 'raids').arrays | Format-Table array_id,raid_level,capacity_bytes,health
(Invoke-DemoApi GET 'luns').luns | Format-Table lun_id,pool_id,capacity_bytes,mode
Write-Host "Sample files: $sampleDir"

