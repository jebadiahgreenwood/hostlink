# HostLink

A lightweight daemon written in C that allows authorized clients to execute shell commands on a host machine and receive structured output (stdout, stderr, exit code).

## Features

- **Two transports:** Unix domain socket (local Docker-to-host) and TCP over WireGuard (remote hosts)
- **Simple wire protocol:** Length-prefixed frames with JSON payloads
- **Pre-shared token auth** with constant-time comparison
- **Output capture:** Inline (with configurable size limits) or file-based for large outputs
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
# Execute a command
hostlink-cli -t desktop exec "uname -a"

# JSON output (for scripting/agents)
hostlink-cli -t dgx-spark -j exec "nvidia-smi"

# Ping
hostlink-cli -t desktop ping

# List targets
hostlink-cli targets
```

## Client Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success (remote exit code 0) |
| 1 | Remote command exited non-zero |
| 2 | Connection failed |
| 3 | Authentication failed |
| 4 | Bad request / server busy |
| 5 | Command timed out |
| 6 | Protocol error |
| 7 | Client-side error |

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
