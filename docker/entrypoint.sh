#!/usr/bin/env bash
#
# Container entrypoint: wire up SSH, clone/refresh the repo, hand off to CMD.
#
# Deliberately does NOT build or run tests. A build failure should be something
# you watch on a terminal, not something that silently kills the container.
set -euo pipefail
log() { printf "\e[36m[entrypoint]\e[0m %s\n" "$*"; }

DATA_DIR="${DATA_DIR:-/workspace}"
REPO_DIR="${GRIMFANG_DIR:-$DATA_DIR/grimfang}"
REPO_URL="${GRIMFANG_REPO:-https://github.com/grimfang-labs/grimfang.git}"
BRANCH="${GRIMFANG_BRANCH:-main}"

# --- Secrets: QuickPod and similar hosts mount secrets as read-only FILES
# under /run/secrets, not as environment variables. Load them so GH_TOKEN and
# friends behave the way the rest of the tooling expects.
for _sf in /run/secrets/*; do
    [[ -f "$_sf" ]] || continue
    _sn=$(basename "$_sf")
    [[ -n "${!_sn:-}" ]] && continue          # an explicit env var wins
    export "$_sn=$(tr -d '[:space:]' < "$_sf")"
    log "loaded secret $_sn"
done
unset _sf _sn

# --- SSH: marketplaces differ on which variable carries the injected key -----
KEY="${PUBLIC_KEY:-${SSH_PUBLIC_KEY:-${SSH_KEY:-${AUTHORIZED_KEYS:-}}}}"
if [[ -n "$KEY" ]]; then
    mkdir -p /root/.ssh && chmod 700 /root/.ssh
    grep -qxF "$KEY" /root/.ssh/authorized_keys 2>/dev/null || echo "$KEY" >> /root/.ssh/authorized_keys
    chmod 600 /root/.ssh/authorized_keys
    log "installed injected SSH key"
fi
if [[ -s /root/.ssh/authorized_keys ]]; then
    ssh-keygen -A >/dev/null 2>&1 || true
    sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config
    sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication no/'  /etc/ssh/sshd_config
    # Preserve env for ssh sessions so GH_TOKEN survives the login shell.
    sed -i 's/^#\?PermitUserEnvironment.*/PermitUserEnvironment yes/' /etc/ssh/sshd_config
    env | grep -E '^(GH_TOKEN|DATA_DIR|GRIMFANG_|REMOTE_SYNC|REMOTE_PORT|NODES|HASH_MB)=' \
        > /root/.ssh/environment 2>/dev/null || true
    chmod 600 /root/.ssh/environment 2>/dev/null || true
    /usr/sbin/sshd && log "sshd up (key auth only)"
else
    log "no SSH key injected; sshd not started"
fi

# --- Repo --------------------------------------------------------------------
mkdir -p "$DATA_DIR"/{pgn,logs}
if [[ -d "$REPO_DIR/.git" ]]; then
    log "refreshing $REPO_DIR"
    git -C "$REPO_DIR" fetch --all --prune || true
    git -C "$REPO_DIR" checkout "$BRANCH" 2>/dev/null || true
    git -C "$REPO_DIR" pull --ff-only || log "pull failed; keeping local state"
else
    log "cloning $REPO_URL ($BRANCH)"
    git clone --branch "$BRANCH" "$REPO_URL" "$REPO_DIR" || log "clone failed -- check networking"
fi
if [[ -d "$REPO_DIR" ]]; then
    git config --global --add safe.directory "$REPO_DIR" || true
    mkdir -p "$REPO_DIR/tools/books"
    [[ -e "$REPO_DIR/tools/fastchess" ]] || ln -s /opt/fastchess "$REPO_DIR/tools/fastchess"
fi

cat <<EOF

  Grimfang test pod
  -----------------
  toolchain  $(tr '\n' ' ' < /etc/grimfang-toolchain 2>/dev/null)
  fastchess  $(cut -c1-8 /etc/grimfang-fastchess-ref 2>/dev/null)
  repo       $REPO_DIR @ $(git -C "$REPO_DIR" rev-parse --short HEAD 2>/dev/null || echo 'NOT CLONED')
  data       $DATA_DIR $(mountpoint -q "$DATA_DIR" && echo '(persistent volume)' || echo '(EPHEMERAL - dies with pod)')
  token      $([[ -n "${GH_TOKEN:-}" ]] && echo present || echo 'NOT SET')

  podctl setup      build + bench gate
  podctl status
  podctl teardown   before you destroy this pod

EOF
exec "$@"
