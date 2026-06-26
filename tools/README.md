# StockWolf testing harness (Stage 3)

This directory holds the SPRT gate for Stage 3 — the strength ladder. Every Elo
claim is decided by **real engine-vs-engine games** (NEW build vs a frozen BASE
build), not by intuition or by the diff.

## The one rule that governs everything

**One change per SPRT test.** Implement exactly one feature on a branch, SPRT it
against the frozen baseline, then keep (merge) or discard. Never bundle two
features in a single test — you lose the ability to attribute the Elo, and a
regression in one can hide a gain in the other.

Corollaries:

- **SPRT is the gate, not your intuition.** A change that "should" help often
  doesn't. Trust the LLR, not the diff.
- **Bench discipline.** Any change that alters search behaviour changes the bench
  node count — that's expected; record the new number in the merge commit. A
  *pure refactor* must **not** change bench; if it does, you introduced a bug.
- **Perft stays green; movegen is frozen.** If a TT/ordering change touches
  movegen, you crossed a boundary.
- **STC to iterate, LTC to confirm.** Tune fast at short TC; for depth-scaling
  features (NMP, RFP, LMR) re-confirm once at a longer TC.

## One-time setup (manual — the agent can't download/run these)

1. **Install fast-chess** (Disservin/fastchess — modern, pentanomial, actively
   maintained). Put `fastchess.exe` on your `PATH` or drop it in `tools/`.
   cutechess-cli is the fallback if needed.
   - Releases: <https://github.com/Disservin/fastchess/releases>
2. **Get a balanced opening book.** At ~1800–2200 strength use a *balanced* book
   such as `8moves_v3.epd` from the official Stockfish books repo, placed at
   `tools/books/8moves_v3.epd`.
   - Books: <https://github.com/official-stockfish/books>
   - Keep unbalanced books (UHO_*, Pohl.epd) for *later* when the engine is much
     stronger — they over-separate weak engines and bias results now.
3. **Freeze a baseline binary.** Build Release from current `main` and copy it
   aside as `tools/stockwolf-base.exe`. Every SPRT compares NEW vs this frozen
   baseline. When a feature passes and merges, rebuild the baseline from the new
   `main` so the baseline advances one rung.

`tools/fastchess.exe`, `tools/stockwolf-base.exe`, `tools/books/*`, and
`tools/out.pgn` are intentionally git-ignored (see `tools/.gitignore`).

## Building NEW

Release build of the engine (the candidate). From the repo root, on Windows:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target stockwolf
# -> build/Release/stockwolf.exe   (single-config generators: build/stockwolf.exe)
```

The same `bench` signature must come out of an optimized build; debug builds
have a different (still reproducible) node count, so always SPRT/compare Release
builds.

## Reproducibility guard — run this before every SPRT

```powershell
./tools/bench_check.ps1 -Exe build/Release/stockwolf.exe
```

Runs `bench` twice and asserts the node counts match. A deterministic search
must yield an identical signature every run; a mismatch means non-determinism
crept in — fix it before trusting any game result. The printed number is the
bench signature to record in the merge commit.

For a pure refactor, also assert the signature is unchanged:

```powershell
./tools/bench_check.ps1 -Exe NEW.exe -Expect 14717091
```

## Running the SPRT

### STC (default — iterate here)

```powershell
./tools/sprt.ps1 -New build/Release/stockwolf.exe -Base tools/stockwolf-base.exe
```

Defaults: `tc=8+0.08`, book `tools/books/8moves_v3.epd` (epd), `elo0=0 elo1=5
alpha=0.05 beta=0.05`, `rounds=20000 -repeat`, `concurrency=10`, `Hash=16`,
`Threads=1`.

### LTC (confirm depth-scaling features: 3.6 NMP, 3.7 RFP, 3.8 LMR)

```powershell
./tools/sprt.ps1 -New NEW.exe -Base tools/stockwolf-base.exe -Tc 40+0.4 -Elo1 8
```

### Wider H1 bound for big-swing features

NMP / RFP / LMR use `[0, 8]`; everything else uses `[0, 5]`:

```powershell
./tools/sprt.ps1 -New NEW.exe -Base tools/stockwolf-base.exe -Elo1 8
```

bash equivalents live in `tools/sprt.sh` (same flags, `-new/-base/-tc/...`).

The canonical fast-chess command these wrap (PowerShell `^` line-continuation;
use `\` on bash):

```text
fastchess -engine cmd=NEW name=new -engine cmd=BASE name=base ^
  -each tc=8+0.08 proto=uci option.Hash=16 option.Threads=1 ^
  -rounds 20000 -repeat -concurrency 10 ^
  -openings file=tools/books/8moves_v3.epd format=epd order=random ^
  -sprt elo0=0 elo1=5 alpha=0.05 beta=0.05 ^
  -ratinginterval 10 -recover -pgnout tools/out.pgn
