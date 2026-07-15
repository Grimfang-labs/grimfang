<#
.SYNOPSIS
    Teardown-race and degenerate-clock torture test for the engine.

.DESCRIPTION
    Directly exercises the stop/quit/teardown paths that produced a rare crash
    in a long self-match, plus absurd clocks that could yield a <= 0 budget:

      * Teardown torture: spawn the engine, send uci/isready/position/go, then
        tear down at a randomized point -- `stop` immediately, `stop` after
        10 ms, `quit` with no `stop`, `quit` mid-search, and `quit` right after
        `bestmove`. Assert: clean exit (code 0), exactly one `bestmove` per
        `go`, and no hang (per-iteration timeout guard).

      * Degenerate clocks: `go wtime 1 btime 1`, `go movetime 1`, and
        `go wtime 0 btime 0 winc 0 binc 0`. Assert the engine returns a legal
        `bestmove` (not 0000 from startpos) and exits promptly (no hang).

.PARAMETER Exe
    Path to the engine executable.

.PARAMETER Iterations
    Teardown-torture iterations (default 200).

.PARAMETER TimeoutMs
    Per-iteration hang guard in milliseconds (default 5000).

.EXAMPLE
    ./tools/reliability_check.ps1 -Exe build/Release/grimfang.exe
#>
[CmdletBinding()]
param(
    [string] $Exe        = 'build/Release/grimfang.exe',
    [int]    $Iterations = 200,
    [int]    $TimeoutMs  = 5000
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Exe)) { throw "engine not found: '$Exe'" }
$exePath = (Resolve-Path -LiteralPath $Exe).Path

function New-Engine {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName               = $exePath
    $psi.RedirectStandardInput  = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute        = $false
    return [System.Diagnostics.Process]::Start($psi)
}

function Count-Bestmove([string] $text) {
    return ([regex]::Matches($text, 'bestmove ')).Count
}

$rng      = [System.Random]::new(20260702)
$failures = 0

Write-Host ("--- Teardown torture: {0} iterations -----------------------------" -f $Iterations) -ForegroundColor Cyan
for ($i = 1; $i -le $Iterations; $i++) {
    $scenario = $rng.Next(0, 5)
    $p = New-Engine
    try {
        $p.StandardInput.WriteLine("uci")
        $p.StandardInput.WriteLine("isready")
        $p.StandardInput.WriteLine("position startpos")
        $p.StandardInput.WriteLine("go movetime 50")

        switch ($scenario) {
            0 { $p.StandardInput.WriteLine("stop") }                                   # stop immediately
            1 { Start-Sleep -Milliseconds 10; $p.StandardInput.WriteLine("stop") }      # stop after 10 ms
            2 { }                                                                       # quit, no stop
            3 { Start-Sleep -Milliseconds ($rng.Next(0, 40)) }                          # quit mid-search
            4 { Start-Sleep -Milliseconds 80 }                                          # quit right after bestmove
        }

        $p.StandardInput.WriteLine("quit")
        $p.StandardInput.Close()

        if (-not $p.WaitForExit($TimeoutMs)) {
            try { $p.Kill() } catch {}
            $failures++
            Write-Host ("  iter {0,4} scenario {1}: HANG (killed)" -f $i, $scenario) -ForegroundColor Red
            continue
        }

        # Process has exited; buffered output is small (well under the pipe
        # buffer) so this returns immediately.
        $out  = $p.StandardOutput.ReadToEnd()
        $code = $p.ExitCode
        $bm   = Count-Bestmove $out

        if ($code -ne 0) {
            $failures++
            Write-Host ("  iter {0,4} scenario {1}: nonzero exit {2}" -f $i, $scenario, $code) -ForegroundColor Red
        } elseif ($bm -ne 1) {
            $failures++
            Write-Host ("  iter {0,4} scenario {1}: {2} bestmove(s), expected 1" -f $i, $scenario, $bm) -ForegroundColor Red
        }
    }
    finally {
        $p.Dispose()
    }
}
Write-Host ("  teardown torture done ({0} failures so far)" -f $failures)

Write-Host "--- Degenerate clocks --------------------------------------------" -ForegroundColor Cyan
$degCmds = @(
    'go wtime 1 btime 1',
    'go movetime 1',
    'go wtime 0 btime 0 winc 0 binc 0'
)
foreach ($cmd in $degCmds) {
    $p = New-Engine
    try {
        $p.StandardInput.WriteLine("uci")
        $p.StandardInput.WriteLine("position startpos")
        $p.StandardInput.WriteLine($cmd)
        # `quit` joins the search thread, so a non-finite budget would hang here
        # and trip the timeout below.
        $p.StandardInput.WriteLine("quit")
        $p.StandardInput.Close()

        if (-not $p.WaitForExit(3000)) {
            try { $p.Kill() } catch {}
            $failures++
            Write-Host ("  '{0}': HANG (killed)" -f $cmd) -ForegroundColor Red
            continue
        }

        $out  = $p.StandardOutput.ReadToEnd()
        $code = $p.ExitCode
        $bm   = Count-Bestmove $out
        $has0000 = $out -match 'bestmove 0000'

        if ($code -ne 0) {
            $failures++
            Write-Host ("  '{0}': nonzero exit {1}" -f $cmd, $code) -ForegroundColor Red
        } elseif ($bm -ne 1) {
            $failures++
            Write-Host ("  '{0}': {1} bestmove(s), expected 1" -f $cmd, $bm) -ForegroundColor Red
        } elseif ($has0000) {
            $failures++
            Write-Host ("  '{0}': returned bestmove 0000 (no legal move)" -f $cmd) -ForegroundColor Red
        } else {
            Write-Host ("  '{0}': OK (legal bestmove, prompt exit)" -f $cmd) -ForegroundColor DarkGray
        }
    }
    finally {
        $p.Dispose()
    }
}

Write-Host "------------------------------------------------------------------" -ForegroundColor Cyan
if ($failures -eq 0) {
    Write-Host "PASS: no crashes, hangs, or bestmove anomalies." -ForegroundColor Green
    exit 0
} else {
    Write-Host ("FAIL: {0} problem(s) detected." -f $failures) -ForegroundColor Red
    exit 1
}
