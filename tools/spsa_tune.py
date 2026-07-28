#!/usr/bin/env python3
"""
tools/spsa_tune.py -- SPSA tuner for Grimfang.

Simultaneous Perturbation Stochastic Approximation, fishtest/Kiiski conventions.

Each iteration:
  1. Draw a Rademacher vector D (each component +1 or -1).
  2. Build two option sets: theta + c_k*D and theta - c_k*D (per-parameter c_k).
  3. Play PAIRS_PER_ITER game pairs of plus-vs-minus with fastchess.
  4. Update theta from the net result.

Both sides are the SAME binary with different `setoption` values. This is the
defining property of SPSA -- if you ever change this to play against a frozen
baseline you have a gauntlet, not a tuner, and it will converge to nothing.

Schedules (fishtest convention):
  c_k[i]   = c_end[i] * (N / (k+1)) ** GAMMA               -> c_end at k = N-1
  a_k[i]   = R_END * c_end[i]**2 * (N / (k+1)) ** ALPHA
  theta[i] += a_k[i] / c_k[i] * net * D[i]

where net = (points_plus - points_minus) / games, in [-1, 1].

State is checkpointed every iteration to spsa_state.json and is resumable:
just re-run the script. Delete the state file to start over.

Usage:
    python tools/spsa_tune.py                 # full run
    python tools/spsa_tune.py --iters 10      # smoke test
    python tools/spsa_tune.py --dry-run       # print the fastchess cmd, play nothing
"""

from __future__ import annotations

import argparse
import csv
import os
import json
import random
import subprocess
import sys
import time
from pathlib import Path

# --------------------------------------------------------------------------
# Paths -- adjust if your layout differs. The script validates all of these
# before it starts, so a typo fails in 1 second, not 6 hours.
# --------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parent.parent

FASTCHESS = ROOT / "tools" / "fastchess.exe"
ENGINE = ROOT / "build" / "Release" / "grimfang.exe"
BOOK = ROOT / "tools" / "books" / "8moves_v3.epd"

WORKDIR = ROOT / "tools" / "spsa"
STATE_FILE = WORKDIR / "spsa_state.json"
CSV_LOG = WORKDIR / "spsa_log.csv"
HEARTBEAT = WORKDIR / "heartbeat.json"
PGN_DIR = WORKDIR / "pgn"

# --------------------------------------------------------------------------
# Match settings. HASH_MB must match whatever you use for the Batch B SPRTs,
# or the two runs are not measuring the same engine.
# --------------------------------------------------------------------------

TC = "8+0.08"
HASH_MB = 64
CONCURRENCY = 6          # Ryzen 5 5500 = 6 physical cores
PAIRS_PER_ITER = 8       # 8 pairs = 16 games per iteration
TOTAL_ITERS = 4000       # 4000 * 16 = 64,000 games

# Flags some fastchess builds do not accept. If iteration 0 dies with a usage
# error, remove the offending entry here -- the script prints the full failing
# command line so you can see which one it is.
EXTRA_ARGS = ["-recover"]
USE_SRAND = True         # per-iteration opening shuffle; set False if unsupported

# --------------------------------------------------------------------------
# SPSA hyperparameters
# --------------------------------------------------------------------------

ALPHA = 0.602
GAMMA = 0.101

# R_END is the step scale. Fishtest's default is 0.002, but that is calibrated
# for iterations of ONE game pair over ~20-60k iterations. Here an iteration is
# 16 games, so there are ~8x fewer iterations for the same game budget and each
# carries ~4x less noise -- 0.002 leaves theta barely off its start after 4000
# iterations. Calibrated by simulation against a synthetic quadratic objective
# with realistic 16-game noise (sigma ~ 0.105 on net):
#
#   R_END   convergence in 4000 iters      drift under ZERO signal (% of range)
#   0.002   ~50% of the way to optimum     ~1%
#   0.010   ~90%                           2-8%
#   0.015   ~95%                           2-12%     <- chosen
#   0.040   overshoots, oscillates         5-33%
#
# 0.015 converges within budget while keeping noise-only drift well under any
# movement you would call a result. If a run looks jumpy, halve it.
R_END = 0.015