```

## Validate the harness BEFORE testing any feature (self-match)

Run NEW == BASE — the *same binary* on both sides:

```powershell
./tools/sprt.ps1 -New tools/stockwolf-base.exe -Base tools/stockwolf-base.exe
```

Expected: the LLR drifts toward the **lower** bound and the test **rejects** (no
Elo difference can pass `[0, 5]`), and the measured Elo sits near 0 within the
error bars. If a self-match shows a large nonzero Elo, your harness or book is
biased — fix that first. This is the proof the gate is sound. **Do not start the
ladder (3.1) until the self-match behaves.**

## Reading the fast-chess output

Periodic lines (every `-ratinginterval` rounds) and the final summary report:

- **LLR** — the log-likelihood ratio with its decision bounds, e.g.
  `LLR: 2.95 (-2.94, 2.94)`. The bounds come from `alpha`/`beta`:
  lower `≈ log(beta/(1-alpha))`, upper `≈ log((1-beta)/alpha)`.
  - **LLR ≥ upper bound → H1 accepted → KEEP.** The feature is worth ≥ `elo0`.
  - **LLR ≤ lower bound → H0 accepted → DISCARD.** Not worth `elo1`.
  - **In between at the round cap → inconclusive.** Extend rounds, or treat as
    no-gain and discard.
- **Elo** — measured Elo difference with a ± error margin (and `nElo`). For a
  *keep*, expect a clearly positive Elo whose lower error bound is comfortably
  above 0.
- **Pentanomial (`Ptnml(0-2)`)** — game-*pair* result distribution
  `[LL, LD, DD/WL, WD, WW]`. fast-chess uses the pentanomial model (paired
  games from the same opening) for tighter variance than the trinomial WDL
  count — this is why `-repeat` and a book matter. Healthy non-regressions are
  draw-heavy (mass piled in the middle bins).

## Per-feature workflow (repeat for every rung)

1. `git checkout -b feat/<name>`
2. Implement **only** that feature (+ any unit test it warrants).
3. Build Release as NEW; run `tools/bench_check.ps1 NEW` (reproducible).
4. `tools/sprt.ps1 -New NEW -Base tools/stockwolf-base.exe` (STC).
   For 3.6 / 3.7 / 3.8 also run an LTC confirmation (`-Tc 40+0.4`).
5. **LLR ≥ upper** → merge to `main`, record the new bench number in the commit,
   and rebuild `tools/stockwolf-base.exe` from the new `main` (baseline advances).
   **LLR ≤ lower** → discard the branch. Inconclusive → extend or discard.
6. Next rung.

## Ladder status

Update as rungs pass/fail (record bench + rough Elo from the SPRT log):

| #    | Feature                              | Status   | Bench | Elo (SPRT) |
|------|--------------------------------------|----------|-------|------------|
| —    | Stage 2 baseline                     | frozen   | 14717091 | —       |
| 3.1  | Transposition table (+ TT-move ord.) | pending  |       |            |
| 3.2  | Principal Variation Search (PVS)     | pending  |       |            |
| 3.3  | Killer moves                         | pending  |       |            |
| 3.4  | History heuristic                    | pending  |       |            |
| 3.5  | Aspiration windows                   | pending  |       |            |
| 3.6  | Null-move pruning (LTC confirm)      | pending  |       |            |
| 3.7  | Reverse futility / static null       | pending  |       |            |
| 3.8  | Late move reductions (LTC confirm)   | pending  |       |            |
| 3.9  | Late move pruning                    | pending  |       |            |
| 3.10 | Futility pruning                     | pending  |       |            |
| 3.11 | SEE (ordering + pruning)             | pending  |       |            |
| 3.12 | Continuation / counter-move history  | pending  |       |            |
| 3.13 | Optional: razoring / check ext / IIR | optional |       |            |
| 3.14 | Eval terms (keep light)              | optional |       |            |

### Deferred / optional TODO (revisit after the core ladder)

- 3.13 extras: razoring, check extensions, IIR.
- 3.14 cheap HCE eval terms (mobility, passers, doubled/isolated pawns, bishop
  pair, rook on open file, basic king safety) — keep light; NNUE (Stage 4)
  supersedes eval wholesale.
- SPSA tuning of margins/reductions (RFP margin, LMR base/divisor, futility
  margins, NMP R, aspiration Δ) once the search features are in. Treat each
  tuning run as an SPRT-gated change.
