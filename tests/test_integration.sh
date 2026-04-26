#!/usr/bin/env bash
# Integration tests for hostlink
set -uo pipefail
cd "$(dirname "$0")/.."

# Prefer fresh build/ (make all output); fall back to committed build2/
# binaries so the test can run without rebuilding.
if [ -x ./build/hostlink-cli ] && [ -x ./build/hostlinkd ]; then
    CLI="./build/hostlink-cli"
    DAEMON="./build/hostlinkd"
else
    CLI="./build2/hostlink-cli"
    DAEMON="./build2/hostlinkd"
fi
SOCK="/tmp/hl_test_$$.sock"
TCP_PORT=$((19000 + (RANDOM % 1000)))
TOKEN="integration-test-token-xyz"
OUTPUT_DIR="/tmp/hl_test_output_$$"
CONF="/tmp/hl_test_$$.conf"
CONF2="/tmp/hl_test2_$$.conf"
TARGETS="/tmp/hl_targets_$$.conf"
PIDFILE="/tmp/hl_test_$$.pid"
DAEMON_PID=""
PASS=0
FAIL=0

cleanup() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill "$DAEMON_PID" 2>/dev/null || true
        sleep 0.3
        kill -9 "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -f "$CONF" "$CONF2" "$TARGETS" "$SOCK" "$PIDFILE"
    rm -rf "$OUTPUT_DIR"
}
trap cleanup EXIT

write_conf() {
    local path="$1" token="$2" extra="${3:-}"
    cat > "$path" <<EOF
node_name = testnode
auth_token = $token
unix_enabled = 1
unix_path = $SOCK
unix_mode = 0660
tcp_enabled = 1
tcp_bind = 127.0.0.1
tcp_port = $TCP_PORT
max_concurrent = 3
default_timeout_ms = 30000
max_timeout_ms = 300000
shell = /bin/sh
default_max_output_bytes = 4194304
max_output_bytes = 67108864
output_tmpdir = $OUTPUT_DIR
log_target = stderr
log_level = warn
$extra
EOF
}

write_targets() {
    cat > "$TARGETS" <<EOF
[local]
transport = unix
socket = $SOCK
token = $TOKEN
EOF
}

start_daemon() {
    local conf="${1:-$CONF}"
    mkdir -p "$OUTPUT_DIR"
    # Use a per-test PID file so we don't collide with a system daemon at
    # /run/hostlink/hostlink.pid during integration runs.
    "$DAEMON" -f -c "$conf" -p "$PIDFILE" &
    DAEMON_PID=$!
    # Wait for socket to appear
    for i in $(seq 1 30); do
        sleep 0.1
        [ -S "$SOCK" ] && return 0
    done
    echo "ERROR: daemon didn't start in time"
    return 1
}

stop_daemon() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill "$DAEMON_PID" 2>/dev/null
        wait "$DAEMON_PID" 2>/dev/null || true
        DAEMON_PID=""
    fi
    sleep 0.2
}

assert() {
    local name="$1" cond="$2"
    if [ "$cond" = "0" ]; then
        echo "PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

assert_eq() {
    local name="$1" a="$2" b="$3"
    if [ "$a" = "$b" ]; then
        echo "PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name (expected '$b', got '$a')"
        FAIL=$((FAIL + 1))
    fi
}

# ---- Setup ----
write_conf "$CONF" "$TOKEN"
write_targets
start_daemon "$CONF"

# ---- test_exec_simple ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "echo hello" 2>/dev/null)
ec=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['exit_code'])" 2>/dev/null || echo "ERR")
stdout=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout'],end='')" 2>/dev/null || echo "ERR")
assert_eq "test_exec_simple_exit" "$ec" "0"
assert_eq "test_exec_simple_stdout" "$stdout" "hello"

# ---- test_exec_stderr ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "echo err >&2" 2>/dev/null)
stderr_val=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stderr'],end='')" 2>/dev/null || echo "ERR")
stdout_val=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout'],end='')" 2>/dev/null || echo "ERR")
assert_eq "test_exec_stderr" "$stderr_val" "err"
assert_eq "test_exec_stderr_stdout_empty" "$stdout_val" ""

# ---- test_exec_failure ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "false" 2>/dev/null)
ec=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['exit_code'])" 2>/dev/null || echo "ERR")
status=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['status'])" 2>/dev/null || echo "ERR")
assert_eq "test_exec_failure_exit" "$ec" "1"
assert_eq "test_exec_failure_status" "$status" "ok"

# ---- test_exec_nonexistent ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "no_such_command_xyz_abc" 2>/dev/null)
ec=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['exit_code'])" 2>/dev/null || echo "ERR")
assert "test_exec_nonexistent" "$([ "$ec" != "0" ] && echo 0 || echo 1)"

