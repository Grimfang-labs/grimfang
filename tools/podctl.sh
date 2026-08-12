#!/usr/bin/env bash
#
# podctl.sh -- Grimfang test harness for rented, ephemeral pods.
#
#   podctl setup                              build + bench gate + report
#   podctl status                             where am I, what's unsaved
#   podctl rung    <tag> "<dev>" ["<base>"] [elo1]     SPRT -> a DECISION
#   podctl measure <tag> "<dev>" ["<base>"] [rounds]   fixed -> a NUMBER
#   podctl save    ["msg"]                    commit + push results
#   podctl archive                            tarball artifacts
#   podctl sync                               rsync artifacts to REMOTE_SYNC
#   podctl pull                               update repo, rebuild, re-gate
#   podctl teardown                           pre-destroy checklist
#
# STATE LIVES IN $DATA_DIR (default /workspace). The image is stateless; mount
# a volume there if the host offers one. On a marketplace pod the disk dies with
# the contract, so `save` (git) and `sync` (rsync) are the real persistence.
#
set -euo pipefail

DATA_DIR="${DATA_DIR:-/workspace}"
REPO_DIR="${GRIMFANG_DIR:-$DATA_DIR/grimfang}"
REPO_URL="${GRIMFANG_REPO:-https://github.com/grimfang-labs/grimfang.git}"
BRANCH="${GRIMFANG_BRANCH:-main}"

# Standard test conditions. Changing any of these breaks comparability with
# every prior result. Change one -> change it everywhere, and say so in
# RESULTS.md.
NODES="${NODES:-1000000}"
HASH_MB="${HASH_MB:-64}"
BOOK="${BOOK:-8moves_v3.epd}"

GIT_NAME="${GIT_NAME:-Dylan}"
GIT_EMAIL="${GIT_EMAIL:-shywolf91@users.noreply.github.com}"

# Optional: user@host:/path for `podctl sync`, e.g. root@104.223.27.112:~/pod_results
REMOTE_SYNC="${REMOTE_SYNC:-}"
REMOTE_PORT="${REMOTE_PORT:-22}"

C_OK=$'\e[32m'; C_WARN=$'\e[33m'; C_ERR=$'\e[31m'; C_HDR=$'\e[36m'; C_OFF=$'\e[0m'
step() { printf "\n${C_HDR}==> %s${C_OFF}\n" "$*"; }
ok()   { printf "${C_OK}  ok   %s${C_OFF}\n" "$*"; }
warn() { printf "${C_WARN}  !!   %s${C_OFF}\n" "$*"; }
die()  { printf "\n${C_ERR}FAIL: %s${C_OFF}\n" "$*" >&2; exit "${2:-1}"; }

ENGINE="$REPO_DIR/build/grimfang"
FC="$REPO_DIR/tools/fastchess/fastchess"
SIGFILE="$REPO_DIR/tools/BENCH_SIG"
RESULTS="$REPO_DIR/tools/RESULTS.md"
PGN_DIR="$DATA_DIR/pgn"
LOG_DIR="$DATA_DIR/logs"

# --- CPU: nproc/lscpu report the HOST on a shared pod; cgroup quota is truth --
detect_cores() {
    local q p c
    if [[ -f /sys/fs/cgroup/cpu.max ]]; then
        read -r q p < /sys/fs/cgroup/cpu.max
        [[ "$q" == "max" ]] && { nproc; return; }
    elif [[ -f /sys/fs/cgroup/cpu/cpu.cfs_quota_us ]]; then
        q=$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us)
        p=$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us)
        [[ "$q" == "-1" ]] && { nproc; return; }
    else nproc; return; fi
    c=$(( q / p )); (( c < 1 )) && c=1; echo "$c"
}
CORES=$(detect_cores); CONC=$(( CORES - 2 )); (( CONC < 1 )) && CONC=1

bench_once() { "$ENGINE" bench 2>&1 | grep -oP 'Nodes searched\s*:\s*\K[0-9]+'; }

