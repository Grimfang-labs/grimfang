<#
.SYNOPSIS
    Reproducibility guard: run `<exe> bench` twice and assert the node counts
    match. A deterministic search must produce an identical bench signature on
    every run; a mismatch means non-determinism (uninitialised memory, a data
    race, an unstable sort, time/thread leakage) has crept into the search.

.DESCRIPTION
    Also prints the node count so it can be copied into the merge commit message
    (the Stage 3 convention: record the new bench number whenever a feature
    that changes search behaviour is merged).

.PARAMETER Exe
    Path to the engine executable to check.

.PARAMETER Depth
    Optional bench depth override (passed through to `<exe> bench <depth>`).
    Omit to use the engine's built-in default depth.

.PARAMETER Expect
    Optional expected node count. If given, the run also fails when the bench
    signature differs from this value (use it to confirm a pure refactor did NOT
    change bench).

.EXAMPLE
    ./tools/bench_check.ps1 -Exe build/Release/grimfang.exe

.EXAMPLE
    # Assert a refactor kept the Stage 2 baseline signature unchanged.
    ./tools/bench_check.ps1 -Exe NEW.exe -Expect 14717091
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [int]    $Depth  = 0,
    [long]   $Expect = -1
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Exe)) {
    throw "engine not found: '$Exe'"
}
$exePath = (Resolve-Path -LiteralPath $Exe).Path

function Invoke-Bench {
    param([string] $path, [int] $depth)

    $cmdArgs = @('bench')
    if ($depth -gt 0) { $cmdArgs += "$depth" }

    $output = & $path @cmdArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "bench run failed (exit $LASTEXITCODE):`n$($output -join "`n")"
    }

    # Engine prints: "Nodes searched  : <N>"
    $line = $output | Select-String -Pattern 'Nodes searched\s*:\s*(\d+)' | Select-Object -First 1
    if (-not $line) {
        throw "could not find 'Nodes searched' in bench output:`n$($output -join "`n")"
    }
    return [long] $line.Matches[0].Groups[1].Value
}

Write-Host "Running bench twice for reproducibility check: $exePath" -ForegroundColor Cyan
$run1 = Invoke-Bench -path $exePath -depth $Depth
Write-Host ("  run 1 : {0} nodes" -f $run1)
$run2 = Invoke-Bench -path $exePath -depth $Depth
Write-Host ("  run 2 : {0} nodes" -f $run2)

if ($run1 -ne $run2) {
    Write-Host "FAIL: bench is non-deterministic ($run1 != $run2)." -ForegroundColor Red
    Write-Host "      The search must be reproducible. Investigate before SPRT-ing." -ForegroundColor Red
    exit 1
}

if ($Expect -ge 0 -and $run1 -ne $Expect) {
    Write-Host ("FAIL: bench {0} != expected {1}." -f $run1, $Expect) -ForegroundColor Red
    Write-Host "      A pure refactor must not change bench; a feature must -- update -Expect accordingly." -ForegroundColor Red
    exit 1
}

Write-Host ("PASS: reproducible bench signature = {0}" -f $run1) -ForegroundColor Green
Write-Host ""
Write-Host "Record this in the merge commit, e.g.:" -ForegroundColor DarkGray
Write-Host ("  feat(<rung>): <feature>  (bench {0})" -f $run1) -ForegroundColor DarkGray
exit 0