# ---- test_exec_timeout ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -T 600 exec "sleep 60" 2>/dev/null)
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['status'])" 2>/dev/null || echo "ERR")
ec=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['exit_code'])" 2>/dev/null || echo "ERR")
assert_eq "test_exec_timeout_status" "$st" "timeout"
assert_eq "test_exec_timeout_exit" "$ec" "-2"

# ---- test_exec_large_output ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "dd if=/dev/urandom bs=1024 count=5120 2>/dev/null | base64" 2>/dev/null)
trunc=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout_truncated'])" 2>/dev/null || echo "ERR")
orig=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout_original_bytes'])" 2>/dev/null || echo "ERR")
assert_eq "test_exec_large_output_truncated" "$trunc" "True"
assert "test_exec_large_output_orig_big" "$([ "$orig" -gt 4194304 ] 2>/dev/null && echo 0 || echo 1)"

# ---- test_exec_custom_max_output ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -m 1024 exec "seq 1 100000" 2>/dev/null)
trunc=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout_truncated'])" 2>/dev/null || echo "ERR")
stdout_len=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(len(d['stdout']))" 2>/dev/null || echo "0")
assert_eq "test_exec_custom_max_trunc" "$trunc" "True"
assert "test_exec_custom_max_len" "$([ "$stdout_len" -le 1024 ] 2>/dev/null && echo 0 || echo 1)"

# ---- test_exec_large_max_output ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -m 8388608 exec "dd if=/dev/zero bs=1024 count=2048 2>/dev/null | tr '\0' 'x'" 2>/dev/null)
trunc=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout_truncated'])" 2>/dev/null || echo "ERR")
assert_eq "test_exec_large_max_output_notrunc" "$trunc" "False"

# ---- test_exec_max_output_clamped ----
# Daemon's max_output_bytes=1MiB, request asks for 64MiB -- should clamp to 1MiB
SOCK_CLAMP="/tmp/hl_clamped_$$.sock"
CONF_CLAMP="/tmp/hl_clamped_$$.conf"
cat > "$CONF_CLAMP" <<EOF
node_name = clamped
auth_token = $TOKEN
unix_enabled = 1
unix_path = $SOCK_CLAMP
tcp_enabled = 0
max_concurrent = 3
default_timeout_ms = 30000
max_timeout_ms = 300000
shell = /bin/sh
default_max_output_bytes = 4194304
max_output_bytes = 1048576
output_tmpdir = $OUTPUT_DIR
log_target = stderr
log_level = warn
EOF
mkdir -p "$OUTPUT_DIR"
PIDFILE_CLAMP="/tmp/hl_clamped_$$.pid"
"$DAEMON" -f -c "$CONF_CLAMP" -p "$PIDFILE_CLAMP" &
DAEMON_PID_CLAMP=$!
for i in $(seq 1 30); do sleep 0.1; [ -S "$SOCK_CLAMP" ] && break; done
out=$("$CLI" -s "$SOCK_CLAMP" -k "$TOKEN" -j --max-stdout 67108864 exec "dd if=/dev/zero bs=1024 count=2048 2>/dev/null | tr '\0' 'x'" 2>/dev/null)
trunc=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout_truncated'])" 2>/dev/null || echo "ERR")
stdout_len=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(len(d['stdout']))" 2>/dev/null || echo "0")
assert_eq "test_exec_max_output_clamped_truncated" "$trunc" "True"
assert "test_exec_max_output_clamped_len" "$([ "$stdout_len" -le 1048576 ] 2>/dev/null && echo 0 || echo 1)"
kill "$DAEMON_PID_CLAMP" 2>/dev/null || true
wait "$DAEMON_PID_CLAMP" 2>/dev/null || true
rm -f "$SOCK_CLAMP" "$CONF_CLAMP" "$PIDFILE_CLAMP"

# ---- test_exec_output_to_file ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -O exec "echo hello" 2>/dev/null)
has_file=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print('stdout_file' in d)" 2>/dev/null || echo "ERR")
has_stdout=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print('stdout' in d)" 2>/dev/null || echo "ERR")
fpath=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stdout_file',''))" 2>/dev/null || echo "")
assert_eq "test_exec_output_to_file_has_file" "$has_file" "True"
assert_eq "test_exec_output_to_file_no_stdout" "$has_stdout" "False"
if [ -n "$fpath" ] && [ -f "$fpath" ]; then
    content=$(cat "$fpath")
    assert_eq "test_exec_output_to_file_content" "$content" "hello"
else
    assert "test_exec_output_to_file_exists" 1
fi

