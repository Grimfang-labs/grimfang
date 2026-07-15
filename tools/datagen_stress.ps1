<#
.SYNOPSIS
    Stress-test tools/datagen.ps1 process wait / exit-code handling.

.DESCRIPTION
    Launches 8 trivial (--games 1) datagen shards in parallel, five times in a
    row. Confirms the wrapper never false-fails and that every shard file is
    present and sized as an exact multiple of sizeof(DatagenRecord) (74).
#>
[CmdletBinding()]
param(
    [string] $Exe         = 'build/Release/stockwolf.exe',
    [int]    $Parallelism = 8,
    [int]    $Runs        = 5,
    [uint64] $Nodes       = 500,
    [string] $OutRoot     = 'tools/data/stress'
)

$ErrorActionPreference = 'Stop'
$recordSize = 74

if (-not (Test-Path -LiteralPath $Exe)) { throw "engine not found: '$Exe'" }

$failures = 0
for ($run = 1; $run -le $Runs; $run++) {
    $outDir = Join-Path $OutRoot ("run_{0}" -f $run)
    if (Test-Path -LiteralPath $outDir) {
        Remove-Item -Recurse -Force -LiteralPath $outDir
    }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    Write-Host ("=== stress run {0}/{1} ===" -f $run, $Runs) -ForegroundColor Cyan
    try {
        & "$PSScriptRoot/datagen.ps1" `
            -Exe $Exe `
            -Games 1 `
            -Parallelism $Parallelism `
            -OutDir $outDir `
            -Seed ([uint64]($run * 1000)) `
            -Nodes $Nodes

        $shards = @(Get-ChildItem -Path $outDir -Filter 'shard_*.bin' | Sort-Object Name)
        if ($shards.Count -ne $Parallelism) {
            Write-Host ("FAIL run {0}: expected {1} shards, got {2}" -f $run, $Parallelism, $shards.Count) -ForegroundColor Red
            $failures++
            continue
        }

        $bad = @()
        foreach ($s in $shards) {
            if ($s.Length -le 0 -or ($s.Length % $recordSize) -ne 0) {
                $bad += ("{0} size={1}" -f $s.Name, $s.Length)
            }
        }
        if ($bad.Count -gt 0) {
            Write-Host ("FAIL run {0}: bad shard sizes: {1}" -f $run, ($bad -join ', ')) -ForegroundColor Red
            $failures++
        } else {
            $total = ($shards | Measure-Object -Property Length -Sum).Sum
            Write-Host ("PASS run {0}: {1} shards, {2} bytes, {3} positions" -f `
                $run, $shards.Count, $total, ($total / $recordSize)) -ForegroundColor Green
        }
    } catch {
        Write-Host ("FAIL run {0} (script threw): {1}" -f $run, $_) -ForegroundColor Red
        $failures++
    }
}

Write-Host ""
if ($failures -eq 0) {
    Write-Host ("STRESS PASS: {0}/{0} runs, zero false failures" -f $Runs) -ForegroundColor Green
    exit 0
}

Write-Host ("STRESS FAIL: {0}/{1} runs failed" -f $failures, $Runs) -ForegroundColor Red
exit 1
