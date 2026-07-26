# HostLink

A lightweight daemon written in C that allows authorized clients to execute shell commands on a host machine and receive structured output (stdout, stderr, exit code).

## Features

- **Two transports:** Unix domain socket (local Docker-to-host) and TCP over WireGuard (remote hosts)
- **Simple wire protocol:** Length-prefixed frames with JSON payloads
- **Pre-shared token auth** with constant-time comparison
- **Output capture:** Inline (with configurable size limits) or file-based for large outputs
- **Detached jobs:** `--detach` returns a job id; tail, wait on, or cancel it later
- **Concurrency control:** Configurable max concurrent executions
- **Graceful shutdown** and hot config reload via signals
- **systemd integration** with `Restart=always`

## Architecture

```
OpenClaw (Docker) ──unix socket──► hostlinkd (local host)
                  ──TCP/WireGuard─► hostlinkd (DGX Spark, cluster nodes)
```

## Quick Start

### Build

```bash
./build.sh release
# Produces: build/hostlinkd, build/hostlink-cli
```

### Install

```bash
sudo ./install.sh
# Creates hostlink user, generates token, enables systemd service
```

### Configure client

```ini
# ~/.config/hostlink/targets.conf
[desktop]
transport = unix
socket = /run/hostlink/hostlink.sock
token = <your-token>

[dgx-spark]
transport = tcp
address = 10.0.0.2
port = 9876
token = <your-token>
```

### Use

```bash
# Execute a command — remaining arguments are joined, as with ssh
hostlink-cli -t desktop exec "uname -a"
hostlink-cli -t desktop exec stat -c%s /var/log/syslog

# JSON output (for scripting/agents)
hostlink-cli -t dgx-spark -j exec "nvidia-smi"

# Transfer flags may go anywhere among the paths
hostlink-cli -t desktop put ./f /srv/new/dir/f --mkdir
hostlink-cli -t desktop put --mkdir ./f /srv/new/dir/f

# Directories transfer in one command, in both directions — no -r
hostlink-cli -t desktop put ./models /srv/models
hostlink-cli -t desktop put ./models /srv/models --exclude .cache --exclude '*.tmp'

# Verify a directory put: every file streamed and sha256-checked by the remote,
# with a sha256sum-format manifest on stdout and a pass/fail count at the end
hostlink-cli -t desktop put ./models /srv/models --verify > manifest.txt

# Free space on the far side (the check `put` runs for you before a directory
# transfer). Walks up to the nearest existing ancestor, so it works on a
# destination that does not exist yet.
hostlink-cli -t desktop df /srv/models/new

# Ping
hostlink-cli -t desktop ping

# List targets
hostlink-cli targets
```

## Detached jobs

`--detach` used to be a one-way door: the command went off into the background
and nothing came back — no id, no output, no exit status. The workaround was to
redirect to a file inside the command and poll it by hand.

Since 1.6.0 it returns a job id, and the daemon spools the job:

```bash
id=$(hostlink-cli -t build-lab -D exec "cargo build --release")

hostlink-cli -t build-lab job tail   "$id" -f     # stream the log as it grows
hostlink-cli -t build-lab job status "$id"        # state, exit code, sizes
hostlink-cli -t build-lab job wait   "$id"        # block; exits with ITS status
hostlink-cli -t build-lab job cancel "$id"        # signal the process group
hostlink-cli -t build-lab job list                # what has run lately
```

The id goes to stdout so it can be captured; the human-readable line goes to
stderr. `job wait` exits with the job's own status, so `job wait "$id" && deploy`
reads the way it should.

**What to know about it:**

- A job **survives a daemon restart** — the registry is the spool directory on
  disk, not a table in the daemon's memory. It does *not* survive
  `systemctl restart` of the unit, because setsid does not leave the cgroup and
  systemd tears the whole group down. A job killed that way reports `lost`
  rather than pretending to still be running: the exit code is unknowable.
- **Output is capped** at `job_max_spool_bytes` per stream. Past the cap the
  daemon keeps draining the pipe (so the command never blocks) but stops
  writing, and reports how much was produced.
