<#
.SYNOPSIS
    Full training run: 35 superbatches (~10.8 passes over 324M positions).

.DESCRIPTION
    Does NOT auto-start - run manually when ready. Requires CUDA + shuffled data.
#>
[CmdletBinding()]
param(
    [string] $BulletRoot = 'C:\Users\shywolf91\Dev\Grimfang\bullet',
    [int]    $EndSuperbatch = 35
)

$ErrorActionPreference = 'Stop'

if (-not $env:CUDA_PATH) {
    throw "CUDA_PATH not set - install CUDA Toolkit first (see run_smoke.ps1 message)."
}

& (Join-Path $PSScriptRoot 'setup.ps1') -BulletRoot $BulletRoot

$shuffled = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'tools\data\train_shuffled.bulletdata'
if (-not (Test-Path -LiteralPath $shuffled)) {
    Write-Host "Shuffled data missing - running shuffle_data.ps1 first..." -ForegroundColor Yellow
    & (Join-Path $PSScriptRoot 'shuffle_data.ps1') -BulletRoot $BulletRoot
}

Write-Host "=== Full training: superbatches 1..$EndSuperbatch ===" -ForegroundColor Cyan
Write-Host "Command:" -ForegroundColor Yellow
Write-Host @"

  cd '$BulletRoot'
  `$env:CUDA_PATH = '<your CUDA toolkit path>'
  `$env:CARGO_TARGET_DIR = '$BulletRoot\target'
  `$env:GRIMFANG_DATA = '$shuffled'
  `$env:GRIMFANG_OUTPUT_DIR = 'checkpoints'
  `$env:GRIMFANG_END_SB = '$EndSuperbatch'
  cargo run -r --features cuda --example grimfang_net001

"@ -ForegroundColor White

Write-Host "After training, copy final quantised net:" -ForegroundColor Yellow
Write-Host "  checkpoints/grimfang-net-001-$EndSuperbatch/quantised.bin -> grimfang-net-001.bin" -ForegroundColor White
