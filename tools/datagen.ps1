<#
.SYNOPSIS
    Launch parallel self-play datagen shards for NNUE training data.

.DESCRIPTION
    Spawns P independent single-threaded engine processes (each with its own
    private TT/killers/history globals), each writing a binary shard of
    DatagenRecord structs. Mirrors the multi-process concurrency model used
    by sprt.ps1 / fastchess rather than threading inside one engine.

.PARAMETER Exe
    Path to the engine executable.

.PARAMETER Games
    Total games to generate, split evenly across shards.

.PARAMETER OutDir
    Directory for shard files (default tools/data).

.PARAMETER Parallelism
    Number of concurrent engine processes (default: logical processor count).

.PARAMETER Seed
    Base seed; shard i uses Seed + i.

.PARAMETER Nodes
    Node cap per move (default 5000).

.PARAMETER RandomPlies
    Random opening plies (default 8).

.PARAMETER Progress
    Print a progress line every N completed games per shard (default 1000; 0 = off).

.EXAMPLE
    ./tools/datagen.ps1 -Exe build/Release/grimfang.exe -Games 1000
#>
[CmdletBinding()]
param(
    [string] $Exe           = 'build/Release/grimfang.exe',
    [int]    $Games         = 100,
    [string] $OutDir         = 'tools/data',
    [int]    $Parallelism   = 0,
    [uint64] $Seed          = 1,
    [uint64] $Nodes         = 5000,
    [int]    $RandomPlies   = 8,
    [int]    $ResignScore   = 1500,
    [int]    $ResignPlies   = 4,
    [int]    $Progress      = 1000
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Exe)) { throw "engine not found: '$Exe'" }
$exePath = (Resolve-Path -LiteralPath $Exe).Path

if ($Parallelism -le 0) {
    $Parallelism = [Environment]::ProcessorCount
}
if ($Parallelism -lt 1) { $Parallelism = 1 }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$recordSize = 74   # sizeof(DatagenRecord), must match C++ static_assert
$gamesPerShard = [Math]::Max(1, [Math]::Ceiling($Games / [double]$Parallelism))

Write-Host ("Launching {0} datagen shard(s), ~{1} games each, nodes={2}" -f $Parallelism, $gamesPerShard, $Nodes) -ForegroundColor Cyan

# Quote a single argv token for ProcessStartInfo.Arguments (Win32 command line).
function Quote-WinArg([string] $arg) {
    if ($arg -notmatch '[\s"]') { return $arg }
    return '"' + ($arg -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

# Use System.Diagnostics.Process (not Start-Process -PassThru): PassThru handles
# often leave .ExitCode as $null, and in PowerShell `$null -ne 0` is $true, which
# produced false "failed with exit code" throws for shards that were still
# running (or had already succeeded). WaitForExit() on a real Process object
# populates ExitCode reliably; only then do we inspect it.
$workDir = (Get-Location).Path
$procs = @()
for ($i = 0; $i -lt $Parallelism; $i++) {
    $shardPath = Join-Path $OutDir ("shard_{0}.bin" -f $i)
    $shardSeed = $Seed + $i
    $argList = @(
        'datagen',
        '--games', "$gamesPerShard",
        '--out', $shardPath,
        '--seed', "$shardSeed",
        '--nodes', "$Nodes",
        '--random-plies', "$RandomPlies",
        '--resign-score', "$ResignScore",
        '--resign-plies', "$ResignPlies",
        '--progress', "$Progress"
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $exePath
    $psi.Arguments = ($argList | ForEach-Object { Quote-WinArg $_ }) -join ' '
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $false

    $proc = [System.Diagnostics.Process]::new()
    $proc.StartInfo = $psi
    $proc.EnableRaisingEvents = $false
    if (-not $proc.Start()) {
        throw ("failed to start datagen shard {0}" -f $i)
    }
    $procs += $proc
}

# Phase 1: block until every shard process has fully terminated.
foreach ($proc in $procs) {
    $proc.WaitForExit()
}

# Phase 2: only after every wait has completed, inspect exit codes.
foreach ($proc in $procs) {
    if (-not $proc.HasExited) {
        throw ("datagen shard pid {0} still running after wait" -f $proc.Id)
    }
    $code = $proc.ExitCode
    if ($code -ne 0) {
        throw ("datagen shard pid {0} failed with exit code {1}" -f $proc.Id, $code)
    }
    $proc.Dispose()
}

$totalBytes = 0L
Get-ChildItem -Path $OutDir -Filter 'shard_*.bin' | ForEach-Object {
    $totalBytes += $_.Length
}

if ($totalBytes % $recordSize -ne 0) {
    throw "total shard bytes ($totalBytes) is not a multiple of record size ($recordSize)"
}

$totalPositions = $totalBytes / $recordSize
Write-Host ("Done: {0} positions written ({1} bytes, record size {2})" -f $totalPositions, $totalBytes, $recordSize) -ForegroundColor Green
