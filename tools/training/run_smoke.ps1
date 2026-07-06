<#
.SYNOPSIS
    Smoke test: 1 superbatch on GPU (CUDA required).
#>
[CmdletBinding()]
param(
    [string] $BulletRoot = 'C:\Users\shywolf91\Dev\StockWolf\bullet'
)

$ErrorActionPreference = 'Stop'

if (-not $env:CUDA_PATH) {
  throw @"
CUDA_PATH is not set and nvcc was not found on PATH.

Install the NVIDIA CUDA Toolkit (winget: Nvidia.CUDA 13.3) and set:
  CUDA_PATH = C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3

Driver 596.36 supports CUDA 13.x runtime. Do NOT run training without CUDA -
CPU fallback would take days on 324M positions.
"@
}

& (Join-Path $PSScriptRoot 'setup.ps1') -BulletRoot $BulletRoot
& (Join-Path $PSScriptRoot 'shuffle_data.ps1') -BulletRoot $BulletRoot

$StockWolfRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$data = Join-Path $StockWolfRoot 'tools\data\train_shuffled.bulletdata'

Write-Host "=== Smoke run: 1 superbatch (~100M positions) ===" -ForegroundColor Cyan
Write-Host "Monitor GPU: nvidia-smi -l 1" -ForegroundColor Yellow

Push-Location $BulletRoot
$env:CARGO_TARGET_DIR = Join-Path $BulletRoot 'target'
$env:STOCKWOLF_DATA = $data
$env:STOCKWOLF_OUTPUT_DIR = 'checkpoints_smoke'
$env:STOCKWOLF_END_SB = '1'
$env:STOCKWOLF_THREADS = '4'
$env:STOCKWOLF_BATCH_QUEUE = '64'

$sw = [System.Diagnostics.Stopwatch]::StartNew()
cargo run -r --features cuda --example stockwolf_net001
$code = $LASTEXITCODE
$sw.Stop()
Pop-Location

if ($code -ne 0) { exit $code }

$positions = 16384 * 6104
$secs = $sw.Elapsed.TotalSeconds
$pps = [math]::Round($positions / $secs)
Write-Host ""
Write-Host "Smoke complete in $([math]::Round($secs,1))s" -ForegroundColor Green
Write-Host "  positions: $positions"
Write-Host "  throughput: $pps pos/sec"
Write-Host "  projected 35-sb full run: $([math]::Round(35 * $secs / 3600, 2)) hours (GPU-bound estimate)"
Write-Host "  checkpoint: $BulletRoot\checkpoints_smoke\stockwolf-net-001-1\"
