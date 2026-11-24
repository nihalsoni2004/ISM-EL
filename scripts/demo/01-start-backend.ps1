param([string]$Demo='Faculty',[ValidateRange(1024,65535)][int]$Port=8000)
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
Assert-Demo (Test-Path -LiteralPath $Engine -PathType Leaf) 'Build the msvc-debug preset before starting the backend.'
$session = Get-Demo $Demo -Create
$url = Start-DemoService $session 'backend' $Port $Port
$ready = Confirm-Engine $url
[pscustomobject]@{
    Api=$url; OpenApi="$url/docs"; EnginePid=$ready.process_id
    Storage=(Join-Path $session.Path 'storage'); Logs=(Join-Path $session.Path 'logs')
} | Format-List