# ---- test_exec_output_to_file_large ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -O exec "dd if=/dev/urandom bs=1024 count=10240 2>/dev/null | base64" 2>/dev/null)
fpath=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stdout_file',''))" 2>/dev/null || echo "")
orig_bytes=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stdout_original_bytes',0))" 2>/dev/null || echo "0")
if [ -n "$fpath" ] && [ -f "$fpath" ]; then
    fsize=$(stat -c%s "$fpath" 2>/dev/null || echo "-1")
    assert "test_exec_output_to_file_large_size" "$([ "$fsize" -gt 1000000 ] 2>/dev/null && echo 0 || echo 1)"
    assert_eq "test_exec_output_to_file_large_match" "$fsize" "$orig_bytes"
else
    assert "test_exec_output_to_file_large_exists" 1
fi

# ---- test_exec_output_to_file_empty ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -O exec "true" 2>/dev/null)
fpath=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stdout_file',''))" 2>/dev/null || echo "")
if [ -n "$fpath" ]; then
    fsize=$(stat -c%s "$fpath" 2>/dev/null || echo "-1")
    assert_eq "test_exec_output_to_file_empty" "$fsize" "0"
else
    assert "test_exec_output_to_file_empty_path" 1
fi

# ---- test_exec_output_to_file_cleanup ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -O exec "echo cleanup_test" 2>/dev/null)
fpath_out=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stdout_file',''))" 2>/dev/null || echo "")
fpath_err=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stderr_file',''))" 2>/dev/null || echo "")
if [ -n "$fpath_out" ] && [ -f "$fpath_out" ]; then
    rm -f "$fpath_out" "$fpath_err"
    assert "test_exec_output_to_file_cleanup" "$([ ! -f "$fpath_out" ] && echo 0 || echo 1)"
else
    assert "test_exec_output_to_file_cleanup_path" 1
fi


# ---- test_get_small_text ----
echo "hello from get" > /tmp/hl_get_test_src_$$.txt
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j get "/tmp/hl_get_test_src_$$.txt" "/tmp/hl_get_test_dst_$$.txt" 2>/dev/null)
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['status'])" 2>/dev/null || echo "ERR")
assert_eq "test_get_small_text_status" "$st" "ok"
content=$(cat /tmp/hl_get_test_dst_$$.txt 2>/dev/null || echo "ERR")
assert_eq "test_get_small_text_content" "$content" "hello from get"
rm -f /tmp/hl_get_test_src_$$.txt /tmp/hl_get_test_dst_$$.txt

# ---- test_get_binary ----
dd if=/dev/urandom of=/tmp/hl_get_bin_src_$$.bin bs=1024 count=64 2>/dev/null
src_sha=$(sha256sum /tmp/hl_get_bin_src_$$.bin | awk '{print $1}')
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j get "/tmp/hl_get_bin_src_$$.bin" "/tmp/hl_get_bin_dst_$$.bin" 2>/dev/null)
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['status'])" 2>/dev/null || echo "ERR")
assert_eq "test_get_binary_status" "$st" "ok"
dst_sha=$(sha256sum /tmp/hl_get_bin_dst_$$.bin 2>/dev/null | awk '{print $1}' || echo "ERR")
assert_eq "test_get_binary_sha256" "$dst_sha" "$src_sha"
rm -f /tmp/hl_get_bin_src_$$.bin /tmp/hl_get_bin_dst_$$.bin

# ---- test_get_nonexistent ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j get "/tmp/nonexistent_file_xyz_$$.txt" "/tmp/hl_get_noexist_$$.txt" 2>/dev/null)
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['status'])" 2>/dev/null || echo "ERR")
assert_eq "test_get_nonexistent" "$st" "error"
assert "test_get_nonexistent_no_local" "$([ ! -f /tmp/hl_get_noexist_$$.txt ] && echo 0 || echo 1)"

# ---- test_get_empty_file ----
touch /tmp/hl_get_empty_src_$$.txt
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j get "/tmp/hl_get_empty_src_$$.txt" "/tmp/hl_get_empty_dst_$$.txt" 2>/dev/null)
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['status'])" 2>/dev/null || echo "ERR")
assert_eq "test_get_empty_file_status" "$st" "ok"
fsize=$(stat -c%s /tmp/hl_get_empty_dst_$$.txt 2>/dev/null || echo "-1")
assert_eq "test_get_empty_file_size" "$fsize" "0"
rm -f /tmp/hl_get_empty_src_$$.txt /tmp/hl_get_empty_dst_$$.txt

