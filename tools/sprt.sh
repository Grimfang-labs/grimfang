#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# sprt.sh - run an engine-vs-engine SPRT of a NEW Grimfang build against a
# frozen BASE build using fast-chess. Stage 3 keep/revert gate: one change per
# test, trust the LLR, not the diff.
#
# The gate is sound only when a NEW == BASE self-match drifts toward the lower
# bound (rejects [elo0, elo1]) with measured Elo near 0. Validate that before
# trusting any feature result (see tools/README.md).
#
# Usage:
#   tools/sprt.sh -new <exe> -base <exe> [options]
#
# Options (defaults in brackets):
#   -new   <path>     NEW engine (candidate with the feature)            (required)
#   -base  <path>     BASE engine (frozen baseline)                      (required)
#   -tc    <tc>       time control move+inc seconds                      [8+0.08]
#   -book  <path>     opening book                          [tools/books/8moves_v3.epd]
#   -format <fmt>     book format (epd|pgn)                              [epd]
#   -elo0  <n>        SPRT H0 bound                                      [0]
#   -elo1  <n>        SPRT H1 bound (use 8 for NMP/RFP/LMR)              [5]
#   -alpha <n>        type-I error                                       [0.05]
#   -beta  <n>        type-II error                                      [0.05]
#   -rounds <n>       max rounds (game pairs with -repeat)               [20000]
#   -concurrency <n>  parallel games                                     [10]
#   -hash  <mb>       UCI Hash MB on both engines                        [16]
#   -threads <n>      UCI Threads on both engines                        [1]
#   -pgnout <path>    PGN output                                         [tools/out.pgn]
#   -fastchess <path> override fast-chess binary                  [PATH, then tools/]
#
# Examples:
#   tools/sprt.sh -new build/grimfang -base tools/grimfang-base          # STC gate
#   tools/sprt.sh -new NEW -base BASE -tc 40+0.4 -elo1 8                    # LTC confirm
#   tools/sprt.sh -new tools/grimfang-base -base tools/grimfang-base      # self-match
# -----------------------------------------------------------------------------
set -euo pipefail

# Repo root = parent of this tools/ directory.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

NEW=""
BASE=""
TC="8+0.08"
BOOK="tools/books/8moves_v3.epd"
FORMAT="epd"
ELO0="0"
ELO1="5"
ALPHA="0.05"
BETA="0.05"
ROUNDS="20000"
CONCURRENCY="10"
HASH="16"
THREADS="1"
PGNOUT="tools/out.pgn"
FASTCHESS=""

die() { echo "error: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -new)         NEW="$2"; shift 2 ;;
        -base)        BASE="$2"; shift 2 ;;
        -tc)          TC="$2"; shift 2 ;;
        -book)        BOOK="$2"; shift 2 ;;
        -format)      FORMAT="$2"; shift 2 ;;
        -elo0)        ELO0="$2"; shift 2 ;;
        -elo1)        ELO1="$2"; shift 2 ;;
        -alpha)       ALPHA="$2"; shift 2 ;;
        -beta)        BETA="$2"; shift 2 ;;
        -rounds)      ROUNDS="$2"; shift 2 ;;
        -concurrency) CONCURRENCY="$2"; shift 2 ;;
        -hash)        HASH="$2"; shift 2 ;;
        -threads)     THREADS="$2"; shift 2 ;;
        -pgnout)      PGNOUT="$2"; shift 2 ;;
        -fastchess)   FASTCHESS="$2"; shift 2 ;;
        *)            die "unknown argument: $1" ;;
    esac
done

[[ -n "${NEW}"  ]] || die "-new <exe> is required"
[[ -n "${BASE}" ]] || die "-base <exe> is required"
[[ -f "${NEW}"  ]] || die "NEW engine not found: ${NEW}"
[[ -f "${BASE}" ]] || die "BASE engine not found: ${BASE}"
[[ -f "${BOOK}" ]] || die "opening book not found: ${BOOK} (download it, see tools/README.md)"

# Locate fast-chess: explicit override, then PATH, then tools/.
if [[ -n "${FASTCHESS}" ]]; then
    [[ -x "${FASTCHESS}" || -f "${FASTCHESS}" ]] || die "fast-chess not found: ${FASTCHESS}"
    FC="${FASTCHESS}"
elif command -v fastchess >/dev/null 2>&1; then
    FC="$(command -v fastchess)"
elif command -v fast-chess >/dev/null 2>&1; then
    FC="$(command -v fast-chess)"
elif [[ -x "tools/fastchess" ]]; then
    FC="tools/fastchess"
else
    die "fast-chess not found on PATH or at tools/fastchess. Install Disservin/fastchess (see tools/README.md) or pass -fastchess <path>."
fi

FC_ARGS=(
    -engine "cmd=${NEW}"  name=new
    -engine "cmd=${BASE}" name=base
    -each "tc=${TC}" proto=uci "option.Hash=${HASH}" "option.Threads=${THREADS}"
    -rounds "${ROUNDS}" -repeat -concurrency "${CONCURRENCY}"
    -openings "file=${BOOK}" "format=${FORMAT}" order=random
    -draw movenumber=40 movecount=8 score=10
    -resign movecount=3 score=600
    -sprt "elo0=${ELO0}" "elo1=${ELO1}" "alpha=${ALPHA}" "beta=${BETA}"
    -ratinginterval 10
    -recover
    -pgnout "${PGNOUT}"
)

echo "--- SPRT configuration ---------------------------------------------"
echo "NEW         : ${NEW}"
echo "BASE        : ${BASE}"
echo "TC          : ${TC}   book: ${BOOK} (${FORMAT})"
echo "SPRT        : elo0=${ELO0} elo1=${ELO1} alpha=${ALPHA} beta=${BETA}"
echo "rounds      : ${ROUNDS} (repeat)   concurrency: ${CONCURRENCY}"
echo "UCI         : Hash=${HASH}MB Threads=${THREADS}"
echo "--- fast-chess invocation ------------------------------------------"
echo "${FC} ${FC_ARGS[*]}"
echo "--------------------------------------------------------------------"

exec "${FC}" "${FC_ARGS[@]}"
