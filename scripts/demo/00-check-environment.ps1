param([ValidateRange(1024,65535)][int]$Port=8000,[switch]$AllowOccupiedPort,[switch]$BeforeBuild)
. "$PSScriptRoot/common.ps1"
Set-Location -LiteralPath $Project
Assert-Demo ($PSVersionTable.PSVersion.Major -ge 5) 'Use Windows PowerShell 5.1 or PowerShell 7.'
foreach ($relative in 'CMakeLists.txt','CMakePresets.json','frontend/package-lock.json','backend/app/main.py','third_party/nlohmann/LICENSE.MIT') {
    Assert-Demo (Test-Path -LiteralPath $relative -PathType Leaf) "Missing $relative"
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
Assert-Demo (Test-Path -LiteralPath $vswhere -PathType Leaf) 'Visual Studio Installer vswhere was not found.'
$compiler = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Assert-Demo ([bool]$compiler) 'Install the MSVC x64 C++ build workload.'
$cmake = & (Get-Command cmake.exe).Source --version
Assert-Demo ($LASTEXITCODE -eq 0) 'CMake version check failed.'
$ctest = & (Get-Command ctest.exe).Source --version
Assert-Demo ($LASTEXITCODE -eq 0) 'CTest version check failed.'
$pythonVersion = & $Python -c "import sys, fastapi, uvicorn, pytest, matplotlib; assert sys.version_info[:2] == (3, 11); print(sys.version.split()[0])"
Assert-Demo ($LASTEXITCODE -eq 0) 'Expected the project Python 3.11 and its backend and analysis requirements.'
$nodeVersion = & (Get-Command node.exe).Source --version
Assert-Demo ($LASTEXITCODE -eq 0) 'Node.js check failed.'
$npmVersion = & (Get-Command npm.cmd).Source --version
Assert-Demo ($LASTEXITCODE -eq 0) 'npm check failed.'
if (-not $BeforeBuild) {
    Assert-Demo (Test-Path -LiteralPath $Engine -PathType Leaf) 'Build the msvc-debug preset first.'
    $engineVersion = & $Engine --version
    Assert-Demo ($LASTEXITCODE -eq 0) 'Simulator version check failed.'
    foreach ($dependency in 'vite/bin/vite.js','react/package.json','playwright-core/package.json') {
        Assert-Demo (Test-Path -LiteralPath "frontend/node_modules/$dependency" -PathType Leaf) 'Run npm.cmd ci from frontend first.'
    }
} else { $engineVersion = 'Build and dependency installation follow' }
$freePort = Test-DemoPort $Port
if (-not $AllowOccupiedPort) { Assert-Demo $freePort "Port $Port is occupied." }
$freeGiB = 'Unavailable'
try {
    $disk = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Project))
    if ($disk.IsReady) { $freeGiB = [Math]::Round($disk.AvailableFreeSpace / 1GB,2) }
} catch [IO.IOException] { Write-Host "Free storage could not be queried for $Project." }
[pscustomobject]@{
    project=$Project; powershell=$PSVersionTable.PSVersion.ToString(); cmake=$cmake[0]; ctest=$ctest[0]; msvc=$compiler
    engine=$engineVersion; python=$pythonVersion; node=$nodeVersion; npm=$npmVersion
    storage_free_gib=$freeGiB
    backend_port=$Port; backend_port_free=$freePort; checked='READY'
} | Format-List

