# HostLink `get` Subcommand — Upgrade Plan

## Problem

HostLink has `put` (push file from client → remote daemon) but no `get` (pull file from remote daemon → client). This creates friction every time we need to retrieve files from a remote machine:

**Current workarounds (all bad):**
- `hl-spark "cat /path/to/file"` — works for small text, breaks on binary, stdout truncation limits
- HTTP server on remote + wget from host — works but requires 3 commands and manual cleanup
- `hl-spark "base64 /path"` | decode — slow, doubles transfer size, breaks on large files
- Can't retrieve eval results, training logs, model files, or any binary output without hacks

**Real cost this session:** Transferring a 17GB merged model from Spark to host required spinning up a Python HTTP server on Spark and wget-ing from the host. The actual `put` over TCP took 2.5 minutes at 130MB/s — the protocol is fast, we just can't use it in reverse.

## Proposed Design

### CLI Interface
```
hostlink get <remote_path> <local_path> [options]
```

Mirror of `put` with reversed direction:
```bash
# Pull a file from Spark to container
hostlink -t spark get /root/model.safetensors /tmp/model.safetensors

# Via wrapper
hl-spark get /root/model.safetensors /tmp/model.safetensors
hl get /etc/some-config.json /tmp/config.json
```

### Protocol Extension

The HostLink daemon protocol likely uses a simple request/response over the socket/TCP connection. `get` needs:

1. **Client sends:** `GET <remote_path>` request (same auth as exec/put)
2. **Daemon reads:** the file from local filesystem
3. **Daemon sends:** file size header + raw bytes (same framing as `put` but reversed)
4. **Client writes:** bytes to `<local_path>`

**Error cases:**
- File not found → error response (no data)
- Permission denied → error response
- Path outside allowed scope → error response (if sandboxing applies)

### Security Considerations

- `get` on the full-access daemon (`hostlinkd`) should work for any readable file
- `get` on the read-only daemon (`hostlinkd-ro`) should also work (it's read-only by nature)
- Consider: should `get` have a max file size? Or let the client timeout handle it?
- Auth: same per-target token model as `put` and `exec`

### Implementation Scope

**Daemon side (C, server.c):**
- New handler for `GET` command type
- Read file, send size + content over the connection
- Error handling for missing/unreadable files

**Client side (Rust, hostlink-cli):**
- New `get` subcommand in CLI parser
- Send GET request, receive file data, write to local path
- Progress indicator for large files (optional)

**Wrappers (bash):**
- `hl-spark` already updated to route subcommands — `get` will work automatically
- `hl-put` could gain a sibling `hl-get`, or just use `hl-spark get` / `hl get` directly

### Wrapper Updates

`hl-spark` (already updated this session) detects `put`/`ping` as subcommands. Adding `get`:

```bash
case "$1" in
  put|get|ping)
    subcmd="$1"; shift
    exec env -u HOSTLINK_TOKEN "$HOSTLINK" \
      --targets-file "$TARGETS" -t spark "${FLAGS[@]}" "$subcmd" "$@"
    ;;
  *)
    exec env -u HOSTLINK_TOKEN "$HOSTLINK" \
      --targets-file "$TARGETS" -t spark "${FLAGS[@]}" exec "$*"
    ;;
esac
```

### Priority

**High.** This blocks smooth operation of:
- Forge eval pipeline (pulling results from Spark)
- Model promotion pipeline (pulling merged checkpoints)
- Any future training/inference workflow that produces output files on remote machines

### Testing

1. Small text file: `hl-spark get /tmp/test.txt /tmp/received.txt` → verify content matches
2. Binary file: `hl-spark get /some/binary /tmp/binary` → verify SHA256 matches
3. Large file: `hl-spark get /root/model.safetensors /tmp/model.safetensors` → verify size and throughput
4. Error cases: nonexistent file, permission denied, empty file
5. Both transports: Unix socket (host) and TCP (Spark)