- **Finished spools are swept** after `job_retention_s`.
- `wait` and `-f` poll from the client rather than blocking the daemon: a
  server-side wait would hold a forked worker for the whole life of the job,
  and a handful of waiters would shut every transfer out of the daemon.
- `--detach` against a pre-1.6.0 daemon still works and simply returns no id.

## Per-target defaults

The right timeout is usually a property of the target, not of the caller: a
build container compiles for minutes where a host answers in milliseconds. Set
it once, in `targets.conf`:

```ini
[build-lab]
transport = unix
socket = /run/hostlink/hostlink-lab.sock
token = ...
timeout_ms = 600000
```

An explicit `-T` still wins. And if a `-T` exceeds the daemon's own
`max_timeout_ms`, the client now says so — it used to be clamped in silence,
which meant a command could die at five minutes when you had asked for ten with
nothing anywhere admitting the request had been overruled.

## Client Exit Codes

`exec` reports the **remote command's own exit status, verbatim**, so ordinary
shell idioms work across the link:

```bash
hostlink-cli -t desktop exec "test -f /etc/fstab" && echo present
hostlink-cli -t desktop exec "grep -q root /etc/passwd"; echo $?   # grep's own 0/1/2
```

Two values are reserved for hostlink's own failures, following the
`timeout(1)`/`env(1)` convention, so they cannot be confused with a remote
status:

| Code | Meaning |
|------|---------|
| 0 | Remote command succeeded |
| 1–123, 126–255 | The remote command's own exit status (128+N if it died on signal N) |
| 124 | Remote command hit the timeout and was killed |
| 125 | HostLink itself failed: connection, authentication, protocol, or usage error |

`put`/`get`/`ping`/`targets` have no remote status to pass through and report
simply 0 (ok) or 125 (failed).

Which transport failure occurred is on stderr, and machine-readably in `-j`
JSON output.

> **Changed in 1.5.0.** Previously every remote failure collapsed to `1`, so a
> remote `exit 7` and a remote `false` were indistinguishable and callers
> inspecting a specific status silently got the wrong answer. Transport errors
> also moved from `2`–`7` into the reserved block.

## Directory transfers

`get` and `put` both handle directories transparently — there is no `-r`, and
passing one is an error rather than a silent no-op. Semantics are cp-style and
identical in both directions: `put ./foo /srv/bar` places files at
`/srv/bar/<relative path>`.

Both walk with `nftw(..., FTW_PHYS)` and transfer **regular files only**, so the
two verbs always agree on what "the tree" is:

- **Symlinks are not followed and not transferred** — no loops, and no surprise
  copy of whatever a link happened to point at. Skipped entries are counted and
  reported, not passed over silently.
- **Empty directories are not recreated**, since only files move. This is a
  shared limitation of both verbs, not an asymmetry.
- Ceiling of 100,000 files per tree on each side; exceeding it is an error.
- Files over 90 MiB stream automatically; `--stream` forces the sha256-verified
  streaming path for every file, which doubles as a per-file integrity manifest.
- A directory `put` always implies `--mkdir` — the daemon is the only thing that
  can create the remote parents.
- Transfers stop at the first failure, leaving what already landed in place so
  you can see how far it got.

`--exclude <glob>` (repeatable, `put` only) skips paths matching the glob. It
matches a basename, a full relative path, or **any leading directory
component** — so `--exclude .cache` prunes the entire `.cache/` subtree, which
is what you want after a HuggingFace `--local-dir` download.

> **New in 1.5.1.** `get` gained directory support in 1.4.0; `put` was strictly
> one file until now, so shipping a tree meant a hand-written `find | while
> read` loop plus pre-creating every nested subdirectory separately.
>
> One asymmetry remains: `get` checks local free space before starting
> (`statvfs`), `put` cannot, because the daemon exposes no remote free-space
> query. A `put` that fills the remote filesystem fails on the file that runs
> out of room.

## Process-matching commands (`pgrep -f`, `pkill -f`)

The command is delivered to the remote shell through the environment, not
`argv`, so it never appears in the exec shell's `/proc/<pid>/cmdline`.

