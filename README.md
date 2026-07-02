# StockWolf

A UCI chess engine written in C++20.

Bitboard move generation, alpha-beta search with a transposition table,
principal variation search, killer/history move ordering, and a tapered
material + piece-square-table evaluation.

## Build

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# engine:  build/Release/stockwolf.exe
# tests:   ctest --test-dir build -C Release
```

`stockwolf bench` prints the deterministic search signature (node count).

## Testing / SPRT harness

See [`tools/README.md`](tools/README.md) for the reproducibility guard
(`bench_check.ps1`), the engine-vs-engine SPRT gate (`sprt.ps1`), and the
reliability torture test (`reliability_check.ps1`).

## License

Released under the MIT License. See [`LICENSE`](LICENSE).