# --------------------------------------------------------------------------
# Parameters to tune -- Batch A only.
#
# Excluded deliberately:
#   HistoryBonusCap        -- bonus = min(depth^2 * pct/100, cap); at pct=100 the
#                             cap binds only at depth >= 40. At 8+0.08 you search
#                             depth 12-18, so its gradient is identically zero and
#                             it only injects noise into the other dimensions.
#   LmrBaseReduction, LmrDepth6Extra, LmrDeepExtra,
#   LmrPvReduction, LmrKillerReduction   -- 3-value domains (0..2). SPSA perturbs
#                             integers by >= 1, i.e. 50% of range, so the gradient
#                             sign is a coin flip. Test these as individual SPRTs.
#   NmpBase, NmpDepthDiv   -- saturate: NmpBase=6 and NmpDepthDiv=1 both produce
#                             exactly 746,690 nodes at bench depth 8, meaning R
#                             clamps identically. The upper half of both ranges is
#                             a plateau. Test as individual SPRTs.
#
# c_end ~= range/20, floored at 1 for integers, raised to 2 for RfpMaxDepth and
# LmrDeepAt whose ranges (12 and 18) tolerate a wider probe and whose signal is
# otherwise too weak to move them. If a parameter flatlines, raise its c_end;
# if it pins to a bound constantly, lower it.
# --------------------------------------------------------------------------

PARAMS: dict[str, dict[str, float]] = {
    "AspirationDelta":     {"start": 120, "min":  20, "max": 400, "c_end": 19},
    "AspirationWidenPct":  {"start": 150, "min": 101, "max": 300, "c_end": 10},
    "RfpMargin":           {"start": 100, "min":  50, "max": 200, "c_end":  8},
    "RfpMaxDepth":         {"start":   8, "min":   4, "max":  16, "c_end":  2},
    "LmrMinDepth":         {"start":   3, "min":   2, "max":   6, "c_end":  1},
    "LmrMinMoveIndex":     {"start":   3, "min":   1, "max":   6, "c_end":  1},
    "LmrDepth6At":         {"start":   6, "min":   2, "max":  12, "c_end":  2},
    "LmrDeepAt":           {"start":  12, "min":   6, "max":  24, "c_end":  2},
    "HistoryBonusMultPct": {"start": 100, "min":  50, "max": 200, "c_end":  8},
}

NAMES = list(PARAMS.keys())

_pgnout_style = "file="   # auto-downgraded to bare path if fastchess rejects it


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

def log(msg: str) -> None:
    """Always flush. Redirected stdout is block-buffered otherwise, which makes
    a running job look frozen for ~40 iterations at a time."""
    print(msg, flush=True)


def beat(iteration: int, phase: str, games: int) -> None:
    """Write a timestamped heartbeat. Closed immediately, so its mtime is an
    unbuffered ground truth for 'is this thing alive'."""
    try:
        HEARTBEAT.write_text(json.dumps({
            "ts": time.strftime("%Y-%m-%d %H:%M:%S"),
            "iter": iteration,
            "phase": phase,
            "games_played": games,
            "pid": os.getpid(),
        }, indent=2))
    except OSError:
        pass


def clamp(name: str, value: float) -> float:
    p = PARAMS[name]
    return max(p["min"], min(p["max"], value))


def c_k(name: str, k: int) -> float:
    return PARAMS[name]["c_end"] * (TOTAL_ITERS / (k + 1)) ** GAMMA


def a_k(name: str, k: int) -> float:
    return R_END * PARAMS[name]["c_end"] ** 2 * (TOTAL_ITERS / (k + 1)) ** ALPHA


def engine_args(name: str, values: dict[str, int]) -> list[str]:
    args = ["-engine", f"cmd={ENGINE}", f"name={name}"]
    args += [f"option.{k}={v}" for k, v in values.items()]
    return args


def pgn_path(k: int) -> Path:
    """One PGN per iteration.

    Windows holds the handle on a just-written file for a short, unpredictable
    window after the writing process exits (engine children, Defender's on-write
    scan). Reusing one filename means an unlink racing that window kills the run.
    Unique names remove the race entirely; cleanup is best-effort.
    """
    return PGN_DIR / f"iter_{k:06d}.pgn"


def build_cmd(plus: dict[str, int], minus: dict[str, int], seed: int,
              pgn: Path) -> list[str]:
    cmd = [str(FASTCHESS)]
    cmd += engine_args("plus", plus)
    cmd += engine_args("minus", minus)
    cmd += ["-each", "proto=uci", f"tc={TC}", f"option.Hash={HASH_MB}"]
    cmd += ["-openings", f"file={BOOK}", "format=epd", "order=random"]
    cmd += ["-rounds", str(PAIRS_PER_ITER), "-games", "2", "-repeat"]
    cmd += ["-concurrency", str(CONCURRENCY)]
    if USE_SRAND:
        cmd += ["-srand", str(seed)]
    cmd += EXTRA_ARGS
    if _pgnout_style == "file=":
        cmd += ["-pgnout", f"file={pgn}"]
    else:
        cmd += ["-pgnout", str(pgn)]
    return cmd


