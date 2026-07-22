# Grimfang CI — Linux Test Failure Findings

## Failing run
https://github.com/Grimfang-labs/grimfang/actions/runs/29856614983/job/88796868777
(`ubuntu-latest`, job "Test (fast)", commit `ccc6679` — "fix(tests): POSIX-correct UCI test shell invocation")

## Symptom
`ctest` step exits with code 8. Catch2 output shows four failures in `grimfang_tests`, escalating in severity:

1. `perft_tests.cpp:84` — `bishop_attacks(s, occ) == bishop_attacks_slow(s, occ)` fails:
   `144396663053683200 (0x201000000110a00)` vs `144115188076972544 (0x200000000110a00)`
2. `perft_tests.cpp:146` — `perft startpos` D1: got `0`, expected `20`
3. `perft_tests.cpp:155` — `perft kiwipete` D1: got `50`, expected `48`
4. `perft_tests.cpp:167` — `perft position 4` — **SIGSEGV**, test process crashes

Windows-latest passes on the same commit; only the `ubuntu-latest` leg fails.

## Diagnosis
This is runtime memory corruption, not a test or CI configuration problem. The `ccc6679` shell-quoting fix for the UCI test is unrelated and correct — the underlying bug pre-dates it.

Key evidence:
- The **slow** slider reference (`bishop_attacks_slow`, a pure ray-walk with no lookup tables) is itself returning a geometrically impossible attack set for at least one query — pointing at corrupted *input* (occupancy bitboard) rather than a broken magic table.
- The **rook** path is clean: position 3's depth-6 perft (11,030,083 nodes, all rook/king-heavy) passed with zero bishops on the board, isolating the fault to bishop-adjacent state/code paths.
- `perft startpos` D1 returning `0` instead of `20` means `generate_legal` produced an empty move list from the standard starting position — consistent with a corrupted occupancy bitboard rather than a movegen logic bug (the logic is exercised correctly by the passing Windows build and by `position 3`).
- The eventual SIGSEGV in `position 4` is consistent with a corrupted attack bitboard eventually causing movegen to treat a king square as capturable, cascading into invalid state and a crash — a downstream symptom, not the root cause.
- Only reproduces on `ubuntu-latest` / GCC in Release (`-O3`); MSVC's memory layout apparently doesn't expose the same stray write. This is classic undefined-behavior fingerprint (e.g. an out-of-bounds write, uninitialized read, or strict-aliasing violation) — compiler- and platform-dependent, and load-bearing on optimization level.

Places inspected that are candidates but not yet confirmed as the cause:
- `attacks.cpp` — magic bitboard init/verify (self-checked at startup via `verify_magics()`, so a build that reaches the test would have already aborted if the *tables* were wrong — points at runtime corruption *after* init, not a bad table)
- `movegen.cpp`, `position.cpp` — occupancy/bitboard mutation during make/unmake
- `nnue.cpp` — reinterprets a raw embedded byte array as `nnue::Network` via `reinterpret_cast`; alignment/UB risk, though guarded by a runtime alignment check
- `MoveList` (`movegen.hpp`) — fixed 256-move buffer with no bounds check on `add()`; a pathological position could overflow it, corrupting adjacent memory

## What was done
Added a second Linux-only CI job (`sanitize`) to `.github/workflows/ci.yml` that builds with AddressSanitizer + UndefinedBehaviorSanitizer under GCC (`-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g`, Debug config) and runs the fast test label. This should pinpoint the exact file/line of the corrupting write/UB the next time CI runs, rather than the current downstream segfault/wrong-answer symptoms.

## Minor unrelated issues noticed
- `src/datagen.cpp:50` — `datagen_to_piece(uint8_t)` defined but unused (`-Wunused-function`)
- `tests/uci_tests.cpp:53` — return value of `std::system(...)` is ignored (`-Wunused-result`); a failed engine launch currently fails silently downstream instead of failing the test clearly

## Next step
Push the branch, let the `sanitize` job run, and read the ASan/UBSan stack trace it produces — that will name the exact corrupting line and let the actual fix be scoped precisely instead of guessed at.
