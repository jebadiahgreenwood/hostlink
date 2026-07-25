# HostLink - Build Progress

## Status: ✅ Code Complete — ⚠️ Push Blocked (exec unavailable in subagent)

## Repo
- URL: https://github.com/jebadiahgreenwood/hostlink

## Spec
- Location: `hostlink/SPEC.md` ✅

## Build Plan (ordered)

### Phase 1: Scaffold & Common ✅
- ✅ 1.1 Repo structure (directories, Makefile skeleton, build.sh)
- ✅ 1.2 Vendor cJSON (single-header, src/common/cjson/)
- ✅ 1.3 src/common/log.c/.h — logging (syslog/stderr/file, levels)
- ✅ 1.4 src/common/util.c/.h — constant-time compare, buffer helpers
- ✅ 1.5 src/common/protocol.c/.h — frame encode/decode, JSON parse/emit
- ✅ 1.6 src/common/config.c/.h — INI config parser
- ✅ 1.7 tests/test_protocol.c — unit tests for protocol
- ✅ 1.8 tests/test_config.c — unit tests for config
- ✅ 1.9 tests/fixtures/ — valid.conf, no_token.conf, empty.conf, with_comments.conf, targets.conf

### Phase 2: Daemon ✅
- ✅ 2.1 src/daemon/main.c — arg parsing, daemonize, PID file, signals
- ✅ 2.2 src/daemon/server.c/.h — epoll event loop, accept, dispatch, SIGHUP hot reload
- ✅ 2.3 src/daemon/executor.c/.h — fork/exec, pipe capture, timeout, file mode
- ✅ 2.4 hostlink.conf.example
- ✅ 2.5 hostlink.service (systemd unit)

### Phase 3: Client ✅
- ✅ 3.1 src/client/connection.c/.h — unix/tcp connect, send/recv frames
- ✅ 3.2 src/client/main.c — arg parsing, exec/ping/targets subcommands
- ✅ 3.3 targets.conf.example

### Phase 4: Tests & Install ✅
- ✅ 4.1 tests/run_tests.sh
- ✅ 4.2 tests/test_integration.sh (all test cases from spec §9.2)
- ✅ 4.3 install.sh
- ✅ 4.4 README.md

### Phase 5: Push to GitHub ✅
- ✅ 5.1 Init repo / pushed 2026-04-11
- ✅ 5.2 Verify build compiles cleanly

## To Complete Phase 5 (run in terminal)
```bash
# Push to GitHub
cd /home/node/.openclaw/workspace
gh repo clone jebadiahgreenwood/hostlink /tmp/hostlink-repo 2>/dev/null || git clone https://github.com/jebadiahgreenwood/hostlink /tmp/hostlink-repo
cp -r hostlink/. /tmp/hostlink-repo/
cd /tmp/hostlink-repo
git add -A
git commit -m "feat: complete HostLink v1.0 implementation

- hostlinkd daemon: epoll event loop, fork/exec model, SIGHUP hot reload
- hostlink-cli: exec/ping/targets subcommands, Unix+TCP transport
- Wire protocol: length-prefixed frames with JSON payloads
- Full test suite: unit tests (protocol, config) + integration tests
- All spec section 9.2 test cases implemented
- systemd unit, install.sh, example configs, README"
git push origin main
```

## Resume Instructions
1. Read `hostlink/SPEC.md` for full requirements
2. Check this file for last completed task (look for ✅)
3. All source is written; just need to build and push
4. GitHub repo URL: https://github.com/jebadiahgreenwood/hostlink

## Key Design Notes
- cJSON chosen for JSON (single-header, no external deps, vendored)
- epoll (Linux-only, as per spec — x86_64 + aarch64 only)
- No threads — fork() model per spec
- Token comparison must be constant-time (util.c)
- executor.c: child calls setpgrp() so kill(-pgid) works on timeout
- server.c: SIGHUP hot-reloads auth_token, max_concurrent, timeouts, output limits, log_level
- All 35+ integration test cases from spec §9.2 implemented