def score_pgn(path: Path) -> tuple[float, float, int]:
    """Return (points_plus, points_minus, games) parsed from the PGN.

    Parsing the PGN rather than fastchess's stdout summary makes this immune
    to changes in fastchess's console output formatting between versions.
    """
    plus = minus = 0.0
    games = 0
    white_is_plus = None

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if line.startswith('[White "'):
            white_is_plus = line[8:-2] == "plus"
        elif line.startswith('[Result "'):
            result = line[9:-2]
            if result == "*" or white_is_plus is None:
                white_is_plus = None
                continue
            if result == "1-0":
                w, b = 1.0, 0.0
            elif result == "0-1":
                w, b = 0.0, 1.0
            elif result == "1/2-1/2":
                w, b = 0.5, 0.5
            else:
                white_is_plus = None
                continue
            if white_is_plus:
                plus += w
                minus += b
            else:
                plus += b
                minus += w
            games += 1
            white_is_plus = None

    return plus, minus, games


def play(plus: dict[str, int], minus: dict[str, int], k: int) -> tuple[float, int]:
    """Play one iteration's games. Return (net_result_for_plus, games).

    Retries transient failures. A multi-day background run must not die because
    a virus scanner held a file handle for 200ms.
    """
    global _pgnout_style

    last_err = ""
    for attempt in range(3):
        pgn = pgn_path(k) if attempt == 0 else PGN_DIR / f"iter_{k:06d}_r{attempt}.pgn"
        cmd = build_cmd(plus, minus, k, pgn)
        proc = subprocess.run(cmd, capture_output=True, text=True)

        if proc.returncode != 0 and _pgnout_style == "file=" and attempt == 0:
            # Older fastchess builds want a bare path after -pgnout.
            _pgnout_style = "bare"
            cmd = build_cmd(plus, minus, k, pgn)
            proc = subprocess.run(cmd, capture_output=True, text=True)

        if proc.returncode == 0 and pgn.exists():
            try:
                p, m, games = score_pgn(pgn)
            except OSError as exc:
                last_err = f"could not read PGN: {exc}"
                time.sleep(2.0)
                continue
            if games > 0:
                cleanup_pgn(k)
                return (p - m) / games, games
            last_err = "no completed games parsed from PGN"
        else:
            last_err = (f"fastchess rc={proc.returncode}\n"
                        f"--- stdout ---\n{proc.stdout[-2000:]}\n"
                        f"--- stderr ---\n{proc.stderr[-2000:]}")

        print(f"  [iter {k}] attempt {attempt + 1} failed: {last_err.splitlines()[0]}",
              flush=True)
        time.sleep(5.0)

    print(f"\nIteration {k} failed 3 times. Command was:\n  " + " ".join(cmd),
          file=sys.stderr)
    print(last_err, file=sys.stderr)
    sys.exit(1)


def cleanup_pgn(k: int, keep: int = 3) -> None:
    """Best-effort removal of PGNs older than the last `keep` iterations."""
    for old in PGN_DIR.glob("iter_*.pgn"):
        try:
            n = int(old.stem.split("_")[1])
        except (IndexError, ValueError):
            continue
        if n <= k - keep:
            try:
                old.unlink()
            except OSError:
                pass  # still locked; next iteration will get it


def preflight() -> None:
    missing = [p for p in (FASTCHESS, ENGINE, BOOK) if not p.exists()]
    if missing:
        for p in missing:
            print(f"NOT FOUND: {p}", file=sys.stderr)
        sys.exit(1)
    WORKDIR.mkdir(parents=True, exist_ok=True)
    PGN_DIR.mkdir(parents=True, exist_ok=True)
    for name, p in PARAMS.items():
        if not (p["min"] <= p["start"] <= p["max"]):
            print(f"{name}: start outside [min, max]", file=sys.stderr)
            sys.exit(1)


def show_status() -> None:
    """Read state + heartbeat and report. Safe to run while a tune is going."""
    if not STATE_FILE.exists():
        log("No run found (no spsa_state.json).")
        return
    st = json.loads(STATE_FILE.read_text())
    k = int(st["iter"])
    age = time.time() - STATE_FILE.stat().st_mtime
    health = "RUNNING" if age < 300 else f"STALE (no update for {age / 60:.0f} min)"

    log(f"iteration {k}/{TOTAL_ITERS}   {100 * k / TOTAL_ITERS:.1f}%   [{health}]")
    log(f"games played  {k * 2 * PAIRS_PER_ITER}")

    if HEARTBEAT.exists():
        hb = json.loads(HEARTBEAT.read_text())
        log(f"heartbeat     {hb['ts']}  phase={hb['phase']}  pid={hb['pid']}")

    log("")
    log(f"{'parameter':<22}{'now':>6}{'start':>7}{'drift':>8}   {'% of range':>10}")
    for n in NAMES:
        cur = st["theta"][n]
        start = PARAMS[n]["start"]
        rng = PARAMS[n]["max"] - PARAMS[n]["min"]
        pct = 100.0 * (cur - start) / rng
        flag = ""
        if abs(cur - PARAMS[n]["min"]) < 0.5:
            flag = "  <-- PINNED at min"
        elif abs(cur - PARAMS[n]["max"]) < 0.5:
            flag = "  <-- PINNED at max"
        log(f"{n:<22}{cur:>6.0f}{start:>7.0f}{cur - start:>+8.1f}   {pct:>+9.1f}%{flag}")


