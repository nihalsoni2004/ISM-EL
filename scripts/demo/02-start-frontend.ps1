param([string]$Demo='Faculty')
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
$session = Get-Demo $Demo
$apiOrigin = Connect-Demo $session
$backendPort = ([uri]$apiOrigin).Port
Assert-Demo (Test-Path -LiteralPath 'frontend/node_modules/vite/bin/vite.js' -PathType Leaf) 'Run npm.cmd ci from frontend first.'
$url = Start-DemoService $session 'frontend' 5173 $backendPort
[pscustomobject]@{Dashboard=$url; Proxy=$apiOrigin; Logs=(Join-Path $session.Path 'logs')} | Format-List

