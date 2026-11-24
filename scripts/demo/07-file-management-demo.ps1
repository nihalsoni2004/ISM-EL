param([string]$Demo='Faculty')
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
$session = Get-Demo $Demo
$Api = Connect-Demo $session
Assert-Demo (Test-Path -LiteralPath (Join-Path $session.Path 'setup.json')) 'Run the demo storage setup to generate sample files.'
$curl = (Get-Command curl.exe).Source
$files = @((Invoke-DemoApi GET 'files?lun_id=lun6').files)
$evidence = foreach ($name in 'faculty note.txt','faculty binary.bin','faculty cross-chunk.bin') {
    $source = Join-Path $session.Path "samples/$name"
    $expected = (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash
    $found = @($files | Where-Object name -ceq $name)
    Assert-Demo ($found.Count -le 1) "More than one record uses $name."
    if ($found.Count -eq 1) { $file = $found[0] }
    else {
        $form = 'file=@' + $source.Replace('\','/')
        $response = & $curl --silent --show-error --fail --form $form "$Api/api/luns/lun6/files"
        Assert-Demo ($LASTEXITCODE -eq 0) "Upload failed for $name."
        $file = $response | ConvertFrom-Json
    }
    $fileId = $file.file_id
    $detail = Invoke-DemoApi GET "files/$fileId"
    $verification = Invoke-DemoApi GET "files/$fileId/verify"
    Assert-Demo ($detail.name -ceq $name -and $detail.status -ceq 'COMPLETED' -and $detail.size_bytes -eq (Get-Item -LiteralPath $source).Length) "File record mismatch for $fileId."
    Assert-Demo ($verification.passed -and $verification.actual_sha256 -eq $expected -and $detail.sha256 -eq $expected) "Stored SHA-256 mismatch for $fileId."
    $destination = Join-Path $session.Path "exports/$name"
    if (-not (Test-Path -LiteralPath $destination)) {
        $part = "$destination.$([guid]::NewGuid().ToString('N')).part"
        & $curl --silent --show-error --fail --output $part "$Api/api/files/$fileId/download"
        Assert-Demo ($LASTEXITCODE -eq 0) "Download failed for $fileId."
        Assert-Demo ((Get-FileHash -Algorithm SHA256 -LiteralPath $part).Hash -ceq $expected) 'Temporary export checksum mismatch.'
        [IO.File]::Move($part,$destination)
    }
    $match = [Convert]::ToBase64String([IO.File]::ReadAllBytes($source)) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($destination))
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    Assert-Demo ($match -and $actual -ceq $expected) "Exported bytes differ for $fileId."
    [pscustomobject]@{file_id=$fileId;name=$name;size_bytes=$detail.size_bytes;start_lba=$detail.start_lba
        block_count=$detail.block_count;sha256=$actual;byte_equal=$match}
}
try {
    [void](Invoke-DemoApi GET 'files/file999999999')
    throw 'Unexpected record for the missing-file demonstration.'
} catch {
    if (-not $_.Exception.PSObject.Properties['Response'] -or [int]$_.Exception.Response.StatusCode -ne 404) { throw }
}
$marker = Join-Path $session.Path 'file-evidence.json'
if (-not (Test-Path -LiteralPath $marker)) { New-DemoJson $marker @($evidence) }
$evidence | Format-Table file_id,name,size_bytes,start_lba,block_count,byte_equal
$evidence | Format-List name,sha256
Write-Host "Verified exports: $($session.Path)\exports"