---

## v1.1.0 — 2026-04-11 ✅

### New Features
- **`put` subcommand** — binary-safe file transfer (container→host or container→spark)
  - base64-encodes locally, sends as HLNK `put` frame, daemon decodes and writes
  - `--mkdir` creates parent directories; `--mode <octal>` sets permissions
  - `hl-put` wrapper added to `bin/` for ergonomic use
- **`--detach` exec flag** — fire-and-forget command execution
  - daemon double-forks: intermediate calls setsid(), grandchild adopted by init
  - returns in <100ms regardless of command duration
  - useful for starting background servers, long-running jobs
- **`hl_b64_encode/decode`** — RFC 4648 base64 in util.c/h (no line wrapping)

### Tests
- 8/8 live tests passed: basic put, mkdir, binary integrity, content verify,
  detach timing, detach execution, hl-put wrapper

### Deployment Notes
- Host (ramboot): compiled on host (glibc 2.38), installed to /usr/local/bin/
- Container: compiled inside container (glibc 2.36), installed to bin/hostlink
- Spark (aarch64): bootstrapped via HTTP relay (exec+detach trick → wget over 10GbE)
  - Two-step: `hl --detach exec "python3 -m http.server PORT --bind 10.0.0.1"`
              then `hl-spark "wget http://10.0.0.1:PORT/file"`
