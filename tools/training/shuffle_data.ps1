<#
.SYNOPSIS
    Shuffle the ChessBoard training file (REQUIRED before training).

.DESCRIPTION
    DirectSequentialDataLoader reads sequentially — game-ordered data must be
    shuffled first via bullet-utils. Uses external-memory shuffle for the full
    ~10 GB file (split + interleave when mem budget is exceeded).
#>
[CmdletBinding()]
param(
    [string] $BulletRoot = 'C:\Users\shywolf91\Dev\StockWolf\bullet',
    [string] $Input  = 'tools\data\train.bulletdata',
    [string] $Output = 'tools\data\train_shuffled.bulletdata',
    [int]    $MemUsedMb = 8192
)

$ErrorActionPreference = 'Stop'
$StockWolfRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$utils = Join-Path $BulletRoot 'target\release\bullet-utils.exe'

if (-not (Test-Path -LiteralPath $utils)) {
    Write-Host "Building bullet-utils (no CUDA required)..." -ForegroundColor Cyan
    Push-Location $BulletRoot
    cargo build -r --package bullet-utils
    Pop-Location
}

$inPath  = Join-Path $StockWolfRoot $Input
$outPath = Join-Path $StockWolfRoot $Output

if (-not (Test-Path -LiteralPath $inPath)) {
    throw "input not found: $inPath"
}

if (Test-Path -LiteralPath $outPath) {
    Write-Host "Output already exists: $outPath" -ForegroundColor Yellow
    Write-Host "Delete it first if you want a fresh shuffle." -ForegroundColor Yellow
    exit 0
}

Write-Host "Shuffling $inPath -> $outPath (mem budget ${MemUsedMb} MB)" -ForegroundColor Cyan
Push-Location $BulletRoot
& $utils shuffle --input $inPath --output $outPath --mem-used-mb $MemUsedMb
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { exit $code }

$inSize  = (Get-Item $inPath).Length
$outSize = (Get-Item $outPath).Length
if ($inSize -ne $outSize) {
    throw "shuffle size mismatch: in=$inSize out=$outSize"
}
Write-Host "Shuffle OK: $outSize bytes" -ForegroundColor Green