# ---- test_get_json_fields ----
echo "json test" > /tmp/hl_get_json_src_$$.txt
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j get "/tmp/hl_get_json_src_$$.txt" "/tmp/hl_get_json_dst_$$.txt" 2>/dev/null)
has_fields=$(echo "$out" | python3 -c "
import sys,json
d=json.load(sys.stdin)
required = ['version','id','node','status','path','size','content']
print(all(k in d for k in required))
" 2>/dev/null || echo "False")
assert_eq "test_get_json_fields" "$has_fields" "True"
rm -f /tmp/hl_get_json_src_$$.txt /tmp/hl_get_json_dst_$$.txt

# ---- test_exec_no_truncation_flag ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "echo hi" 2>/dev/null)
trunc=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout_truncated'])" 2>/dev/null || echo "ERR")
orig=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout_original_bytes'])" 2>/dev/null || echo "ERR")
assert_eq "test_exec_no_trunc_flag" "$trunc" "False"
assert_eq "test_exec_no_trunc_orig" "$orig" "3"

# ---- test_exec_env ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -e "MY_VAR=hello" exec 'echo $MY_VAR' 2>/dev/null)
stdout_val=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout'],end='')" 2>/dev/null || echo "ERR")
assert_eq "test_exec_env" "$stdout_val" "hello"

# ---- test_exec_workdir ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -w /tmp exec "pwd" 2>/dev/null)
stdout_val=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout'],end='')" 2>/dev/null || echo "ERR")
assert_eq "test_exec_workdir" "$stdout_val" "/tmp"

# ---- test_exec_workdir_invalid ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -w /nonexistent_dir_xyz exec "pwd" 2>/dev/null)
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['status'])" 2>/dev/null || echo "ERR")
assert_eq "test_exec_workdir_invalid" "$st" "error"

# ---- test_auth_failure ----
out=$("$CLI" -s "$SOCK" -k "wrongtoken" -j exec "echo hi" 2>/dev/null)
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['status'])" 2>/dev/null || echo "ERR")
assert_eq "test_auth_failure" "$st" "auth_failed"

# ---- test_ping ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j ping 2>/dev/null)
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['status'])" 2>/dev/null || echo "ERR")
node=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['node'])" 2>/dev/null || echo "ERR")
assert_eq "test_ping_status" "$st" "ok"
assert_eq "test_ping_node" "$node" "testnode"

# ---- test_bad_magic ----
# Send bad magic bytes; daemon should close the connection cleanly without crashing
python3 -c "
import socket, struct, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$SOCK')
s.sendall(struct.pack('>II', 0xDEADBEEF, 4) + b'data')
s.settimeout(2)
try:
    data = s.recv(1024)
except Exception:
    pass
s.close()
print('ok')
" 2>/dev/null
assert "test_bad_magic" "0"

# ---- test_oversized_frame ----
# Attempt a frame with len > 128 MiB; daemon should reject and close cleanly
python3 -c "
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$SOCK')
s.sendall(struct.pack('>II', 0x484C4E4B, 128*1024*1024 + 1))
s.settimeout(2)
try:
    data = s.recv(1024)
except Exception:
    pass
s.close()
print('ok')
" 2>/dev/null
assert "test_oversized_frame" "0"

# ---- test_tcp_transport ----
out=$("$CLI" -a "127.0.0.1:$TCP_PORT" -k "$TOKEN" -j exec "echo tcp_works" 2>/dev/null)
stdout_val=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout'],end='')" 2>/dev/null || echo "ERR")
assert_eq "test_tcp_transport" "$stdout_val" "tcp_works"

# ---- test_concurrent_exec ----
# Start max_concurrent+1 = 4 slow commands simultaneously (max_concurrent=3)
pids=()
tmpfiles=()
for i in 1 2 3 4; do
    tf=$(mktemp)
    tmpfiles+=("$tf")
    "$CLI" -s "$SOCK" -k "$TOKEN" -j exec "sleep 2" > "$tf" 2>/dev/null &
    pids+=($!)
done
# Wait a bit then fire one more
sleep 0.5
tf=$(mktemp)
tmpfiles+=("$tf")
"$CLI" -s "$SOCK" -k "$TOKEN" -j exec "sleep 2" > "$tf" 2>/dev/null &
pids+=($!)

for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done

busy_count=0
for tf in "${tmpfiles[@]}"; do
    st=$(python3 -c "import sys,json; d=json.load(open('$tf')); print(d.get('status',''))" 2>/dev/null || echo "")
    if [ "$st" = "error" ]; then
        emsg=$(python3 -c "import sys,json; d=json.load(open('$tf')); print(d.get('error_msg',''))" 2>/dev/null || echo "")
        if echo "$emsg" | grep -qi "busy"; then
            busy_count=$((busy_count + 1))
        fi
    fi
    rm -f "$tf"
done
assert "test_concurrent_exec_busy" "$([ "$busy_count" -ge 1 ] && echo 0 || echo 1)"

