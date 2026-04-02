# HostLink — Secure Host Command Execution Agent

## Specification v1.0

---

## 1. Overview

HostLink is a lightweight daemon written in C that allows authorized clients (such as an OpenClaw Docker container) to execute shell commands on the host machine and receive structured output (stdout, stderr, exit code). It supports two transport modes simultaneously:

- **Unix domain socket** — for local Docker-to-host communication on the same machine.
- **TCP over WireGuard** — for remote host-to-host communication across a local network.

The daemon is designed to be generic and multi-target: a single OpenClaw instance can connect to multiple HostLink agents running on different machines (desktop, DGX Spark, future cluster nodes) and address them by a human-readable node name.

---

## 2. Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│ Host Machine (Desktop)                                                 │
│                                                                        │
│  ┌────────────────────────────────────┐                                │
│  │ Docker: OpenClaw Agent             │                                │
│  │                                    │                                │
│  │  hostlink-cli -t desktop exec ... │──┐                              │
│  │  hostlink-cli -t dgx-spark exec ..│──┼──┐                          │
│  └────────────────────────────────────┘  │  │                          │
│                                          │  │                          │
│  ┌────────────────────────────────────┐  │  │                          │
│  │ hostlinkd (daemon)                 │◄─┘  │  (WireGuard tunnel)      │
│  │ listening on:                      │     │                          │
│  │   /run/hostlink/hostlink.sock      │     │                          │
│  │   10.0.0.1:9876 (wg0)             │     │                          │
│  └────────────────────────────────────┘     │                          │
└─────────────────────────────────────────────┼──────────────────────────┘
                                              │
                              ┌───────────────┘
                              │
