# Grimfang pod workflow

Deploy Grimfang testing on rented hourly hardware, work, save, destroy.

---

## TLDR

**One-time setup** (do this once, ever):

1. Commit these files, plus `echo 216361 > tools/BENCH_SIG` and
   `git add -f tools/books/8moves_v3.epd`
2. Push to `main`. GitHub Actions builds `ghcr.io/grimfang-labs/grimfang-pod:latest`
3. Make that package **public** (GitHub → Packages → Settings → visibility)
4. Create a fine-grained GitHub token: **this repo only**, Contents = read/write,
   7-day expiry

**Every pod, start to finish:**

```bash
# QuickPod: image = ghcr.io/grimfang-labs/grimfang-pod:latest
#           secret binding GH_TOKEN = github_pat_...
ssh -p <port> root@<ip>

podctl setup                                  # ~3 min: build + bench gate
podctl rung    v13 "LmrDeepExtra=0" "" 5      # SPRT  -> a decision
podctl measure v21 "LmrDeepExtra=0" "" 5000   # fixed -> a number
podctl save                                   # push results to GitHub
podctl teardown                               # checklist before destroying
```

Then destroy the pod and revoke the token.

**If `podctl setup` fails the bench gate, stop.** Every result from that build
would be worthless.

---

## Why the image exists

The time saved is ~3 minutes per pod. That is not the point.

The point is **pinning the toolchain**. Your bench signature is a function of
the compiler. Without a pinned image, a bench mismatch on a pod is ambiguous:
real bug, or just a different GCC on a different host? With the image, a
mismatch always means something real. Grimfang has already shipped UB that MSVC
tolerated and GCC optimised away — that class of bug is exactly what the bench
gate exists to catch, and ambiguity defeats it.

The image also pins **fastchess**, your tournament manager. A version bump
between pods could change results with no engine change at all.

---

## What lives where

| | contents | survives pod destruction |
|---|---|---|
| **Image** | GCC, cmake, ninja, fastchess (pinned), podctl wrapper, sshd | yes — it's in the registry |
| **`/workspace`** | repo clone, build, PGNs, logs | **only if a volume is mounted** |
| **GitHub** | `RESULTS.md`, `BENCH_SIG`, scripts, engine source | yes |
| **`REMOTE_SYNC`** | logs + results rsynced to your VPS | yes |

**No credentials in the image, ever.** Image layers are permanent and
recoverable. `GH_TOKEN` arrives at runtime through the host's secret binding.

The engine source is *not* baked in either — it's cloned at container start, so
a pod always tests current `main` rather than whatever was current when the
image was built. A stale baked-in engine silently produces results against the
wrong code.

---

## Persistent storage, honestly

On a community marketplace pod, **assume the disk is gone the moment the
contract ends**. QuickPod hosts are individuals; the reliability rating on your
current host is 92.9%, and pods stop permanently at contract end with no
restart.

So there are three tiers, and you should use all three:

1. **Volume at `/workspace`** — if the host offers one, mount it there and the
   repo and build survive pod restarts *on that host*. Convenient, not durable.
2. **`podctl save`** — commits `RESULTS.md` and `BENCH_SIG` and pushes to
   GitHub. This is your real persistence for anything small and irreplaceable.
3. **`podctl sync`** — rsyncs logs and results to your VPS. Set
   `REMOTE_SYNC=root@104.223.27.112:~/pod_results`.

PGNs are deliberately excluded from both save and archive. 260 MB of SPRT games
whose verdicts are already recorded is not worth moving. Pull a specific one by
hand if you need re-analysis.

**Run `podctl save` after every result, not at the end of the session.** A
verdict that exists only in a terminal buffer on rented hardware is one dropped
SSH connection from gone.

---

## Detailed setup

### 1. Repo layout

```
docker/Dockerfile
docker/entrypoint.sh
.github/workflows/pod-image.yml
tools/podctl.sh
tools/BENCH_SIG            <- one line, the current signature
tools/books/8moves_v3.epd  <- git add -f (it's probably gitignored)
```

Add to `.gitattributes` so Windows checkouts don't break the scripts:

```
*.sh text eol=lf
```

Without it, git rewrites the shell scripts to CRLF and the pod fails with
`bad interpreter: No such file or directory`.

Add to `.gitignore`:

```
/workspace/
tools/pgn/
tools/logs/
```

### 2. Build the image

Push to `main`. The workflow triggers on changes under `docker/`, or manually
via **Actions → pod-image → Run workflow**.

Then make the package public: **GitHub → your profile → Packages →
grimfang-pod → Package settings → Change visibility → Public**. Marketplaces
generally cannot authenticate to a private registry.

### 3. Token

GitHub → Settings → Developer settings → Personal access tokens →
**Fine-grained tokens** → Generate new:

- Repository access: **only `grimfang-labs/grimfang`**
- Permissions: **Contents = Read and write**. Nothing else.
- Expiration: **7 days**

This is community hardware and the host operator has root. Scoped this way, the
worst case is junk commits to one public repo — revertible and revocable. Never
put a broad-scope token or an SSH key on a pod.

### 4. Create the pod

QuickPod → pick a machine → Create Pod:

- **Template**: `ghcr.io/grimfang-labs/grimfang-pod:latest` (custom image field)
- **Secret Binding**: `GH_TOKEN` = your token
- Optional env: `REMOTE_SYNC`, `GRIMFANG_BRANCH` (defaults to `main`)
- Storage: 20–30 GB

**Check `nproc` against the cgroup quota before trusting the core count.**
`podctl setup` does this automatically and warns — on a shared host `nproc`
reports the whole machine. Your current pod advertises 128 threads and grants
32.

### 5. Work

```bash
podctl setup      # deps are baked in; this builds Grimfang + gates bench + reports NPS
podctl status     # allocation, unpushed commits, running matches
podctl pull       # after you merge something on the desktop: pull, rebuild, re-gate
```

Running tests:

```bash
podctl rung    <tag> "<dev opts>" ["<base opts>"] [elo1]
podctl measure <tag> "<dev opts>" ["<base opts>"] [rounds]
```

`rung` and `measure` are separate commands on purpose. **SPRT gives a decision;
fixed-length gives a number.** An SPRT stops when evidence looks favourable, so
its stopping rule correlates with upward noise and its point estimate is
inflated — roughly 3× in the v01/v10 vs v20 case. Keeping the two commands
distinct makes that error harder to repeat.

Both append to `RESULTS.md` automatically, the moment the run ends, including a
`RUN FAILED (0 games)` marker when a match never started.

Long runs go in tmux:

```bash
tmux new -s work
podctl measure v21 "LmrDeepExtra=0" "" 5000
# Ctrl-B then D to detach
```

### 6. Teardown

```bash
podctl teardown
```

Checks for still-running matches, commits and pushes anything outstanding,
builds an archive, syncs to `REMOTE_SYNC` if set, prints the exact `scp` command
for the archive and the three largest PGNs, and tells you whether it's safe to
destroy.

Then: pull the archive, destroy the pod, **revoke the token**.

---

## Cost

At $0.09/hr on a 32-core EPYC:

| job | wall time | cost |
|---|---|---|
| `podctl setup` | ~3 min | $0.005 |
| SPRT rung, decisive | 20–60 min | ~$0.05 |
| SPRT rung, null (runs full 6,000 games) | ~2.6 h | ~$0.24 |
| 10,000-game fixed measurement | ~4.5 h | ~$0.40 |
| Full SPSA batch (64k games) | ~28 h | ~$2.50 |
| Datagen v4 | ~11 h | ~$1.00 |

Budget for the null case, not the decisive one. A change that does nothing
never triggers either SPRT boundary and runs every game.

---

## Known unknowns

**QuickPod's custom-image contract is unverified.** The entrypoint handles
`PUBLIC_KEY`, `SSH_PUBLIC_KEY`, `SSH_KEY`, and `AUTHORIZED_KEYS`, which covers
the usual marketplace conventions, and starts sshd only when a key actually
arrives. But if QuickPod overrides `ENTRYPOINT` or injects keys differently,
**you will be locked out of the pod**.

**Test the image on a one-hour rental before relying on it.** That's a wasted
dollar; discovering it mid-project is a wasted evening.

**Steal is not constant.** `st=0` at setup doesn't mean `st=0` in six hours —
other tenants arrive. `podctl setup` starts a background `vmstat` logger to
`$DATA_DIR/steal.log`. Under fixed-node TC steal cannot corrupt results, but it
does mean you're paying for cycles you don't get.
