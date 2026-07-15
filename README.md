# Grimfang

A UCI chess engine written in C++20. Formerly **StockWolf**.

Bitboard move generation, alpha-beta search with a transposition table,
principal variation search, killer/history move ordering, and a perspective
NNUE evaluation.

## Playing strength

| Release | Notes |
|---------|--------|
| **Sköll** | NNUE-launch milestone (bench `210851`) |

Codename history lives here (and in git tags such as `v1.0-skoll`); the engine
binary and UCI `id name` stay **Grimfang** across milestones.

## Build

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# engine:  build/Release/grimfang.exe
# tests:   ctest --test-dir build -C Release
```

`grimfang bench` prints the deterministic search signature (node count).

## Testing / SPRT harness

See [`tools/README.md`](tools/README.md) for the reproducibility guard
(`bench_check.ps1`), the engine-vs-engine SPRT gate (`sprt.ps1`), and the
reliability torture test (`reliability_check.ps1`).

## License

Released under the MIT License. See [`LICENSE`](LICENSE).
