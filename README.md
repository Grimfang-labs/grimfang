# Grimfang

A UCI chess engine written in C++20. Formerly **StockWolf**.

Bitboard move generation, alpha-beta search with a transposition table,
principal variation search, killer/history move ordering, and a perspective
NNUE evaluation.

## Playing strength

| Release | Notes |
|---------|--------|
| **Sköll** | NNUE-launch milestone (bench `210851` at the then-default depth 6) |

Codename history lives here (and in git tags such as `v1.0-skoll`); the engine
binary and UCI `id name` stay **Grimfang** across milestones.

## Build

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# engine:  build/Release/grimfang.exe
# tests:   ctest --test-dir build -C Release
```

`grimfang bench [depth]` prints the deterministic search signature (node count,
default depth 12). The current signature is recorded in
[`tools/BENCH_SIG`](tools/BENCH_SIG); every search change updates it.

Release builds use link-time optimization when the toolchain supports it
(`-DGRIMFANG_LTO=OFF` disables it). LTO does not change the bench signature.

## Testing / SPRT harness

See [`tools/README.md`](tools/README.md) for the reproducibility guard
(`bench_check.ps1`), the engine-vs-engine SPRT gate (`sprt.ps1`), and the
reliability torture test (`reliability_check.ps1`).

## License

Released under the MIT License. See [`LICENSE`](LICENSE).