# --------------------------------------------------------------------------
# Main loop
# --------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iters", type=int, default=None,
                    help="stop after this many NEW iterations (smoke testing)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the iteration-0 command line and exit")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="log the perturbed option sets and per-phase timing")
    ap.add_argument("--status", action="store_true",
                    help="print progress and exit; starts nothing")
    args = ap.parse_args()

    preflight()

    if args.status:
        show_status()
        return

    theta = {n: float(PARAMS[n]["start"]) for n in NAMES}
    k0 = 0
    if STATE_FILE.exists():
        saved = json.loads(STATE_FILE.read_text())
        for n in NAMES:
            if n in saved["theta"]:
                theta[n] = float(saved["theta"][n])
        k0 = int(saved["iter"])
        log(f"Resuming from iteration {k0}.")

    if args.dry_run:
        rng = random.Random(k0)
        d = {n: rng.choice((-1, 1)) for n in NAMES}
        plus = {n: int(round(clamp(n, theta[n] + c_k(n, k0) * d[n]))) for n in NAMES}
        minus = {n: int(round(clamp(n, theta[n] - c_k(n, k0) * d[n]))) for n in NAMES}
        print(" ".join(build_cmd(plus, minus, k0, pgn_path(k0))))
        return

    if not CSV_LOG.exists():
        with CSV_LOG.open("w", newline="", encoding="utf-8") as fh:
            csv.writer(fh).writerow(["iter", "games", "net"] + NAMES)

    stop_at = TOTAL_ITERS if args.iters is None else min(TOTAL_ITERS, k0 + args.iters)
    started = time.time()
    total_games = 0

    for k in range(k0, stop_at):
        rng = random.Random(k)
        d = {n: rng.choice((-1, 1)) for n in NAMES}

        ck = {n: c_k(n, k) for n in NAMES}
        plus = {n: int(round(clamp(n, theta[n] + ck[n] * d[n]))) for n in NAMES}
        minus = {n: int(round(clamp(n, theta[n] - ck[n] * d[n]))) for n in NAMES}

        pinned = [n for n in NAMES if plus[n] == minus[n]]
        if pinned:
            log(f"  [iter {k}] pinned at a bound, no signal this iter: "
                + ", ".join(pinned))
        if len(pinned) == len(NAMES):
            print("All parameters pinned -- ranges are wrong. Stopping.", file=sys.stderr)
            sys.exit(1)

        if args.verbose:
            log(f"  [iter {k}] plus : "
                + " ".join(f"{n}={plus[n]}" for n in NAMES))
            log(f"  [iter {k}] minus: "
                + " ".join(f"{n}={minus[n]}" for n in NAMES))
            log(f"  [iter {k}] playing {2 * PAIRS_PER_ITER} games at {TC} ...")

        beat(k, "playing", total_games)
        t_iter = time.time()
        net, games = play(plus, minus, k)
        secs = time.time() - t_iter
        total_games += games
        beat(k + 1, "idle", total_games)

        for n in NAMES:
            theta[n] = clamp(n, theta[n] + a_k(n, k) / ck[n] * net * d[n])

        STATE_FILE.write_text(json.dumps(
            {"iter": k + 1, "theta": theta, "total_iters": TOTAL_ITERS}, indent=2))
        with CSV_LOG.open("a", newline="", encoding="utf-8") as fh:
            csv.writer(fh).writerow(
                [k, games, f"{net:+.4f}"] + [f"{theta[n]:.2f}" for n in NAMES])

        elapsed = time.time() - started
        done = k - k0 + 1
        eta_h = (elapsed / done) * (stop_at - k - 1) / 3600.0
        rounded = {n: int(round(theta[n])) for n in NAMES}
        log(f"[{k + 1}/{stop_at}] games={total_games} net={net:+.3f} "
            f"{secs:.0f}s eta={eta_h:.1f}h  {rounded}")

    log("\nFinal theta (rounded -- these become your new defaults):")
    for n in NAMES:
        log(f"  {n:<22} {int(round(theta[n])):>5}   (was {int(PARAMS[n]['start'])})")


if __name__ == "__main__":
    main()