┌─────────────────────────────┼──────────────────────────────────────────┐
│ DGX Spark                   │                                          │
│                             ▼                                          │
│  ┌────────────────────────────────────┐                                │
│  │ hostlinkd (daemon)                 │                                │
│  │ listening on:                      │                                │
│  │   /run/hostlink/hostlink.sock      │                                │
│  │   10.0.0.2:9876 (wg0)             │                                │
│  └────────────────────────────────────┘                                │
└────────────────────────────────────────────────────────────────────────┘
```

### 2.1 Components

| Binary        | Role                                                                 |
|---------------|----------------------------------------------------------------------|
| `hostlinkd`   | The daemon. Listens for connections, executes commands, returns results. |
| `hostlink-cli`| The client. Sends a command to a specified target and prints the result. |

---

## 3. Wire Protocol

All communication uses a simple length-prefixed binary protocol. Every message is a **frame**:

```
┌───────────┬──────────┬──────────────────┐
│ magic (4) │ len (4)  │ payload (len)    │
└───────────┴──────────┴──────────────────┘
```

| Field     | Type       | Description                                                      |
|-----------|------------|------------------------------------------------------------------|
| `magic`   | `uint32`   | Fixed value `0x484C4E4B` (ASCII "HLNK"), network byte order.   |
| `len`     | `uint32`   | Length of `payload` in bytes, network byte order. Max: 128 MiB. |
| `payload` | `bytes`    | JSON-encoded message body (UTF-8).                              |

### 3.1 Request Payload (client → daemon)

```json
{
  "version": 1,
  "type": "exec",
  "id": "a1b2c3d4",
  "token": "shared-secret-here",
  "command": "nvidia-smi --query-gpu=name --format=csv,noheader",
  "timeout_ms": 30000,
  "max_stdout_bytes": 4194304,
  "max_stderr_bytes": 4194304,
  "output_to_file": false,
  "env": {
    "CUDA_VISIBLE_DEVICES": "0"
  },
  "workdir": "/home/user"
}
```

| Field              | Type     | Required | Description                                                       |
|--------------------|----------|----------|-------------------------------------------------------------------|
| `version`          | `int`    | yes      | Protocol version. Must be `1`.                                   |
| `type`             | `string` | yes      | Message type. `"exec"` or `"ping"`.                              |
| `id`               | `string` | yes      | Client-generated unique request ID. UUID4 or unique string ≤64 chars. |
| `token`            | `string` | yes      | Pre-shared authentication token.                                 |
| `command`          | `string` | yes*     | Shell command to execute (passed to `/bin/sh -c`). Required for `type: "exec"`. |
| `timeout_ms`       | `int`    | no       | Max execution time in ms. Default: 30000. Max: 300000. 0 = use default. |
| `max_stdout_bytes` | `int`    | no       | Max bytes of stdout to capture. Default: daemon's `default_max_output_bytes`. |
| `max_stderr_bytes` | `int`    | no       | Max bytes of stderr to capture. Same rules as `max_stdout_bytes`. |
| `output_to_file`   | `bool`   | no       | If true, write stdout/stderr to temp files instead of inline. Default: false. |
| `env`              | `object` | no       | Additional env vars. Merged with daemon's base environment.      |
| `workdir`          | `string` | no       | Working directory. Must be absolute. Default: `/`.               |

### 3.2 Response Payload (daemon → client)

```json
{
  "version": 1,
  "id": "a1b2c3d4",
  "node": "desktop",
  "status": "ok",
  "exit_code": 0,
  "stdout": "NVIDIA GeForce RTX 4090\n",
  "stderr": "",
  "stdout_truncated": false,
  "stderr_truncated": false,
  "stdout_original_bytes": 24,
  "stderr_original_bytes": 0,
  "duration_ms": 142
}
```

| Field                   | Type     | Description                                                          |
|-------------------------|----------|----------------------------------------------------------------------|
| `version`               | `int`    | Protocol version. Always `1`.                                       |
| `id`                    | `string` | Echoed request ID.                                                  |
| `node`                  | `string` | Daemon's configured node name.                                      |
| `status`                | `string` | One of: `"ok"`, `"error"`, `"timeout"`, `"auth_failed"`, `"bad_request"`. |
| `exit_code`             | `int`    | Process exit code. -1 if not started. -2 if timed out.             |
| `stdout`                | `string` | Captured stdout. Omitted when `output_to_file` was true.           |
| `stderr`                | `string` | Captured stderr. Omitted when `output_to_file` was true.           |
| `stdout_truncated`      | `bool`   | true if stdout exceeded `max_stdout_bytes`.                         |
| `stderr_truncated`      | `bool`   | true if stderr exceeded `max_stderr_bytes`.                         |
| `stdout_original_bytes` | `int`    | Total bytes produced on stdout before truncation.                   |
| `stderr_original_bytes` | `int`    | Total bytes produced on stderr before truncation.                   |
| `stdout_file`           | `string` | Path to temp file with full stdout. Present only when `output_to_file` true. |
| `stderr_file`           | `string` | Path to temp file with full stderr. Present only when `output_to_file` true. |
| `duration_ms`           | `int`    | Wall-clock execution time in milliseconds.                          |
| `error_msg`             | `string` | Human-readable error. Present when status is not `"ok"`.           |

### 3.3 Ping / Health Check

A `type: "ping"` request requires only `version`, `type`, `id`, and `token`. Response:

```json
{
  "version": 1,
  "id": "...",
  "node": "desktop",
  "status": "ok",
  "uptime_s": 84523
}
```

---

## 4. Daemon (`hostlinkd`)

### 4.1 Command-Line Interface

```
hostlinkd [OPTIONS]

OPTIONS:
  -c, --config <path>   Path to config file (default: /etc/hostlink/hostlink.conf)
  -f, --foreground      Run in foreground (do not daemonize).
  -v, --verbose         Increase log verbosity. Repeatable (-vv, -vvv).
  -h, --help            Print help and exit.
  -V, --version         Print version and exit.
```

### 4.2 Configuration File

INI-style. `key = value`, `#` for comments.