bench_gate() {
    [[ -x "$ENGINE" ]] || die "engine not built"
    local want b1 b2
    if [[ -f "$SIGFILE" ]]; then want=$(tr -d '[:space:]' < "$SIGFILE")
    else warn "tools/BENCH_SIG missing -- recording current"; bench_once > "$SIGFILE"
         want=$(tr -d '[:space:]' < "$SIGFILE"); fi
    b1=$(bench_once); b2=$(bench_once)
    printf "  expected %s | run1 %s | run2 %s\n" "$want" "$b1" "$b2"
    [[ "$b1" == "$want" && "$b2" == "$want" ]] || die \
"BENCH MISMATCH. This build does not match tools/BENCH_SIG.
Run nothing on it. Causes: wrong commit, missing NNUE net, or a genuine
codegen/UB difference (GCC has exposed UB here that MSVC hid)." 1
    ok "bench $want reproduced twice"
}

require_ready() {
    [[ -x "$ENGINE" ]] || die "engine missing -- podctl setup"
    [[ -x "$FC" ]]     || die "fastchess missing -- podctl setup"
    [[ -s "$REPO_DIR/tools/books/$BOOK" ]] || die \
"book missing: tools/books/$BOOK -- commit it (git add -f) so setup is hands-off" 2
}

opts_to_args() { local o out=""; for o in $1; do out="$out option.$o"; done; echo "$out"; }

# --- record: append every verdict to RESULTS.md the moment it lands -----------
record() {  # tag kind desc log
    local tag="$1" kind="$2" desc="$3" log="$4" elo games verdict
    elo=$(grep -oP '^Elo: \K[-0-9.]+ \+/- [0-9.]+' "$log" | tail -1 || true)
    games=$(grep -oP '^Games: \K[0-9]+' "$log" | tail -1 || true)
    verdict=$(grep -oE 'H[01] was accepted' "$log" | tail -1 || true)
    [[ "$kind" == "FIXED" ]] && verdict="measurement"
    [[ -z "$verdict" ]] && verdict="no verdict (rounds exhausted)"
    [[ -z "$games" || "$games" == "0" ]] && verdict="RUN FAILED (0 games)"

    [[ -f "$RESULTS" ]] || cat > "$RESULTS" <<'HDR'
# Grimfang test results

Standard conditions unless noted: `nodes=1000000`/move, `Hash=64`,
`8moves_v3.epd`, 1 thread.

**SPRT results are DECISIONS, not measurements.** SPRT stops when evidence
looks favourable, so the stopping rule correlates with upward noise and the
point estimate is inflated. Never sum them; measure with a fixed-length match.
Demonstrated 2026-07-29: v01 (+33.06) + v10 (+20.35) = +53.4, but the
fixed-length 10,000-game measurement of the same change gave +17.04 (~3x).

| date | tag | kind | change | Elo | games | verdict |
|---|---|---|---|---|---|---|
HDR
    printf "| %s | %s | %s | %s | %s | %s | %s |\n" "$(date +%F)" "$tag" \
        "$kind" "$desc" "${elo:-?}" "${games:-0}" "$verdict" >> "$RESULTS"
    printf "\n${C_HDR}--- recorded ---${C_OFF}\n"; tail -1 "$RESULTS"
    warn "not yet on GitHub -- run: podctl save"
}

run_match() {  # tag kind dev base extra_args...
    local tag="$1" kind="$2" dev="$3" base="$4"; shift 4
    require_ready
    mkdir -p "$PGN_DIR" "$LOG_DIR"
    local log="$LOG_DIR/$tag.log"
    cd "$REPO_DIR"
    "$FC" \
        -engine cmd="$ENGINE" name=dev  $(opts_to_args "$dev")  nodes="$NODES" \
        -engine cmd="$ENGINE" name=base $(opts_to_args "$base") nodes="$NODES" \
        -each proto=uci option.Hash="$HASH_MB" \
        -openings file="tools/books/$BOOK" format=epd order=random \
        -games 2 -repeat -recover -concurrency "$CONC" \
        -pgnout file="$PGN_DIR/$tag.pgn" \
        "$@" 2>&1 | tee "$log"
    record "$tag" "$kind" "dev:[$dev] vs base:[${base:-defaults}]" "$log"
}

# --- subcommands -------------------------------------------------------------