- Future: add `make cross-compile` target for clean multi-arch builds (#13 in OPTIMIZE_ENVIRONMENT.md)

### Known Friction Points Added (OPTIMIZE_ENVIRONMENT.md)
- #11: `hl-put` impractical for files >50MB (single-frame, in-memory base64)
- #12: `hl-get` missing (host→container direction)
- #13: Cross-compilation needed for clean multi-arch deploy

---

## v1.2.0 — 2026-04-11 ✅

### New Feature: `get` subcommand
- **`get <remote_path> <local_path>`** — binary-safe file retrieval (daemon→client)
  - Daemon reads file, base64-encodes, sends as HLNK `get` response frame
  - 90 MiB max file size (fits under 128 MiB frame limit after base64 overhead)
  - Error responses for missing, unreadable, and oversized files
  - Client decodes base64, writes to local path
  - `-j` mode: writes file AND prints JSON (unlike other subcommands where -j replaces output)
  - `hl-get` wrapper added to `bin/` for ergonomic use

### Deployment
- **Host (ramboot):** v1.2.0 compiled, installed, daemon restarted via systemd ✅
- **Container:** v1.2.0 compiled, installed to `bin/hostlink` ✅
- **Spark (aarch64):** v1.2.0 compiled and binary installed ⚠️ **Daemon needs manual restart**
  - Binary at `/usr/local/bin/hostlinkd` is updated
  - Run on Spark: `systemctl restart hostlinkd`

### Tests
- 5 new integration tests: text get, binary SHA256 verify, nonexistent file, empty file, JSON field validation
- **67/67 integration tests pass** (62 existing + 5 new)

### Bug Fixes
- `hl-spark` wrapper: `put|get` case was hardcoding `put` subcommand for both — fixed to use `$subcmd`
- `fscanf` return value warnings on host GCC (stricter `-Werror=unused-result`)

### Usage
```bash
# From container:
hl-get /etc/hostname /tmp/host-hostname.txt
hl-get -t spark /root/results.json /tmp/results.json
hl-spark get /root/model.safetensors /tmp/model.safetensors
hl get /var/log/syslog /tmp/syslog.txt
```

---

## v1.5.0 — argument handling, exit status, and signal hygiene (2026-07-25)

Five defects, all of one family: **the client or daemon quietly did something
other than what was asked, and reported success.** Each was reproduced before
being fixed and is covered by `tests/test_regressions.sh`.

### Fixed

- **`exec` dropped every argument after the first.** `exec stat -c%s /path` ran
  a bare `stat`; `exec ls /dir` listed `/`. This once produced a false "all
  files MISSING" verification report after a transfer that had entirely
  succeeded. `exec` now joins `argv[optind..]` the way ssh does. The `bin/hl*`
  wrappers pre-join into a single token, so they were never affected and are
  unchanged by this (join-of-one is the identity).

- **`put`/`get` flags were position-dependent.** `--mkdir` *after* the paths was
  parsed by nobody and silently dropped; *before* the paths it was consumed as
  the local path. Both now work, along with `--mode` and `--stream`, in any
  position — and an unknown flag or a third path is now an error instead of
  being ignored.

- **Remote exit status collapsed to 1.** A remote `exit 7`, a remote `false`,
  and a remote `grep` no-match were indistinguishable. `exec` now passes the
  remote status through verbatim, with 124 (timeout) and 125 (hostlink itself
  failed) reserved. See README § *Client Exit Codes*.

- **`bad_request` responses were reported as success.** The daemon sends
  `status:"bad_request"` for a missing or malformed command, but the client only
  tested for `status:"error"` — so those fell through to the success branch and
  printed `exit=0`. Any unrecognised status now fails closed. Relatedly, a
  command longer than the daemon's 8 KiB buffer was silently truncated and the
  *fragment* executed; it is now rejected with an explicit error.

- **The command text sat in the exec shell's `argv`,** so process-pattern tools
  run over hostlink matched hostlink itself: `pgrep -fc <impossible pattern>`
  returned 2, and `pkill -f` killed its own session mid-command. The command now
  travels in `HOSTLINK_COMMAND` and the shell runs a fixed stub that unsets it
  before `eval`. Server side only — see README for the client-side caveat.

- **The daemon's signal state leaked into every command.** It blocks
  HUP/INT/TERM/CHLD for its signalfd and ignores PIPE; a signal mask survives
  `fork`+`execve` and `SIG_IGN` survives `execve`. So remote commands ran with
  those signals blocked: `yes | head -1` did not die on SIGPIPE (producer
  reported 1 instead of 141), a command could not be terminated by its own
  `kill`, and — worst — the timeout path's own `SIGTERM` was blocked by the
  child, so every timeout waited out the full 2 s escalation to `SIGKILL`.
  The child now resets its mask and dispositions before exec; timeouts stop in
  ~0.6 s instead of ~2.5 s.

Also: the daemon reported `VERSION "1.0.0"` regardless of release; client and
daemon are now both `1.5.0`.

### Tests

`tests/test_regressions.sh [shell] [tree]` — 61 assertions, spins up a
throwaway daemon on a private socket and PID file, so it can run alongside a
live one. Run it under both shells; the daemon's configured shell is
deployment-specific.

Against this release: **61/61 (bash), 60/60 (sh)**. Against the unpatched
tree for comparison: **26/61**.

### Deployment (2026-07-25)

All seven links on 1.5.0, cut over with the backup-daemon procedure (primary
first, verify, then backup — the backup keeps the old binary in memory and is
the recovery path):

| target | how built |
|---|---|
| host (ramboot, x86_64) — primary, backup, ro | built natively on host |
| Spark (aarch64) — primary, backup, ro | source synced, built natively on Spark |
| build-lab (container, x86_64) | host-built binary, container restarted |

**Two operational notes learned during this cutover:**

1. **Never restart a daemon through its own link.** `systemctl restart
   hostlinkd` issued over `hostlinkd` deadlocks: the restarting process is
   itself a worker inside the unit's cgroup, so systemd's stop job waits on the
   process that is waiting for the stop job. The unit sticks in `deactivating`.
   Restart the primary *through the backup link* and the backup *through the
   primary* — different units, no deadlock.
2. **Old and new speak the same protocol,** so a mixed fleet is fine during a
   staged rollout (verified: 1.5.0 client against a 1.0.0 daemon).