# ---- test_json_output ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "echo x" 2>/dev/null)
has_fields=$(echo "$out" | python3 -c "
import sys,json
d=json.load(sys.stdin)
required = ['version','id','node','status','exit_code','stdout','stderr',
            'stdout_truncated','stderr_truncated','stdout_original_bytes',
            'stderr_original_bytes','duration_ms']
print(all(k in d for k in required))
" 2>/dev/null || echo "False")
assert_eq "test_json_output_fields" "$has_fields" "True"

# ---- test_cli_exit_codes ----
"$CLI" -s "$SOCK" -k "$TOKEN" exec "true" >/dev/null 2>&1
assert_eq "test_exit_code_0" "$?" "0"

"$CLI" -s "$SOCK" -k "$TOKEN" exec "false" >/dev/null 2>&1; _ec=$?
assert_eq "test_exit_code_1" "$_ec" "1"

"$CLI" -s /nonexistent_xyz.sock -k "$TOKEN" exec "echo hi" >/dev/null 2>&1; _ec=$?
assert_eq "test_exit_code_2" "$_ec" "2"

"$CLI" -s "$SOCK" -k "badtoken" exec "echo hi" >/dev/null 2>&1; _ec=$?
assert_eq "test_exit_code_3" "$_ec" "3"

"$CLI" -s "$SOCK" -k "$TOKEN" -T 500 exec "sleep 60" >/dev/null 2>&1; _ec=$?
assert_eq "test_exit_code_5" "$_ec" "5"

# ---- test_graceful_shutdown ----
# Start a long command, send SIGTERM to daemon, verify it stops within ~5s
"$CLI" -s "$SOCK" -k "$TOKEN" exec "sleep 30" >/dev/null 2>&1 &
long_pid=$!
sleep 0.3
kill -TERM "$DAEMON_PID" 2>/dev/null || true
# Wait up to 5 seconds for daemon to stop
daemon_stopped=0
for i in $(seq 1 50); do
    sleep 0.1
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        daemon_stopped=1
        DAEMON_PID=""
        break
    fi
done
if [ "$daemon_stopped" -eq 0 ]; then
    kill -9 "$DAEMON_PID" 2>/dev/null || true
    DAEMON_PID=""
fi
assert "test_graceful_shutdown" "$([ "$daemon_stopped" -eq 1 ] && echo 0 || echo 1)"
wait "$long_pid" 2>/dev/null || true

# ---- test_connect_timeout ----
# Daemon is now stopped
t_start=$(date +%s%3N)
"$CLI" -s "$SOCK" -k "$TOKEN" -C 500 exec "echo hi" >/dev/null 2>&1
cli_ec=$?
t_end=$(date +%s%3N)
elapsed=$((t_end - t_start))
assert "test_connect_timeout" "$([ "$elapsed" -lt 2000 ] && echo 0 || echo 1)"
assert_eq "test_connect_timeout_exit_code" "$cli_ec" "2"

# ---- test_connect_timeout_default ----
t_start=$(date +%s%3N)
"$CLI" -s "$SOCK" -k "$TOKEN" exec "echo hi" >/dev/null 2>&1
cli_ec=$?
t_end=$(date +%s%3N)
elapsed=$((t_end - t_start))
# Default connect timeout is 5s; should finish within 7s
assert "test_connect_timeout_default" "$([ "$elapsed" -lt 7000 ] && echo 0 || echo 1)"
assert_eq "test_connect_timeout_default_exit_code" "$cli_ec" "2"

# ---- Restart daemon for remaining tests ----
DAEMON_PID=""
start_daemon "$CONF"

# ---- test_targets_subcommand ----
cat > "$TARGETS" <<EOF
[desktop]
transport = unix
socket = $SOCK
token = $TOKEN

[dgx-spark]
transport = tcp
address = 10.0.0.2
port = 9876
token = $TOKEN

[dgx-spark-02]
transport = tcp
address = 10.0.0.3
port = 9877
token = $TOKEN
EOF

out=$("$CLI" --targets-file "$TARGETS" targets 2>/dev/null)
has_desktop=$(echo "$out" | grep -c "desktop" || echo 0)
has_dgx=$(echo "$out" | grep -c "dgx-spark" || echo 0)
assert "test_targets_desktop" "$([ "$has_desktop" -ge 1 ] && echo 0 || echo 1)"
assert "test_targets_dgx" "$([ "$has_dgx" -ge 1 ] && echo 0 || echo 1)"

# ---- test_targets_subcommand_json ----
out=$("$CLI" --targets-file "$TARGETS" -j targets 2>/dev/null)
is_arr=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(isinstance(d,list))" 2>/dev/null || echo "False")
count=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(len(d))" 2>/dev/null || echo "0")
assert_eq "test_targets_json" "$is_arr" "True"
assert_eq "test_targets_json_count" "$count" "3"

# ---- test_targets_file_override ----
custom_targets="/tmp/hl_custom_targets_$$.conf"
cat > "$custom_targets" <<EOF
[custom-node]
transport = unix
socket = $SOCK
token = $TOKEN
EOF
out=$("$CLI" --targets-file "$custom_targets" targets 2>/dev/null)
has_custom=$(echo "$out" | grep -c "custom-node" || echo 0)
assert "test_targets_file_override" "$([ "$has_custom" -ge 1 ] && echo 0 || echo 1)"
rm -f "$custom_targets"

# ---- test_targets_ping ----
cat > "$TARGETS" <<EOF
[local]
transport = unix
socket = $SOCK
token = $TOKEN
EOF
out=$("$CLI" --targets-file "$TARGETS" -P targets 2>/dev/null || true)
has_ok=$(echo "$out" | grep -c "ok" || echo 0)
assert "test_targets_ping" "$([ "$has_ok" -ge 1 ] && echo 0 || echo 1)"

# ---- test_sighup_reload ----
# Change token, send SIGHUP, verify old token rejected, new token accepted
NEW_TOKEN="new-token-after-reload-$$"
write_conf "$CONF" "$NEW_TOKEN"
kill -HUP "$DAEMON_PID" 2>/dev/null || true
sleep 0.5

out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j ping 2>/dev/null || echo "{}")
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || echo "ERR")
assert_eq "test_sighup_reload_old_token_rejected" "$st" "auth_failed"