```ini
# /etc/hostlink/hostlink.conf

node_name = desktop
auth_token = a3f8...long_hex_string...9c12

# --- Unix socket transport ---
unix_enabled = true
unix_path = /run/hostlink/hostlink.sock
unix_mode = 0660
unix_group = hostlink

# --- TCP transport ---
tcp_enabled = true
tcp_bind = 10.0.0.1
tcp_port = 9876

# --- Execution limits ---
max_concurrent = 8
default_timeout_ms = 30000
max_timeout_ms = 300000
shell = /bin/sh

# --- Output limits ---
default_max_output_bytes = 4194304
max_output_bytes = 67108864
output_tmpdir = /run/hostlink/output

# --- Logging ---
log_target = syslog
log_level = info

# --- Security ---
run_as_user = hostlink
```

### 4.3 Daemon Lifecycle

1. Parse args → read/validate config → bind sockets → drop privileges → fork to background → write PID → install signal handlers → enter event loop.
2. **Shutdown (SIGTERM/SIGINT):** Stop accepting, wait up to 5s for in-flight commands, SIGKILL remaining, remove socket/PID files, exit 0.
3. **Reload (SIGHUP):** Re-read config. Hot-reloadable: `auth_token`, `max_concurrent`, timeout values, `log_level`, output limits. Socket/bind changes require full restart.

### 4.4 Signal Handling

| Signal    | Action                                            |
|-----------|---------------------------------------------------|
| `SIGTERM` | Graceful shutdown.                                |
| `SIGINT`  | Graceful shutdown.                                |
| `SIGHUP`  | Reload configuration.                             |
| `SIGCHLD` | Reap child processes (`waitpid` with `WNOHANG`). |
| `SIGPIPE` | Ignored (`SIG_IGN`).                             |

All handlers must be async-signal-safe. Use self-pipe or `signalfd`.

### 4.5 Event Loop and Concurrency Model

Single-threaded epoll event loop + `fork()` for command execution:

1. Monitor listener sockets and active client connections via epoll.
2. On connect: accept and add fd to epoll set.
3. On complete frame: validate (auth, schema).
4. Fork child: parent creates stdout/stderr pipes, forks; child redirects and calls `execve`; parent monitors via SIGCHLD and pipe fds via epoll.
5. On child complete/timeout: assemble and send response, close connection.

**Concurrency limit:** If `max_concurrent` children running, respond immediately with `error` / "server busy".

### 4.6 Command Execution Details

1. Resolve effective limits: `eff_max_stdout = min(request.max_stdout_bytes or default, config.max_output_bytes)`. If `max_output_bytes` = 0, no clamping.
2. If `output_to_file`: create `hl_<id>_stdout` and `hl_<id>_stderr` in `output_tmpdir` (permissions 0600). Create dir if missing; error if creation fails.
3. Create stdout/stderr pipes. Fork.
4. **Child:** redirect stdout/stderr, close excess fds, set workdir, merge env, `execve(shell, [shell, "-c", command], env)`. On execve fail: write error, `_exit(126)`.
5. **Parent (inline mode):** read into dynamic buffers. Track `original_bytes`. When buffer exceeds limit, drain pipe but stop appending, set `truncated=true`. Trim final buffer to exactly the limit.
6. **Parent (file mode):** write all data to temp files. No truncation. Track `original_bytes`. `truncated=false`.
7. Timer for `timeout_ms`: on timeout → SIGTERM to process group → wait 2s → SIGKILL → status `"timeout"`, exit_code `-2`.
8. On child exit: collect via `waitpid`. `WEXITSTATUS` if normal, `WTERMSIG+128` if signaled.

#### 4.6.1 File Output Mode Details

