$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$Python = Join-Path $Project '.venv/Scripts/python.exe'
$Engine = Join-Path $Project 'build/msvc-debug/Debug/raid_simulator.exe'
$ReleaseEngine = Join-Path $Project 'build/msvc-debug/Release/raid_simulator.exe'

function Assert-Demo([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function New-DemoJson([string]$Path, $Value) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew)
    try {
        $writer = New-Object IO.StreamWriter($stream, (New-Object Text.UTF8Encoding($false)))
        try { $writer.WriteLine(($Value | ConvertTo-Json -Depth 20)) } finally { $writer.Dispose() }
    } finally { $stream.Dispose() }
}

function Read-DemoJson([string]$Path) {
    Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Assert-RegularDirectory([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    Assert-Demo ($item.PSIsContainer -and -not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) "Expected a regular directory at $Path"
}

function Get-Demo([string]$Name, [switch]$Create) {
    Assert-Demo ($Name -match '^[A-Za-z0-9][A-Za-z0-9 _-]{0,47}$' -and $Name.TrimEnd() -ceq $Name) 'Use a demo name of up to 48 letters, digits, spaces, hyphens or underscores.'
    $build = Join-Path $Project 'build'
    Assert-RegularDirectory $build
    $parent = Join-Path $build 'final-demo'
    if ($Create -and -not (Test-Path -LiteralPath $parent)) {
        [void](New-Item -ItemType Directory -Path $parent)
    }
    Assert-RegularDirectory $parent
    $path = Join-Path $parent $Name
    $identity = Join-Path $path 'demo.json'
    if ($Create -and -not (Test-Path -LiteralPath $path)) {
        [void](New-Item -ItemType Directory -Path $path)
        foreach ($subdir in 'storage','logs','samples','exports') {
            [void](New-Item -ItemType Directory -Path (Join-Path $path $subdir))
        }
        New-DemoJson $identity ([ordered]@{version=1; token=[guid]::NewGuid().ToString('N'); name=$Name})
    }
    Assert-RegularDirectory $path
    Assert-Demo (Test-Path -LiteralPath $identity -PathType Leaf) "Directory exists without a demo identity. Choose a new demo name. $path"
    $record = Read-DemoJson $identity
    Assert-Demo ($record.version -eq 1 -and $record.name -eq $Name -and $record.token -match '^[0-9a-f]{32}$') 'Invalid demo identity.'
    foreach ($subdir in 'storage','logs','samples','exports') { Assert-RegularDirectory (Join-Path $path $subdir) }
    return [pscustomobject]@{Path=$path; Token=$record.token; Name=$record.name}
}

function Get-TrackedProcess($Owner) {
    $process = Get-Process -Id $Owner.pid -ErrorAction SilentlyContinue
    if ($process -and -not $process.HasExited) {
        if ($process.StartTime.ToUniversalTime().Ticks.ToString() -ceq [string]$Owner.start_ticks) { return $process }
    }
}

function Get-ServiceOwner($Session, [string]$Kind) {
    $path = Join-Path $Session.Path "$Kind.service.json"
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $owner = Read-DemoJson $path
        Assert-Demo ($owner.kind -ceq $Kind -and $owner.demo_token -ceq $Session.Token -and $owner.run -match '^[0-9a-f]{32}$') 'Service ownership record is invalid.'
        return $owner
    }
}

function Test-DemoPort([int]$Port) {
    $listener = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback), $Port
    try { $listener.Server.ExclusiveAddressUse = $true; $listener.Start(); return $true }
    catch [Net.Sockets.SocketException] { return $false }
    finally { $listener.Stop() }
}

function Invoke-DemoApi([string]$Method, [string]$Route, $Body = $null, [string]$Origin = $Api) {
    $params = @{Method=$Method; Uri="$Origin/api/$Route"; TimeoutSec=45; ErrorAction='Stop'}
    if ($null -ne $Body) { $params.ContentType='application/json'; $params.Body=ConvertTo-Json -InputObject $Body -Depth 20 -Compress }
    Invoke-RestMethod @params
}

function Confirm-Engine([string]$Origin) {
    $status = Invoke-DemoApi GET 'engine/status' -Origin $Origin
    Assert-Demo ($status.ready -and $status.catalog_accessible -and $status.engine_available) "Engine or catalog unavailable at $Origin"
    return $status
}

function Wait-DemoService($Session, $Owner) {
    $url = "http://127.0.0.1:$($Owner.port)"
    $deadline = [datetime]::UtcNow.AddSeconds(35)
    $last = ''
    while ([datetime]::UtcNow -lt $deadline -and (Get-TrackedProcess $Owner)) {
        try {
            if ($Owner.kind -ceq 'backend') { [void](Confirm-Engine $url) }
            else {
                $page = Invoke-WebRequest -UseBasicParsing -Uri "$url/" -TimeoutSec 3
                Assert-Demo ($page.Content -match '/src/main.jsx') 'Unexpected Vite response.'
                [void](Confirm-Engine $url)
            }
            return $url
        } catch { $last = $_.Exception.Message; Start-Sleep -Milliseconds 250 }
    }
    throw "$($Owner.kind) did not become ready. $last Log: $($Owner.log)"
}