cmd_setup() {
    step "Allocation"
    printf "  host %s CPUs | cgroup grants %s | concurrency %s\n" "$(nproc)" "$CORES" "$CONC"
    [[ "$CORES" -lt "$(nproc)" ]] && warn "you have $CORES of $(nproc) host CPUs"
    printf "  toolchain: %s\n" "$(cat /etc/grimfang-toolchain 2>/dev/null | tr '\n' ' ')"

    step "Steal (10s)"
    local st; st=$(vmstat 1 10 | tail -n +3 | awk '{s+=$(NF-1)} END{print s+0}')
    [[ "$st" -gt 0 ]] && warn "steal=$st -- host oversubscribed, you're paying for stolen cycles" || ok "none"

    step "Repo"
    mkdir -p "$DATA_DIR" "$PGN_DIR" "$LOG_DIR"
    if [[ -d "$REPO_DIR/.git" ]]; then git -C "$REPO_DIR" pull --ff-only
    else git clone --branch "$BRANCH" "$REPO_URL" "$REPO_DIR"; fi
    cd "$REPO_DIR"
    git config user.name "$GIT_NAME"; git config user.email "$GIT_EMAIL"
    [[ -e tools/fastchess ]] || ln -s /opt/fastchess tools/fastchess
    ok "$(git rev-parse --short HEAD) on $(git branch --show-current)"

    step "Build"
    cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja >/dev/null
    cmake --build build -j"$CORES" >/dev/null
    ok "built"

    step "Bench gate"; bench_gate

    step "Speed"
    local nps; nps=$( (printf "position startpos\ngo nodes 50000000\n"; sleep 60) \
        | timeout 90 "$ENGINE" | grep -oP 'nps \K[0-9]+' | tail -1 || true)
    [[ -n "$nps" ]] && printf "  %s nps | est %.1fx a 6-core 4.93M desktop\n" \
        "$nps" "$(echo "scale=4;($CONC/6)*($nps/4930000)" | bc)"

    step "Book"
    [[ -s "$REPO_DIR/tools/books/$BOOK" ]] && ok "$BOOK present" \
        || warn "$BOOK MISSING -- no match can run. Commit it: git add -f tools/books/$BOOK"

    tmux kill-session -t steal 2>/dev/null || true
    tmux new-session -d -s steal "vmstat 60 >> $DATA_DIR/steal.log"
    cat <<EOF

${C_HDR}READY${C_OFF}  conc=$CONC nodes=$NODES hash=${HASH_MB}MB data=$DATA_DIR
  podctl rung    v13 "LmrDeepExtra=0" "" 5
  podctl measure v21 "LmrDeepExtra=0" "" 5000
  podctl save
EOF
}

cmd_rung()    { run_match "$1" SPRT "$2" "${3:-}" \
                  -sprt elo0=0 elo1="${4:-5}" alpha=0.05 beta=0.05 \
                  -rounds 3000 -ratinginterval 200; }

cmd_measure() { warn "fixed length: runs all $(( ${4:-5000} * 2 )) games. This is the trustworthy number."
                run_match "$1" FIXED "$2" "${3:-}" \
                  -rounds "${4:-5000}" -ratinginterval 500; }

git_push() {
    [[ -n "${GH_TOKEN:-}" ]] || die \
"GH_TOKEN not set. Fine-grained token, THIS REPO ONLY, Contents=read/write,
7-day expiry. Set it via the pod host's secret binding, never in the image.
  export GH_TOKEN=github_pat_..."
    # Transient helper: token never enters .git/config, the remote URL, or argv.
    git -c credential.helper='!f(){ echo username=x-access-token; echo "password=$GH_TOKEN"; };f' \
        push origin HEAD
}