> **Changed in 1.5.0.** Before this, the exec shell's command line *was* the
> command text, so `pgrep -fc 'no_such_pattern'` returned 2, and `pkill -f
> <pattern>` could match and kill its own session mid-command — which happened,
> orphaning a server and leaving a port bound. The `[p]attern` trick does not
> help, since the pattern still appears verbatim in the wrapper's command line.

**This fixes the server side only.** The *client* still receives the pattern as
a normal command-line argument, so `hostlink-cli`'s own `argv` contains it. If
the client is visible in the target's process table — notably a container
client against its own host, since the host's `/proc` shows container processes
— those client processes still match. Measured on the reference deployment:
matches went from 3 to 2, and the 2 remaining are the caller's own wrapper and
client, not hostlink's server-side machinery.

When the match has to be exact, keep the literal out of the command line:

```bash
# assemble the pattern on the remote side
hostlink-cli -t desktop exec 'p=zzz_uni; pgrep -f "${p}que"'

# or capture the PID at launch and match on that instead
hostlink-cli -t desktop exec "kill $known_pid"

# to inspect rather than kill: snapshot, then grep the FILE in a separate call,
# so the reading command's own cmdline is not part of what is searched
hostlink-cli -t desktop exec "ps -eo pid,args > /tmp/ps.txt"
hostlink-cli -t desktop exec "grep some-pattern /tmp/ps.txt"
```

## Docker Integration

```bash
docker run \
  -v /run/hostlink/hostlink.sock:/run/hostlink/hostlink.sock \
  -v /run/hostlink/output:/run/hostlink/output:ro \
  -v /path/to/targets.conf:/etc/hostlink/targets.conf:ro \
  --network host \
  your-image
```

The `hostlink-cli` binary should be in the container image. The agent can then execute `hostlink-cli -t desktop -j exec "<command>"` directly.

## Large Output

For commands with large output, use `--output-to-file` / `-F`:

```bash
hostlink-cli -t dgx-spark -j -F exec "tar tf /data/big.tar.gz"
# Returns: {"stdout_file": "/run/hostlink/output/hl_<id>_stdout", ...}
# Then read with: head -n 100 /run/hostlink/output/hl_<id>_stdout
# Clean up: rm /run/hostlink/output/hl_<id>_*
```

## WireGuard Setup

### Desktop (`10.0.0.1`)

```ini
# /etc/wireguard/wg0.conf
[Interface]
PrivateKey = <desktop_private_key>
Address = 10.0.0.1/24
ListenPort = 51820

[Peer]
PublicKey = <dgx_spark_public_key>
AllowedIPs = 10.0.0.2/32
Endpoint = <dgx_spark_lan_ip>:51820
PersistentKeepalive = 25
```

### DGX Spark (`10.0.0.2`)

```ini
[Interface]
PrivateKey = <dgx_spark_private_key>
Address = 10.0.0.2/24
ListenPort = 51820

[Peer]
PublicKey = <desktop_public_key>
AllowedIPs = 10.0.0.1/32
Endpoint = <desktop_lan_ip>:51820
PersistentKeepalive = 25
```

Activate: `wg-quick up wg0` on both. Verify: `ping 10.0.0.2` from desktop.

In `hostlinkd.conf` on each machine, set `tcp_bind` to that machine's WireGuard IP.

## Security Notes

- The auth token is the primary security boundary for TCP connections
- Unix socket permissions (group ownership) control local access
- Always bind TCP to the WireGuard interface IP, never `0.0.0.0`
- The daemon executes arbitrary shell commands — the security model relies on controlling who can authenticate, not sanitizing commands
- Tokens are compared in constant time to prevent timing attacks

## Build Modes

```bash
./build.sh release   # Optimized build
./build.sh debug     # Debug symbols
./build.sh test      # Build + run all tests
./build.sh clean     # Remove build artifacts
```

## Protocol

Frames: `[magic(4)][len(4)][JSON payload(len)]`
- Magic: `0x484C4E4B` ("HLNK")
- Max payload: 128 MiB
- All integers in network byte order
