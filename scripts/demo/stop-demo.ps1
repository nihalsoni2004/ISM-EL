param([string]$Demo='Faculty')
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
$session = Get-Demo $Demo
foreach ($kind in 'frontend','backend') {
    $owner = Get-ServiceOwner $session $kind
    if ($owner -and (Get-TrackedProcess $owner)) {
        Request-DemoStop $session $owner
        Write-Host "Stopped the tracked $kind for $Demo."
    } else { Write-Host "The tracked $kind is already stopped." }
}
Write-Host "Demo storage and logs remain in $($session.Path)"