cmd_save() {
    cd "$REPO_DIR"
    git add -f tools/RESULTS.md tools/BENCH_SIG 2>/dev/null || true
    git add -A tools/*.sh docker/ 2>/dev/null || true
    if git diff --cached --quiet; then ok "nothing to commit"; else
        git commit -m "${1:-results: pod run $(date +%F)}"
    fi
    git pull --rebase --autostash || die "rebase conflict -- resolve manually before pushing"
    git_push
    ok "pushed $(git rev-parse --short HEAD)"
}

cmd_archive() {
    local out="$DATA_DIR/grimfang_$(date +%Y%m%d_%H%M).tar.gz"
    tar czf "$out" -C "$DATA_DIR" logs \
        -C "$REPO_DIR" tools/RESULTS.md 2>/dev/null || true
    ls -lh "$out"
    warn "PGNs excluded (large). Keep a specific one:  scp ... $PGN_DIR/<tag>.pgn"
    echo "$out"
}

cmd_sync() {
    [[ -n "$REMOTE_SYNC" ]] || die "REMOTE_SYNC not set (user@host:/path)"
    rsync -avz -e "ssh -p $REMOTE_PORT -o StrictHostKeyChecking=no" \
        "$LOG_DIR" "$REPO_DIR/tools/RESULTS.md" "$REMOTE_SYNC/"
    ok "synced to $REMOTE_SYNC"
}

cmd_pull() {
    cd "$REPO_DIR"; git pull --ff-only
    cmake --build build -j"$CORES" >/dev/null
    ok "at $(git rev-parse --short HEAD)"; bench_gate
}

cmd_status() {
    cd "$REPO_DIR" 2>/dev/null || die "no repo at $REPO_DIR -- podctl setup"
    printf "commit     %s (%s)\n" "$(git rev-parse --short HEAD)" "$(git branch --show-current)"
    printf "bench sig  %s\n" "$([[ -f $SIGFILE ]] && cat "$SIGFILE" || echo '<unset>')"
    printf "toolchain  %s\n" "$(cat /etc/grimfang-toolchain 2>/dev/null | tr '\n' ' ')"
    printf "cores      %s granted, conc %s\n" "$CORES" "$CONC"
    printf "conditions nodes=%s hash=%sMB book=%s\n" "$NODES" "$HASH_MB" "$BOOK"
    printf "token      %s\n" "$([[ -n ${GH_TOKEN:-} ]] && echo present || echo 'NOT SET')"
    printf "data       %s (pgn %s, logs %s)\n" "$DATA_DIR" \
        "$(du -sh "$PGN_DIR" 2>/dev/null | cut -f1)" "$(du -sh "$LOG_DIR" 2>/dev/null | cut -f1)"
    printf "matches    %s fastchess running\n" "$(pgrep -c fastchess || echo 0)"
    printf "\nUNPUSHED:\n"; git log --oneline @{u}..HEAD 2>/dev/null || echo "  (none)"
    printf "\nUNCOMMITTED:\n"; git status --short || true
    printf "\nlast results:\n"; tail -4 "$RESULTS" 2>/dev/null || echo "  (none)"
}

cmd_teardown() {
    step "Pre-destroy checklist"
    local fail=0
    cd "$REPO_DIR" 2>/dev/null || die "no repo"

    local running; running=$(pgrep -c fastchess || echo 0)
    if [[ "$running" -gt 0 ]]; then
        warn "$running fastchess processes STILL RUNNING -- results incomplete"; fail=1
    else ok "no matches running"; fi

    if [[ -n "$(git status --porcelain)" ]] || ! git diff --quiet @{u}..HEAD 2>/dev/null; then
        warn "uncommitted or unpushed work -- running save"
        cmd_save "results: teardown $(date +%F)" || fail=1
    else ok "git clean and pushed"; fi

    local arc; arc=$(cmd_archive | tail -1)
    [[ -n "$REMOTE_SYNC" ]] && cmd_sync || warn "REMOTE_SYNC unset -- pull manually"

    cat <<EOF

${C_HDR}=== BEFORE YOU DESTROY THIS POD ===${C_OFF}
  From your desktop:
    scp -P <port> root@<ip>:$arc .
  Largest PGNs (only if you need re-analysis):
$(ls -S "$PGN_DIR"/*.pgn 2>/dev/null | head -3 | sed 's/^/    /')

  Then: revoke the GH_TOKEN you issued for this pod.
EOF
    [[ "$fail" -eq 0 ]] && ok "SAFE TO DESTROY" || warn "NOT clean -- review above"
}

case "${1:-}" in
    setup) cmd_setup ;;
    pull) cmd_pull ;;
    rung) shift; cmd_rung "$@" ;;
    measure) shift; cmd_measure "$@" ;;
    save) shift; cmd_save "${1:-}" ;;
    archive) cmd_archive >/dev/null ;;
    sync) cmd_sync ;;
    status) cmd_status ;;
    teardown) cmd_teardown ;;
    *) sed -n '3,20p' "$0"; exit 1 ;;
esac