- Permissions: `0600`, owned by daemon's effective user.
- Naming: `hl_<request_id>_stdout` / `hl_<request_id>_stderr` in `output_tmpdir`.
- Daemon does NOT delete files — client's responsibility.
- Empty output streams: file is still created (0 bytes).
- Timeout: files contain partial output, may be mid-line truncated.
- For Docker-to-host: `output_tmpdir` should be bind-mounted into the container (`:ro`).

### 4.7 Security Considerations

- Constant-time token comparison.
- Privilege dropping after socket bind.
- Unix socket file permissions are the access control for local connections.
- TCP always binds to WireGuard interface IP, never `0.0.0.0`.
- Commands are shell commands by design — security boundary is auth token + network exposure.

---

## 5. Client (`hostlink-cli`)

### 5.1 Command-Line Interface

```
hostlink-cli [OPTIONS] <SUBCOMMAND>

SUBCOMMANDS:
  exec      Execute a command on a target host.
  ping      Check connectivity and discover node name.
  targets   List configured targets.

GLOBAL OPTIONS:
  -t, --target <name>         Target name from targets config.
  -s, --socket <path>         Connect via Unix socket (overrides target).
  -a, --address <host:port>   Connect via TCP (overrides target).
  -k, --token <token>         Auth token (overrides target). Also: HOSTLINK_TOKEN env var.
  -T, --timeout <ms>          Command timeout in ms (default: 30000).
  -C, --connect-timeout <ms>  Connection timeout in ms (default: 5000).
  --targets-file <path>       Path to targets config file.
  -j, --json                  Output raw JSON response.
  -h, --help                  Print help and exit.
  -V, --version               Print version and exit.

EXEC OPTIONS:
  -e, --env <KEY=VALUE>   Set env var (repeatable).
  -w, --workdir <path>    Set working directory.
  -m, --max-output <bytes> Max stdout/stderr capture. Accepts K/M/G suffixes.
  --max-stdout <bytes>    Override max for stdout only.
  --max-stderr <bytes>    Override max for stderr only.
  -F, --output-to-file    Request file output mode.
```

### 5.2 Targets Configuration

Search order (first wins):
1. `--targets-file` flag
2. `HOSTLINK_TARGETS` env var
3. `/etc/hostlink/targets.conf`
4. `~/.config/hostlink/targets.conf`

```ini
[desktop]
transport = unix
socket = /run/hostlink/hostlink.sock
token = a3f8...same_token...9c12

[dgx-spark]
transport = tcp
address = 10.0.0.2
port = 9876
token = a3f8...same_token...9c12

[dgx-spark-02]
transport = tcp
address = 10.0.0.3
port = 9876
token = b7e1...different_token...4d0a
```

#### 5.2.1 `targets` Subcommand

Default output (one line per target):
```
desktop    unix  /run/hostlink/hostlink.sock
dgx-spark  tcp   10.0.0.2:9876
```

With `--ping`:
```
desktop    unix  /run/hostlink/hostlink.sock  ok           desktop (up 23h)
dgx-spark  tcp   10.0.0.2:9876               ok           dgx-spark-01 (up 4d)
dgx-spark-02 tcp 10.0.0.3:9876              unreachable
```

### 5.3 Exit Codes

| Code | Meaning                                          |
|------|--------------------------------------------------|
| 0    | Success, remote exit code was 0.                 |
| 1    | Command ran but remote exit code was non-zero.   |
| 2    | Connection failed.                               |
| 3    | Authentication failed.                           |
| 4    | Request rejected (bad request / server busy).    |
| 5    | Command timed out on remote side.                |
| 6    | Protocol error.                                  |
| 7    | Client-side error (bad args, missing config).    |

### 5.4 Output Format

**Default:**
- First line to stderr: `[node_name] exit=<code> time=<duration>ms` plus truncation/file annotations.
- Stdout content to stdout, stderr content to stderr (inline mode only).

**JSON mode (`-j`):** Full response JSON to stdout.

---

## 6. Build System

### 6.1 Project Layout

