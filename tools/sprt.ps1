<#
.SYNOPSIS
    Run an engine-vs-engine SPRT of a NEW StockWolf build against a frozen BASE
    build using fast-chess. This is the Stage 3 keep/revert gate: one change per
    test, trust the LLR, not the diff.

.DESCRIPTION
    Wraps the canonical fast-chess SPRT invocation. Defaults target short time
    control (STC) tuning at ~1800-2200 strength with a balanced opening book.

    The gate is sound only when a NEW == BASE self-match drifts toward the lower
    bound (rejects [elo0, elo1]) with measured Elo near 0. Validate that before
    trusting any feature result (see tools/README.md).

.PARAMETER New
    Path to the NEW engine executable (the candidate with the feature).

.PARAMETER Base
    Path to the BASE engine executable (the frozen baseline, e.g.
    stockwolf-base.exe). Every SPRT compares NEW against this.

.PARAMETER Tc
    Time control "moveSeconds+incrementSeconds". Default STC: 8+0.08.
    For LTC confirmation (3.6/3.7/3.8) use e.g. 40+0.4.

.PARAMETER Book
    Path to the opening book. Default: tools/books/8moves_v3.epd.

.PARAMETER BookFormat
    Opening book format passed to fast-chess (epd or pgn). Default: epd.

.PARAMETER Elo0
    SPRT H0 bound. Default 0.

.PARAMETER Elo1
    SPRT H1 bound. Default 5. Use 8 for depth-scaling features (NMP/RFP/LMR).

.PARAMETER Alpha
    SPRT type-I error. Default 0.05.

.PARAMETER Beta
    SPRT type-II error. Default 0.05.

.PARAMETER Rounds
    Maximum number of rounds (game pairs with -repeat). Default 20000.

.PARAMETER Concurrency
    Parallel games. Default 10. Keep it <= physical cores to avoid noisy timing.

.PARAMETER Hash
    Hash (MB) set on both engines via UCI. Default 16.

.PARAMETER Threads
    Threads set on both engines via UCI. Default 1 (search must stay single-thread
    for clean comparison until Lazy SMP exists).

.PARAMETER PgnOut
    Where to write game PGNs. Default tools/out.pgn.

.PARAMETER Fastchess
    Override the fast-chess executable path. Default: auto-detect on PATH, then
    tools/fastchess.exe.

.EXAMPLE
    # STC gate of a feature branch build vs the frozen baseline
    ./tools/sprt.ps1 -New build/Release/stockwolf.exe -Base tools/stockwolf-base.exe

.EXAMPLE
    # LTC confirmation for a depth-scaling feature
    ./tools/sprt.ps1 -New NEW.exe -Base tools/stockwolf-base.exe -Tc 40+0.4 -Elo1 8

.EXAMPLE
    # Harness self-match validation (must reject [0,5], Elo ~ 0)
    ./tools/sprt.ps1 -New tools/stockwolf-base.exe -Base tools/stockwolf-base.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $New,
    [Parameter(Mandatory = $true)] [string] $Base,
    [string] $Tc          = '8+0.08',
    [string] $Book        = 'tools/books/8moves_v3.epd',
    [string] $BookFormat  = 'epd',
    [double] $Elo0        = 0,
    [double] $Elo1        = 5,
    [double] $Alpha       = 0.05,
    [double] $Beta        = 0.05,
    [int]    $Rounds      = 20000,
    [int]    $Concurrency = 10,
    [int]    $Hash        = 16,
    [int]    $Threads     = 1,
    [string] $PgnOut      = 'tools/out.pgn',
    [string] $Fastchess   = ''
)

$ErrorActionPreference = 'Stop'

# --- Resolve repo root (parent of this tools/ directory) so relative defaults work
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    function Resolve-Required([string] $path, [string] $what) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "$what not found: '$path' (cwd: $((Get-Location).Path))"
        }
        return (Resolve-Path -LiteralPath $path).Path
    }

    # --- Locate fast-chess: explicit override, then PATH, then tools/.
    if ($Fastchess -ne '') {
        $fc = Resolve-Required $Fastchess 'fast-chess (-Fastchess)'
    } else {
        $cmd = Get-Command 'fastchess' -ErrorAction SilentlyContinue
        if (-not $cmd) { $cmd = Get-Command 'fast-chess' -ErrorAction SilentlyContinue }
        if ($cmd) {
            $fc = $cmd.Source
        } elseif (Test-Path -LiteralPath 'tools/fastchess.exe') {
            $fc = (Resolve-Path -LiteralPath 'tools/fastchess.exe').Path
        } else {
            throw "fast-chess not found on PATH or at tools/fastchess.exe. Install Disservin/fastchess (see tools/README.md) or pass -Fastchess <path>."
        }
    }

    $newExe  = Resolve-Required $New  'NEW engine'
    $baseExe = Resolve-Required $Base 'BASE engine'
    $bookAbs = Resolve-Required $Book "opening book (download it, see tools/README.md)"

    # --- Build the fast-chess argument vector.
    $fcArgs = @(
        '-engine', "cmd=$newExe",  'name=new',
        '-engine', "cmd=$baseExe", 'name=base',
        '-each', "tc=$Tc", 'proto=uci', "option.Hash=$Hash", "option.Threads=$Threads",
        '-rounds', "$Rounds", '-repeat', '-concurrency', "$Concurrency",
        '-openings', "file=$bookAbs", "format=$BookFormat", 'order=random',
        '-draw', 'movenumber=40', 'movecount=8', 'score=10',
        '-resign', 'movecount=3', 'score=600',
        '-sprt', "elo0=$Elo0", "elo1=$Elo1", "alpha=$Alpha", "beta=$Beta",
        '-ratinginterval', '10',
        '-recover',
        '-pgnout', "file=$PgnOut"
    )

    Write-Host '--- SPRT configuration ---------------------------------------------' -ForegroundColor Cyan
    Write-Host ("NEW         : {0}" -f $newExe)
    Write-Host ("BASE        : {0}" -f $baseExe)
    Write-Host ("TC          : {0}   book: {1} ({2})" -f $Tc, $bookAbs, $BookFormat)
    Write-Host ("SPRT        : elo0={0} elo1={1} alpha={2} beta={3}" -f $Elo0, $Elo1, $Alpha, $Beta)
    Write-Host ("rounds      : {0} (repeat)   concurrency: {1}" -f $Rounds, $Concurrency)
    Write-Host ("UCI         : Hash={0}MB Threads={1}" -f $Hash, $Threads)
    Write-Host '--- fast-chess invocation ------------------------------------------' -ForegroundColor Cyan
    Write-Host ("`"$fc`" " + ($fcArgs -join ' '))
    Write-Host '--------------------------------------------------------------------' -ForegroundColor Cyan

    & $fc @fcArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
