<#
.SYNOPSIS
    Link StockWolf training example into the bullet repo and register it.

.DESCRIPTION
    Copies/symlinks tools/training/stockwolf_net001.rs into bullet/examples/
    and patches bullet_lib Cargo.toml to register the example target.
    Idempotent - safe to re-run.
#>
[CmdletBinding()]
param(
    [string] $BulletRoot = 'C:\Users\shywolf91\Dev\StockWolf\bullet',
    [string] $StockWolfRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $BulletRoot)) {
    throw "bullet repo not found at '$BulletRoot' - clone with: git clone https://github.com/jw1912/bullet '$BulletRoot'"
}

$exampleSrc = Join-Path $StockWolfRoot 'tools\training\stockwolf_net001.rs'
$exampleDst = Join-Path $BulletRoot 'examples\stockwolf_net001.rs'

if (-not (Test-Path -LiteralPath $exampleSrc)) {
    throw "training example missing: $exampleSrc"
}

Copy-Item -LiteralPath $exampleSrc -Destination $exampleDst -Force
Write-Host "Copied example -> $exampleDst" -ForegroundColor Cyan

$cargoToml = Join-Path $BulletRoot 'crates\bullet_lib\Cargo.toml'
$content = Get-Content -LiteralPath $cargoToml -Raw
$marker = 'name = "stockwolf_net001"'
if ($content -notmatch [regex]::Escape($marker)) {
    $entry = @"

[[example]]
name = "stockwolf_net001"
path = "../../examples/stockwolf_net001.rs"

"@
    Add-Content -LiteralPath $cargoToml -Value $entry
    Write-Host "Registered stockwolf_net001 example in bullet_lib Cargo.toml" -ForegroundColor Cyan
} else {
    Write-Host "Example already registered in bullet_lib Cargo.toml" -ForegroundColor DarkGray
}

$commit = (git -C $BulletRoot rev-parse HEAD)
Write-Host "bullet commit: $commit" -ForegroundColor Green