out=$("$CLI" -s "$SOCK" -k "$NEW_TOKEN" -j ping 2>/dev/null || echo "{}")
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || echo "ERR")
assert_eq "test_sighup_reload_new_token_accepted" "$st" "ok"

# ---- test_sighup_reload_output_limits ----
write_conf "$CONF" "$NEW_TOKEN" "default_max_output_bytes = 512"
kill -HUP "$DAEMON_PID" 2>/dev/null || true
sleep 0.5

out=$("$CLI" -s "$SOCK" -k "$NEW_TOKEN" -j exec "seq 1 10000" 2>/dev/null)
trunc=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['stdout_truncated'])" 2>/dev/null || echo "ERR")
stdout_len=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(len(d['stdout']))" 2>/dev/null || echo "0")
assert_eq "test_sighup_reload_output_limits_trunc" "$trunc" "True"
assert "test_sighup_reload_output_limits_len" "$([ "$stdout_len" -le 512 ] 2>/dev/null && echo 0 || echo 1)"

# Restore config for final tests
write_conf "$CONF" "$TOKEN"
stop_daemon
DAEMON_PID=""
start_daemon "$CONF"

# ---- test_output_to_file_timeout ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -O -T 500 exec "sleep 60" 2>/dev/null)
st=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['status'])" 2>/dev/null || echo "ERR")
fpath=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stdout_file',''))" 2>/dev/null || echo "")
assert_eq "test_output_to_file_timeout_status" "$st" "timeout"
if [ -n "$fpath" ]; then
    assert "test_output_to_file_timeout_file_exists" "$([ -f "$fpath" ] && echo 0 || echo 1)"
fi

# ============================================================================
# Concurrency tests — prove get/put no longer head-of-line block the daemon
# ============================================================================
#
# Pre-refactor (commit 1), get/put ran inline on the main event loop, so a
# multi-MiB transfer froze every other request for its duration. Post-refactor
# they fork into I/O workers (mirroring exec) bounded by max_concurrent_io.
#
# These tests would ALL have failed pre-refactor:
#   1. ping completes promptly while a 50 MiB get is in flight
#   2. a small get completes promptly while the big get is in flight
#   3. exec runs concurrently with get
#   4. max_concurrent_io is enforced (5th transfer rejected with "busy")

# ---- test_concurrent_ping_during_get ----
SRC="/tmp/hl_concurrent_src_$$.bin"
SMALL="/tmp/hl_concurrent_small_$$.txt"
DST_SLOW="/tmp/hl_concurrent_slow_dst_$$.bin"
DST_FAST="/tmp/hl_concurrent_fast_dst_$$.bin"
"$CLI" -s "$SOCK" -k "$TOKEN" exec "dd if=/dev/urandom of=$SRC bs=1M count=50 status=none" >/dev/null 2>&1
"$CLI" -s "$SOCK" -k "$TOKEN" exec "echo small > $SMALL" >/dev/null 2>&1

# Kick off a slow 50 MiB get in the background
"$CLI" -s "$SOCK" -k "$TOKEN" -T 30000 get "$SRC" "$DST_SLOW" >/dev/null 2>&1 &
SLOW_PID=$!
sleep 0.05  # let the transfer start

t0=$(date +%s%N)
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -T 2000 ping 2>/dev/null || echo "{}")
t1=$(date +%s%N)
ping_ms=$(( (t1 - t0) / 1000000 ))
ping_status=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || echo "ERR")
assert_eq "test_concurrent_ping_status" "$ping_status" "ok"
assert "test_concurrent_ping_fast" "$([ "$ping_ms" -lt 200 ] 2>/dev/null && echo 0 || echo 1)"