```
hostlink/
├── build.sh
├── Makefile
├── hostlink.conf.example
├── targets.conf.example
├── hostlink.service
├── install.sh
├── README.md
├── src/
│   ├── common/
│   │   ├── protocol.c / .h
│   │   ├── config.c / .h
│   │   ├── log.c / .h
│   │   └── util.c / .h
│   ├── daemon/
│   │   ├── main.c
│   │   ├── server.c / .h
│   │   └── executor.c / .h
│   └── client/
│       ├── main.c
│       ├── connection.c / .h
└── tests/
    ├── run_tests.sh
    ├── test_protocol.c
    ├── test_config.c
    ├── test_integration.sh
    └── fixtures/
        ├── valid.conf
        ├── invalid.conf
        └── ...
```

### 6.2 build.sh

```bash
#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-release}"

case "$MODE" in
  release)
    CFLAGS="-std=c11 -O2 -flto -DNDEBUG -Wall -Wextra -Werror -pedantic"
    LDFLAGS="-flto -s"
    ;;
  debug)
    CFLAGS="-std=c11 -O0 -g3 -fsanitize=address,undefined -Wall -Wextra -Werror -pedantic"
    LDFLAGS="-fsanitize=address,undefined"
    ;;
  test)
    "$0" debug
    exec tests/run_tests.sh
    ;;
  clean)
    make clean
    exit 0
    ;;
  *)
    echo "Usage: $0 [release|debug|test|clean]" >&2
    exit 1
    ;;
esac

export CFLAGS LDFLAGS
make -j"$(nproc)" all
echo "Build complete: build/hostlinkd, build/hostlink-cli"
```

### 6.3 Compiler and Dependencies

- GCC or Clang, C11 (`-std=c11`)
- No external libraries — POSIX + Linux APIs (`epoll`) only
- JSON: minimal embedded parser (cJSON vendored in `src/common/`)
- Target platforms: Linux x86_64 and aarch64

---

## 7. systemd Integration

### 7.1 Unit File: `hostlink.service`

```ini
[Unit]
Description=HostLink Command Execution Agent
After=network-online.target
Wants=network-online.target

[Service]
Type=forking
ExecStart=/usr/local/bin/hostlinkd -c /etc/hostlink/hostlink.conf
ExecReload=/bin/kill -HUP $MAINPID
PIDFile=/run/hostlink/hostlink.pid
Restart=always
RestartSec=3
RuntimeDirectory=hostlink
RuntimeDirectoryMode=0750
ExecStartPre=/bin/mkdir -p /run/hostlink/output
ExecStartPre=/bin/chown hostlink:hostlink /run/hostlink/output
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=read-only
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
```

---

## 8. Docker Integration

```bash
docker run \
  -v /run/hostlink/hostlink.sock:/run/hostlink/hostlink.sock \
  -v /run/hostlink/output:/run/hostlink/output:ro \
  -v /path/to/targets.conf:/etc/hostlink/targets.conf:ro \
  --network host \
  openclaw-image
```

### 8.1 OpenClaw Agent Tool Definition

```
Tool: run_host_command
Parameters:
  - target (string, required)
  - command (string, required)
  - timeout_ms (int, optional, default 30000)
  - workdir (string, optional)
  - max_output (string, optional) — accepts K/M/G suffixes
  - output_to_file (bool, optional)
Usage: hostlink-cli -t <target> -j [--max-output <size>] [-F] exec "<command>"
```

Agent strategy notes:
- Exploratory commands: use defaults.
- Unknown output size: start with `--max-output 4K`, re-run with larger limit if truncated.
- `stdout_truncated=true`: check `stdout_original_bytes`, decide on larger limit or `--output-to-file`.
- File mode on local target: files readable at `/run/hostlink/output/` inside container.
- File mode on remote: use follow-up command to read portions (`head`, `tail`, `grep`).
- Always clean up: `rm /run/hostlink/output/hl_<id>_*`.

