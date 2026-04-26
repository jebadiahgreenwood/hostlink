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