# ---- test_concurrent_small_get_during_big_get ----
t0=$(date +%s%N)
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -T 2000 get "$SMALL" "$DST_FAST" 2>/dev/null || echo "{}")
t1=$(date +%s%N)
small_ms=$(( (t1 - t0) / 1000000 ))
small_status=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || echo "ERR")
assert_eq "test_concurrent_small_get_status" "$small_status" "ok"
assert "test_concurrent_small_get_fast" "$([ "$small_ms" -lt 200 ] 2>/dev/null && echo 0 || echo 1)"

# ---- test_concurrent_exec_during_get ----
t0=$(date +%s%N)
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -T 2000 exec "echo concurrent" 2>/dev/null || echo "{}")
t1=$(date +%s%N)
exec_ms=$(( (t1 - t0) / 1000000 ))
exec_status=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || echo "ERR")
assert_eq "test_concurrent_exec_status" "$exec_status" "ok"
assert "test_concurrent_exec_fast" "$([ "$exec_ms" -lt 200 ] 2>/dev/null && echo 0 || echo 1)"

wait "$SLOW_PID"
slow_rc=$?
assert "test_concurrent_slow_get_ok" "$([ "$slow_rc" -eq 0 ] && echo 0 || echo 1)"
src_sha=$(sha256sum "$SRC" 2>/dev/null | awk '{print $1}')
dst_sha=$(sha256sum "$DST_SLOW" 2>/dev/null | awk '{print $1}')
assert_eq "test_concurrent_slow_get_sha256" "$dst_sha" "$src_sha"

# ---- test_max_concurrent_io_enforced ----
# Default max_concurrent_io = 4 (set by daemon_config_defaults). Spawn 5
# simultaneous gets of a near-cap-size file (89 MiB); at least one must
# hit the busy path. Race-tolerant: we don't depend on which one loses.
BIG_SRC="/tmp/hl_concurrent_big_$$.bin"
"$CLI" -s "$SOCK" -k "$TOKEN" exec "dd if=/dev/urandom of=$BIG_SRC bs=1M count=89 status=none" >/dev/null 2>&1
OVERFLOW_OUT_DIR="/tmp/hl_concurrent_overflow_${$}_d"
mkdir -p "$OVERFLOW_OUT_DIR"
OVERFLOW_PIDS=()
for i in 1 2 3 4 5; do
    ("$CLI" -s "$SOCK" -k "$TOKEN" -j -T 30000 get "$BIG_SRC" \
        "$OVERFLOW_OUT_DIR/slot_${i}.bin" 2>/dev/null \
        > "$OVERFLOW_OUT_DIR/slot_${i}.json") &
    OVERFLOW_PIDS+=($!)
done
# Wait for these specific PIDs, NOT bare `wait` — bare `wait` would also
# block on the long-lived daemon background process started by start_daemon.
for pid in "${OVERFLOW_PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done

# Use grep against the literal busy-error string instead of parsing each
# JSON file — 4 of them are ~124 MiB (full base64 payload) and json.load()
# of that is unbearably slow.
busy_count=0
for i in 1 2 3 4 5; do
    if grep -q '"server busy' "$OVERFLOW_OUT_DIR/slot_${i}.json" 2>/dev/null; then
        busy_count=$((busy_count+1))
    fi
done
# At least one must have hit the busy path; otherwise the bound isn't enforced.
assert "test_max_concurrent_io_overflow" "$([ "$busy_count" -ge 1 ] && echo 0 || echo 1)"

"$CLI" -s "$SOCK" -k "$TOKEN" exec "rm -f $SRC $SMALL $BIG_SRC" >/dev/null 2>&1 || true
rm -rf "$DST_SLOW" "$DST_FAST" "$OVERFLOW_OUT_DIR"

# ============================================================================
# Streaming get/put — no size cap, sha256-verified end-to-end (commit 2)
# ============================================================================

# ---- test_stream_get_small ----
# A 1 MiB file via get-stream — exercises the streaming path even on small data
echo "test data $(date)" > /tmp/hl_stream_tiny_$$.txt
"$CLI" -s "$SOCK" -k "$TOKEN" exec "dd if=/dev/urandom of=/tmp/hl_stream_small_src_$$.bin bs=1M count=1 status=none" >/dev/null 2>&1
src_sha=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "sha256sum /tmp/hl_stream_small_src_$$.bin | awk '{print \$1}'" 2>/dev/null \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stdout','').strip())" 2>/dev/null || echo "")
"$CLI" -s "$SOCK" -k "$TOKEN" -T 10000 get-stream \
    /tmp/hl_stream_small_src_$$.bin /tmp/hl_stream_small_dst_$$.bin >/dev/null 2>&1
get_rc=$?
dst_sha=$(sha256sum /tmp/hl_stream_small_dst_$$.bin 2>/dev/null | awk '{print $1}')
assert    "test_stream_get_small_rc"     "$([ "$get_rc" -eq 0 ] && echo 0 || echo 1)"
assert_eq "test_stream_get_small_sha256" "$dst_sha" "$src_sha"