---

## 9. Test Specifications

### 9.1 Unit Tests (`test_protocol.c`, `test_config.c`)

- Frame encoding roundtrip, magic/length correctness, >128 MiB rejection, truncated frame detection, multiple frames, malformed JSON.
- Valid config parse, missing required keys, invalid values, comment/whitespace handling, empty file.

### 9.2 Integration Tests (`test_integration.sh`)

Full list of required test cases:
- `test_exec_simple` — echo hello, assert stdout/exit_code
- `test_exec_stderr` — echo to stderr
- `test_exec_failure` — false, assert exit_code=1, status="ok"
- `test_exec_nonexistent` — bad command, exit_code=127
- `test_exec_timeout` — sleep 60 with timeout_ms=500
- `test_exec_large_output` — >4 MiB with default limits, truncated=true
- `test_exec_custom_max_output` — seq with --max-output 1K, exactly 1024 bytes
- `test_exec_large_max_output` — 2 MiB with --max-output 8M, not truncated
- `test_exec_max_output_clamped` — daemon max_output_bytes=1MiB, request 64MiB
- `test_exec_output_to_file` — assert stdout_file, no stdout field
- `test_exec_output_to_file_large` — 10 MiB, file size matches original_bytes
- `test_exec_output_to_file_empty` — true (no output), file exists at 0 bytes
- `test_exec_output_to_file_cleanup` — delete files, assert success
- `test_exec_no_truncation_flag` — small output, truncated=false
- `test_exec_env` — env var passthrough
- `test_exec_workdir` — pwd, assert /tmp
- `test_exec_workdir_invalid` — nonexistent workdir, status=error
- `test_auth_failure` — wrong token
- `test_ping` — status=ok, node name matches
- `test_bad_magic` — connection closed
- `test_oversized_frame` — len > 128 MiB, connection closed
- `test_concurrent_exec` — max_concurrent+1, one gets "server busy"
- `test_tcp_transport` — loopback TCP
- `test_graceful_shutdown` — SIGTERM mid-command
- `test_sighup_reload` — change token, reload, assert old rejected / new accepted
- `test_json_output` — all required fields present
- `test_cli_exit_codes` — all 7 exit code scenarios
- `test_connect_timeout` — daemon down, exit code 2 within ~500ms
- `test_connect_timeout_default` — daemon down, exit code 2 within ~5s
- `test_targets_subcommand` — 3 targets, all listed
- `test_targets_subcommand_json` — valid JSON array
- `test_targets_file_override` — custom --targets-file
- `test_output_to_file_timeout` — timeout with file mode, files valid
- `test_targets_ping` — --ping shows ok + node name
- `test_sighup_reload_output_limits` — change default_max_output_bytes, reload, verify applied

### 9.3 Stress Tests (optional)

- 1000 sequential commands, no fd leaks
- 100 concurrent --output-to-file, no cross-contamination
- Kill mid-command, verify systemd restart within 5s

---

## 10. Install Helper (`install.sh`)

1. Copy `hostlinkd` and `hostlink-cli` to `/usr/local/bin/`
2. Create `/etc/hostlink/`, copy example config if none exists
3. Generate `auth_token` via `openssl rand -hex 32`
4. Create `hostlink` system user and group (if not existing)
5. Install systemd unit file
6. `systemctl daemon-reload && systemctl enable --now hostlink`
7. Print summary: token, socket path, Docker mount instructions

---

## 11. WireGuard Setup Notes (README only)

Desktop (`10.0.0.1`) and DGX Spark (`10.0.0.2`) WireGuard peer configs with `PersistentKeepalive = 25`. Future nodes get next IPs (10.0.0.3, etc.).

---

## 12. Future Extensions (Out of Scope for v1)

- Streaming output
- File transfer
- mTLS on TCP
- Command allowlisting
- Multiplexed connections
- stdin forwarding