function Connect-Demo($Session, [switch]$ThroughFrontend) {
    $kind = if ($ThroughFrontend) { 'frontend' } else { 'backend' }
    $owner = Get-ServiceOwner $Session $kind
    Assert-Demo ($null -ne $owner -and $null -ne (Get-TrackedProcess $owner)) "Start the tracked $kind for demo $($Session.Name) first."
    $origin = "http://127.0.0.1:$($owner.port)"
    [void](Confirm-Engine $origin)
    return $origin
}

function Start-DemoService($Session, [string]$Kind, [int]$Port, [int]$BackendPort) {
    $leasePath = Join-Path $Session.Path "$Kind.launch.lock"
    $lease = [IO.File]::Open($leasePath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try {
        $prior = Get-ServiceOwner $Session $Kind
        if ($prior -and (Get-TrackedProcess $prior)) {
            Assert-Demo ($prior.port -eq $Port -and $prior.backend_port -eq $BackendPort) "Tracked $Kind is using a different port. Stop that demo before relaunching it."
            return Wait-DemoService $Session $prior
        }
        Assert-Demo (Test-DemoPort $Port) "Port $Port is occupied. Inspect the listener or select a different backend port."
        Assert-Demo (Test-Path -LiteralPath $Python -PathType Leaf) 'The project Python 3.11 environment is missing.'
        $run = [guid]::NewGuid().ToString('N')
        $log = Join-Path $Session.Path "logs/$Kind-$run.log"
        $environment = @{
            FINAL_DEMO_ROOT=$Session.Path; FINAL_DEMO_RUN=$run; FINAL_DEMO_PORT=$Port
            FINAL_DEMO_BACKEND_PORT=$BackendPort; FINAL_DEMO_NPM=(Get-Command npm.cmd).Source
            PYTHONUNBUFFERED='1'
        }
        $previous = @{}
        foreach ($key in $environment.Keys) {
            $previous[$key] = [Environment]::GetEnvironmentVariable($key,'Process')
            [Environment]::SetEnvironmentVariable($key,[string]$environment[$key],'Process')
        }
        try {
            $process = Start-Process -FilePath $Python -ArgumentList @('-m','scripts.demo.service',$Kind) -WorkingDirectory $Project -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
        } finally {
            foreach ($key in $previous.Keys) { [Environment]::SetEnvironmentVariable($key,$previous[$key],'Process') }
        }
        $owner = [ordered]@{kind=$Kind; demo_token=$Session.Token; run=$run; pid=$process.Id; start_ticks=$process.StartTime.ToUniversalTime().Ticks.ToString(); port=$Port; backend_port=$BackendPort; log=$log}
        $history = Join-Path $Session.Path "logs/$Kind-$run.json"
        New-DemoJson $history $owner
        $current = Join-Path $Session.Path "$Kind.service.json"
        if (Test-Path -LiteralPath $current) { [IO.File]::Replace($history, $current, (Join-Path $Session.Path "logs/$Kind-previous-$run.json")) }
        else { [IO.File]::Move($history, $current) }
        try { return Wait-DemoService $Session $owner }
        catch {
            Request-DemoStop $Session $owner
            throw
        }
    } finally { $lease.Dispose() }
}

function Request-DemoStop($Session, $Owner) {
    $process = Get-TrackedProcess $Owner
    if ($process) {
        $flag = Join-Path $Session.Path "$($Owner.kind)-$($Owner.run).stop"
        if (-not (Test-Path -LiteralPath $flag)) { New-DemoJson $flag @{run=$Owner.run; demo_token=$Session.Token} }
        Assert-Demo ($process.WaitForExit(30000)) "Tracked $($Owner.kind) did not stop in time. See $($Owner.log)"
        $resultPath = Join-Path $Session.Path "logs/$($Owner.kind)-$($Owner.run).exit.json"
        Assert-Demo ((Test-Path -LiteralPath $resultPath) -and (Read-DemoJson $resultPath).exit_code -eq 0) "Tracked $($Owner.kind) shut down with an error. See $($Owner.log)"
    }
}

function Wait-DemoJob([string]$Id, [string]$State) {
    $deadline = [datetime]::UtcNow.AddSeconds(75)
    do {
        $job = Invoke-DemoApi GET "rebuilds/$Id"
        Assert-Demo ($job.status -notin 'FAILED','INTERRUPTED') "Rebuild $Id ended in $($job.status)."
        if ($job.status -ceq $State -and -not $job.worker_active) { return $job }
        Start-Sleep -Milliseconds 180
    } while ([datetime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Id to reach $State."
}

function Write-DemoBytes([string]$Path, [byte[]]$Bytes) {
    if (Test-Path -LiteralPath $Path) {
        Assert-Demo ([Convert]::ToBase64String([IO.File]::ReadAllBytes($Path)) -ceq [Convert]::ToBase64String($Bytes)) "Existing file differs. Choose a fresh demo: $Path"
        return
    }
    $stream = [IO.File]::Open($Path,[IO.FileMode]::CreateNew)
    try { $stream.Write($Bytes,0,$Bytes.Length) } finally { $stream.Dispose() }
}
$Pattern = '17b43a85' * 1024
$Zeros = '00' * 4096