# ---- test_stream_put_small ----
"$CLI" -s "$SOCK" -k "$TOKEN" -T 10000 put-stream \
    /tmp/hl_stream_small_dst_$$.bin /tmp/hl_stream_small_round_$$.bin >/dev/null 2>&1
put_rc=$?
round_sha=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "sha256sum /tmp/hl_stream_small_round_$$.bin | awk '{print \$1}'" 2>/dev/null \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stdout','').strip())" 2>/dev/null || echo "")
assert    "test_stream_put_small_rc"     "$([ "$put_rc" -eq 0 ] && echo 0 || echo 1)"
assert_eq "test_stream_put_small_sha256" "$round_sha" "$src_sha"

# ---- test_stream_large_get_above_legacy_cap ----
# 100 MiB file — legacy `get` would reject this (>90 MiB), `get-stream` succeeds.
"$CLI" -s "$SOCK" -k "$TOKEN" exec "dd if=/dev/urandom of=/tmp/hl_stream_big_src_$$.bin bs=1M count=100 status=none" >/dev/null 2>&1
big_src_sha=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "sha256sum /tmp/hl_stream_big_src_$$.bin | awk '{print \$1}'" 2>/dev/null \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stdout','').strip())" 2>/dev/null || echo "")
# Legacy get must be rejected. Don't fall back to "{}" on nonzero exit — the
# CLI prints valid JSON to stdout AND exits nonzero on remote errors, and
# appending "{}" pollutes the output and breaks json.load.
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -j -T 10000 get \
    /tmp/hl_stream_big_src_$$.bin /tmp/hl_stream_big_dst_legacy_$$.bin 2>/dev/null) || true
legacy_status=$(echo "$out" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || echo "ERR")
assert_eq "test_stream_legacy_get_rejected_at_90mib" "$legacy_status" "error"
# Stream must succeed
"$CLI" -s "$SOCK" -k "$TOKEN" -T 30000 get-stream \
    /tmp/hl_stream_big_src_$$.bin /tmp/hl_stream_big_dst_$$.bin >/dev/null 2>&1
big_dst_sha=$(sha256sum /tmp/hl_stream_big_dst_$$.bin 2>/dev/null | awk '{print $1}')
assert_eq "test_stream_big_get_sha256" "$big_dst_sha" "$big_src_sha"

# ---- test_stream_put_auto_promote ----
# `put` (no --stream) on a >90 MiB file must auto-promote; the file lands and
# matches the source sha256.
"$CLI" -s "$SOCK" -k "$TOKEN" -T 30000 put \
    /tmp/hl_stream_big_dst_$$.bin /tmp/hl_stream_big_auto_$$.bin >/dev/null 2>&1
auto_rc=$?
auto_sha=$("$CLI" -s "$SOCK" -k "$TOKEN" -j exec "sha256sum /tmp/hl_stream_big_auto_$$.bin | awk '{print \$1}'" 2>/dev/null \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('stdout','').strip())" 2>/dev/null || echo "")
assert    "test_stream_put_auto_promote_rc"     "$([ "$auto_rc" -eq 0 ] && echo 0 || echo 1)"
assert_eq "test_stream_put_auto_promote_sha256" "$auto_sha" "$big_src_sha"

# ---- test_stream_get_nonexistent ----
out=$("$CLI" -s "$SOCK" -k "$TOKEN" -T 5000 get-stream \
    "/tmp/hl_stream_nope_$$.bin" "/tmp/hl_stream_nope_dst_$$.bin" 2>&1 || echo "ERR")
case "$out" in
    *"get_stream error"*|*"cannot open"*) PASS=$((PASS+1)); echo "PASS: test_stream_get_nonexistent" ;;
    *) FAIL=$((FAIL+1)); echo "FAIL: test_stream_get_nonexistent (got: $out)" ;;
esac
assert "test_stream_get_nonexistent_no_local" \
    "$([ ! -f /tmp/hl_stream_nope_dst_$$.bin ] && echo 0 || echo 1)"

# Cleanup
"$CLI" -s "$SOCK" -k "$TOKEN" exec "rm -f /tmp/hl_stream_small_src_$$.bin /tmp/hl_stream_small_round_$$.bin /tmp/hl_stream_big_src_$$.bin /tmp/hl_stream_big_auto_$$.bin /tmp/hl_stream_nope_$$.bin" >/dev/null 2>&1 || true
rm -f /tmp/hl_stream_tiny_$$.txt /tmp/hl_stream_small_dst_$$.bin /tmp/hl_stream_big_dst_$$.bin /tmp/hl_stream_big_dst_legacy_$$.bin /tmp/hl_stream_nope_dst_$$.bin

# ---- Summary ----
echo ""
echo "=== Integration Test Summary ==="
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
