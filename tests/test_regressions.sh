#!/bin/bash
# Regression suite for the v1.5.0 argument-handling / exit-status / signal fixes.
# Spins up a throwaway daemon on a private unix socket — touches nothing live.
#
# Usage: tests/test_regressions.sh [shell] [tree]
#   shell: shell the daemon execs commands with (default /bin/sh)
#   tree:  hostlink tree to test, must already be built (default: this repo)
#
# Run it for both shells — the daemon's configured shell is deployment-specific
# (the ramboot host uses /bin/bash, the example config uses /bin/sh):
#   ./build.sh release && tests/test_regressions.sh /bin/bash && tests/test_regressions.sh /bin/sh
#
# NOTE ON PID NAMESPACES: client and daemon run in the SAME namespace here, so
# the client's own argv (which does contain the command text) is visible to any
# process-matching command under test. The F8 invariant is therefore asserted
# directly — "the exec shell's cmdline does not contain the command" — which is
# namespace-independent. The end-to-end pgrep/pkill behaviour is verified
# separately against the real host daemon, where the client is in a container.
set -uo pipefail

BASE="$(cd "$(dirname "$0")/.." && pwd)"
SHELL_UNDER_TEST="${1:-/bin/sh}"
W="${2:-$BASE}"
T="${TMPDIR:-/tmp}/hl-regress-$$"
CLI="$W/build/hostlink-cli"
DAEMON="$W/build/hostlinkd"
TOKEN="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

rm -rf "$T"; mkdir -p "$T/out"
cat > "$T/hostlink.conf" <<EOF
node_name = testnode
auth_token = $TOKEN
unix_enabled = true
unix_path = $T/hl.sock
unix_mode = 0600
tcp_enabled = false
max_concurrent = 4
max_concurrent_io = 2
default_timeout_ms = 30000
max_timeout_ms = 300000
shell = $SHELL_UNDER_TEST
default_max_output_bytes = 4194304
max_output_bytes = 67108864
output_tmpdir = $T/out
log_target = $T/daemon.log
log_level = info
job_max_spool_bytes = 65536
job_retention_s = 60
EOF

"$DAEMON" -c "$T/hostlink.conf" -p "$T/hl.pid" -f > "$T/daemon.stderr" 2>&1 &
DPID=$!
trap 'kill $DPID 2>/dev/null' EXIT
# Wait for the daemon to actually ANSWER, not merely for the socket file to
# exist: bind() creates the path well before the accept loop is running, and on
# a slower box that window is wide enough to fail the first assertion.
ready=0
for _ in $(seq 1 100); do
  if [ -S "$T/hl.sock" ] && "$CLI" -s "$T/hl.sock" -k "$TOKEN" ping >/dev/null 2>&1; then
    ready=1; break
  fi
  sleep 0.1
done
[ "$ready" = "1" ] || { echo "FATAL: daemon did not become ready"; cat "$T/daemon.stderr"; exit 1; }

hl() { "$CLI" -s "$T/hl.sock" -k "$TOKEN" "$@"; }

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m  %s\n     %s\n' "$1" "$2"; }
check_eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1" "expected [$3] got [$2]"; }

echo "=== testing $W  (shell: $SHELL_UNDER_TEST) ==="

echo "--- F7: remote exit status passes through verbatim ---"
for code in 0 1 2 7 42 100 200; do
  hl exec "exit $code" >/dev/null 2>&1
  check_eq "remote exit $code propagates" "$?" "$code"
done
hl exec "false" >/dev/null 2>&1; check_eq "false -> 1" "$?" "1"
hl exec "grep -q nomatch /dev/null" >/dev/null 2>&1; check_eq "grep no-match -> 1" "$?" "1"
hl exec 'sh -c "kill -TERM \$\$"' >/dev/null 2>&1
check_eq "signal death -> 128+15" "$?" "143"
( hl exec "exit 3" >/dev/null 2>&1 && echo CONT || echo STOP ) | grep -q STOP \
  && ok "&& chain stops on remote failure" || bad "&& chain" "chain continued"
( hl exec "true" >/dev/null 2>&1 && echo CONT || echo STOP ) | grep -q CONT \
  && ok "&& chain continues on remote success" || bad "&& chain ok" "chain stopped"

echo "--- F7: hostlink's own failures use the reserved block ---"
"$CLI" -s "$T/nonexistent.sock" -k "$TOKEN" exec "true" >/dev/null 2>&1
check_eq "unreachable socket -> 125" "$?" "125"
hl -k "wrongtokenwrongtokenwrongtoken00" exec "true" >/dev/null 2>&1
check_eq "bad token -> 125" "$?" "125"
hl -T 400 exec "sleep 5" >/dev/null 2>&1
check_eq "timeout -> 124" "$?" "124"

echo "--- F4: daemon rejections are no longer reported as success ---"
hl exec "" >/dev/null 2>&1
check_eq "empty command -> 125 (daemon says bad_request)" "$?" "125"
# NB: capture first, then grep. Piping hl into grep under `set -o pipefail`
# takes hl's (now meaningful) exit status as the pipeline's status.
msg=$(hl exec "" 2>&1)
case "$msg" in *"command is required"*) ok "bad_request message reaches the user";;
                *) bad "bad_request msg" "got [$msg]";; esac
big=$(head -c 9000 /dev/zero | tr '\0' 'x')
hl exec "echo $big" >/dev/null 2>&1
check_eq "9KB command -> 125 (rejected, not truncated)" "$?" "125"
msg=$(hl exec "echo $big" 2>&1)
case "$msg" in *"too long"*) ok "oversized command explains itself";;
                *) bad "oversize msg" "got [$msg]";; esac
out=$(hl exec "echo $big" 2>/dev/null | wc -c)
check_eq "oversized command produces no output at all" "$out" "0"

echo "--- F1: exec joins all remaining arguments ---"
check_eq "multi-token exec runs whole command" "$(hl exec echo hello world 2>/dev/null)" "hello world"
check_eq "exec stat -c%s" "$(hl exec stat -c%s /etc/hostname 2>/dev/null)" "$(stat -c%s /etc/hostname)"
check_eq "exec uname -n" "$(hl exec uname -n 2>/dev/null)" "$(uname -n)"
check_eq "exec ls <dir> lists that dir, not /" \
  "$(hl exec ls /etc/ssl 2>/dev/null | head -1)" "$(ls /etc/ssl | head -1)"
check_eq "single quoted token still works (the wrapper path)" \
  "$(hl exec 'echo joined once' 2>/dev/null)" "joined once"
check_eq "flags after exec belong to the command, as with ssh" \
  "$(hl exec echo -n abc 2>/dev/null)" "abc"

echo "--- F8: the command text never reaches the exec shell's cmdline ---"
# The shell prints its OWN /proc/<pid>/cmdline. Before the fix that contained
# the whole command; now it is a constant stub.
cl=$(hl exec 'MARKER_9f3b_probe=1; tr "\0" " " < /proc/$$/cmdline' 2>/dev/null)
echo "$cl" | grep -q 'MARKER_9f3b_probe' \
  && bad "exec shell cmdline leaks the command" "cmdline=[$cl]" \
  || ok "exec shell cmdline does not contain the command"
echo "$cl" | grep -q 'HOSTLINK_COMMAND' \
  && ok "exec shell cmdline is the constant stub" || bad "stub missing" "cmdline=[$cl]"
check_eq "HOSTLINK_COMMAND is unset for the command and its children" \
  "$(hl exec 'echo "[${HOSTLINK_COMMAND:-unset}]"' 2>/dev/null)" "[unset]"
check_eq "child processes do not inherit it either" \
  "$(hl exec 'sh -c "echo [\${HOSTLINK_COMMAND:-unset}]"' 2>/dev/null)" "[unset]"

echo "--- F2: transfer flags work in any position ---"
echo "payload-$$" > "$T/src.txt"
rm -rf "$T/d1"; hl put "$T/src.txt" "$T/d1/nested/deep/file.txt" --mkdir >/dev/null 2>&1; rc=$?
[ "$rc" = "0" ] && [ -f "$T/d1/nested/deep/file.txt" ] \
  && ok "put --mkdir AFTER the paths creates parents" \
  || bad "put trailing --mkdir" "rc=$rc exists=$([ -f "$T/d1/nested/deep/file.txt" ] && echo y || echo n)"
rm -rf "$T/d2"; hl put --mkdir "$T/src.txt" "$T/d2/a/b.txt" >/dev/null 2>&1; rc=$?
[ "$rc" = "0" ] && [ -f "$T/d2/a/b.txt" ] \
  && ok "put --mkdir BEFORE the paths creates parents" \
  || bad "put leading --mkdir" "rc=$rc exists=$([ -f "$T/d2/a/b.txt" ] && echo y || echo n)"
rm -rf "$T/d3"; hl put "$T/src.txt" --mkdir "$T/d3/a/b.txt" >/dev/null 2>&1
[ -f "$T/d3/a/b.txt" ] && ok "put --mkdir BETWEEN the paths creates parents" || bad "put middle --mkdir" "missing"
hl put "$T/src.txt" "$T/d4.txt" --mode 600 >/dev/null 2>&1
check_eq "put --mode after the paths applies" "$(stat -c%a "$T/d4.txt" 2>/dev/null)" "600"
hl put "$T/src.txt" "$T/d5.txt" --bogus-flag >/dev/null 2>&1
check_eq "unknown trailing flag -> 125 (was silently ignored)" "$?" "125"
hl put "$T/src.txt" "$T/d6.txt" extra-positional >/dev/null 2>&1
check_eq "third path -> 125" "$?" "125"
hl put "$T/src.txt" >/dev/null 2>&1
check_eq "missing second path -> 125" "$?" "125"
hl put "$T/src.txt" "$T/d7.txt" --mode >/dev/null 2>&1
check_eq "--mode with no argument -> 125" "$?" "125"

echo "--- transfers still work ---"
rm -f "$T/g.txt"; hl get "$T/src.txt" "$T/g.txt" >/dev/null 2>&1
[ -f "$T/g.txt" ] && cmp -s "$T/src.txt" "$T/g.txt" && ok "get round-trips intact" || bad "get" "missing/differs"
rm -f "$T/gs.txt"; hl get-stream "$T/src.txt" "$T/gs.txt" >/dev/null 2>&1
[ -f "$T/gs.txt" ] && cmp -s "$T/src.txt" "$T/gs.txt" && ok "get-stream round-trips intact" || bad "get-stream" "missing/differs"
rm -rf "$T/ps"; hl put-stream "$T/src.txt" "$T/ps/x.txt" --mkdir >/dev/null 2>&1
[ -f "$T/ps/x.txt" ] && ok "put-stream honours --mkdir in any position" || bad "put-stream --mkdir" "missing"
head -c 2000000 /dev/urandom > "$T/big.bin"
rm -f "$T/big.out"; hl put "$T/big.bin" "$T/big.out" >/dev/null 2>&1
cmp -s "$T/big.bin" "$T/big.out" && ok "2MB binary put round-trips byte-identical" || bad "big put" "differs"

echo "--- signal environment matches an ordinary shell ---"
# The daemon blocks HUP/INT/TERM/CHLD for its signalfd and ignores PIPE; masks
# survive fork+exec and SIG_IGN survives exec, so all of that used to leak into
# every remote command. Compare against the same command run locally.
if [ "$SHELL_UNDER_TEST" = "/bin/bash" ]; then   # PIPESTATUS is a bash-ism
  check_eq "a SIGPIPE producer dies with 141, as it does locally" \
    "$(hl exec 'yes | head -1 >/dev/null; echo ${PIPESTATUS[0]}' 2>/dev/null)" \
    "$(bash -c 'yes | head -1 >/dev/null; echo ${PIPESTATUS[0]}')"
fi
check_eq "remote command can be terminated by its own kill" \
  "$(hl exec 'sh -c "kill -TERM \$\$"' >/dev/null 2>&1; echo $?)" "143"
# Reference must use the SAME shell: bash blocks SIGCHLD around an eval, dash
# does not, so the expected mask is shell-dependent (and neither is hostlink's).
check_eq "signal mask matches that shell run locally, with no hostlink" \
  "$(hl exec 'grep SigBlk /proc/$$/status' 2>/dev/null)" \
  "$("$SHELL_UNDER_TEST" -c '__c="grep SigBlk /proc/\$\$/status"; eval "$__c"')"
# The timeout path TERMs the process group; with TERM blocked that was ignored
# and every timeout waited out the full 2s escalation to SIGKILL.
s=$( date +%s%N ); hl -T 500 exec 'sleep 10' >/dev/null 2>&1; e=$( date +%s%N )
ms=$(( (e - s) / 1000000 ))
[ "$ms" -lt 1500 ] && ok "timeout stops promptly (${ms}ms, graceful TERM honoured)" \
                   || bad "timeout escalation" "took ${ms}ms — TERM still blocked?"

echo "--- output preserved on timeout ---"
got=$(hl -T 1200 exec "echo before-the-timeout; sleep 5" 2>/dev/null)
check_eq "partial stdout survives a timeout" "$got" "before-the-timeout"

echo "--- regression: ordinary behaviour unchanged ---"
check_eq "stdout passthrough" "$(hl exec 'echo plain' 2>/dev/null)" "plain"
check_eq "stderr passthrough" "$(hl exec 'echo err >&2' 2>&1 >/dev/null | tail -1)" "err"
check_eq "workdir" "$(hl -w /etc exec pwd 2>/dev/null)" "/etc"
check_eq "env override" "$(hl -e FOO=bar exec 'echo $FOO' 2>/dev/null)" "bar"
check_eq "env override does not clobber inherited PATH" \
  "$(hl -e FOO=bar exec 'test -n "$PATH" && echo haspath' 2>/dev/null)" "haspath"
check_eq "quoting: nested single quotes" "$(hl exec "echo 'it'\''s fine'" 2>/dev/null)" "it's fine"
check_eq "quoting: command substitution" "$(hl exec 'echo "count=$(echo a b c | wc -w)"' 2>/dev/null)" "count=3"
check_eq "quoting: shell function definition" "$(hl exec 'f() { echo "hi $1"; }; f there' 2>/dev/null)" "hi there"
# printf, not echo: dash's echo builtin interprets backslash escapes (\c ends
# output), which is dash behaviour and identical with or without hostlink.
check_eq "quoting: literal dollar and backslash" "$(hl exec 'printf "%s\n" "a\$b\\c"' 2>/dev/null)" 'a$b\c'
check_eq "multiline command" "$(hl exec 'echo one
echo two' 2>/dev/null | tr '\n' ',')" "one,two,"
check_eq "pipeline exit status" "$(hl exec 'echo x | grep -q y'; echo $?)" "1"
check_eq "large output not truncated" "$(hl exec 'seq 1 20000 | tail -1' 2>/dev/null)" "20000"
hl ping >/dev/null 2>&1; check_eq "ping ok" "$?" "0"
hl exec 'echo detach-target' -D >/dev/null 2>&1; check_eq "detach flag before subcmd" "$(hl -D exec 'true' >/dev/null 2>&1; echo $?)" "0"

# ---- build a source tree with nesting, an empty dir, a symlink, odd names ----
SRC="$T/src"
mkdir -p "$SRC/a/b/c" "$SRC/assets/img" "$SRC/empty" "$SRC/.cache/blobs"
echo top            > "$SRC/top.txt"
echo deep           > "$SRC/a/b/c/deep.txt"
echo img            > "$SRC/assets/img/logo.bin"
echo asset          > "$SRC/assets/style.css"
echo cached         > "$SRC/.cache/blobs/junk.bin"
echo cachedtop      > "$SRC/.cache/meta.json"
echo 'spaces here'  > "$SRC/a/name with spaces.txt"
echo obj            > "$SRC/a/b/thing.o"
head -c 300000 /dev/urandom > "$SRC/assets/big.bin"
ln -s top.txt        "$SRC/link-to-top"
ln -s /etc           "$SRC/link-to-etc"
NREG=$(find "$SRC" -type f | wc -l)

echo "=== F3: directory put ==="
echo "--- basic recursive put ---"
rm -rf "$T/dst"
out=$(hl put "$SRC" "$T/dst" 2>&1); rc=$?
check_eq "put of a directory succeeds" "$rc" "0"
check_eq "all regular files landed" "$(find "$T/dst" -type f | wc -l)" "$NREG"
check_eq "nested structure mirrored" "$(cat "$T/dst/a/b/c/deep.txt" 2>/dev/null)" "deep"
check_eq "file with spaces in the name" "$(cat "$T/dst/a/name with spaces.txt" 2>/dev/null)" "spaces here"
cmp -s "$SRC/assets/big.bin" "$T/dst/assets/big.bin" && ok "300KB binary byte-identical" || bad "big.bin" "differs"
echo "$out" | grep -q "$NREG files" && ok "summary reports the file count" || bad "summary" "got [$(echo "$out"|head -1)]"

echo "--- symlinks are not followed (parity with get's FTW_PHYS walk) ---"
[ ! -e "$T/dst/link-to-top" ] && ok "symlink not transferred" || bad "symlink" "it was transferred"
[ ! -d "$T/dst/link-to-etc" ] && ok "symlinked dir not descended into" || bad "symlink dir" "descended"
echo "$out" | grep -q 'skipped 2 symlink' && ok "skipped symlinks are reported, not silent" || bad "skip report" "not mentioned"

echo "--- empty directories (documented shared limitation with get) ---"
[ ! -d "$T/dst/empty" ] && ok "empty dir absent, as with get (files-only transfer)" \
                        || bad "empty dir" "unexpectedly present"

echo "--- --exclude ---"
rm -rf "$T/dst2"
hl put "$SRC" "$T/dst2" --exclude .cache >/dev/null 2>&1
[ ! -e "$T/dst2/.cache" ] && ok "--exclude .cache prunes the whole subtree" || bad "exclude dir" "still there"
check_eq "non-excluded files still all present" "$(find "$T/dst2" -type f | wc -l)" "$((NREG-2))"
rm -rf "$T/dst3"
hl put "$SRC" "$T/dst3" --exclude '*.o' --exclude '.cache' >/dev/null 2>&1
[ ! -e "$T/dst3/a/b/thing.o" ] && ok "--exclude '*.o' matches basenames" || bad "exclude glob" "still there"
check_eq "two excludes compose" "$(find "$T/dst3" -type f | wc -l)" "$((NREG-3))"
rm -rf "$T/dst4"
hl put "$SRC" "$T/dst4" --exclude 'assets/style.css' >/dev/null 2>&1
[ ! -e "$T/dst4/assets/style.css" ] && ok "--exclude matches a full relative path" || bad "exclude relpath" "still there"
[ -e "$T/dst4/assets/img/logo.bin" ] && ok "sibling under same dir untouched" || bad "exclude overreach" "removed too much"
rm -rf "$T/dst5"; hl put --exclude .cache "$SRC" "$T/dst5" >/dev/null 2>&1
[ ! -e "$T/dst5/.cache" ] && ok "--exclude works before the paths too" || bad "exclude position" "ignored"

echo "--- round trip: put a tree, get it back, compare ---"
rm -rf "$T/back"
hl get "$T/dst" "$T/back" >/dev/null 2>&1
if diff -r "$T/dst" "$T/back" >/dev/null 2>&1; then ok "put -> get round-trips identically"
else bad "round trip" "$(diff -r "$T/dst" "$T/back" 2>&1 | head -3)"; fi
check_eq "put and get agree on the file set" \
  "$(cd "$T/dst" && find . -type f | sort | md5sum)" \
  "$(cd "$T/back" && find . -type f | sort | md5sum)"

echo "--- error handling ---"
echo blocker > "$T/blocker"
hl put "$SRC" "$T/blocker" >/dev/null 2>&1
check_eq "remote destination is an existing file -> 125" "$?" "125"
hl put "$SRC" "$T/dstX" -r >/dev/null 2>&1
check_eq "-r rejected with guidance (dirs are automatic)" "$?" "125"
msg=$(hl put "$SRC" "$T/dstX" -r 2>&1)
case "$msg" in *"not needed"*) ok "-r message explains why";; *) bad "-r msg" "got [$msg]";; esac
hl put "$SRC" "$T/dstY" --exclude >/dev/null 2>&1
check_eq "--exclude with no pattern -> 125" "$?" "125"
mkdir -p "$T/allexcluded/sub"; echo x > "$T/allexcluded/sub/only.o"
hl put "$T/allexcluded" "$T/dstZ" --exclude '*.o' >/dev/null 2>&1
check_eq "everything excluded -> 0, not a spurious failure" "$?" "0"
hl put "$T/no-such-dir" "$T/dstW" >/dev/null 2>&1
check_eq "missing local dir -> 125" "$?" "125"

echo "--- single-file put unchanged ---"
rm -f "$T/one.txt"; hl put "$SRC/top.txt" "$T/one.txt" >/dev/null 2>&1
check_eq "file put still works" "$(cat "$T/one.txt" 2>/dev/null)" "top"
rm -rf "$T/mk"; hl put "$SRC/top.txt" "$T/mk/deep/one.txt" --mkdir >/dev/null 2>&1
[ -f "$T/mk/deep/one.txt" ] && ok "file put --mkdir still works" || bad "file mkdir" "missing"

echo "--- deep trees: correct or refused, never a truncated (wrong) path ---"
DEEP="$T/deep"; P="$DEEP"
for i in $(seq 1 25); do P="$P/component_$i"; done
mkdir -p "$P"; echo deeppayload > "$P/leaf.txt"
rm -rf "$T/deepdst"; hl put "$DEEP" "$T/deepdst" >/dev/null 2>&1; rc=$?
land=$(find "$T/deepdst" -name leaf.txt 2>/dev/null | head -1)
want="$T/deepdst${P#$DEEP}/leaf.txt"
if [ "$rc" = "0" ] && [ "$land" = "$want" ]; then ok "deep tree lands at the exact right path"
elif [ "$rc" != "0" ] && [ -z "$land" ]; then ok "over-long path refused outright (no wrong-path write)"
else bad "deep path" "rc=$rc landed=[$land] wanted=[$want]"; fi
# and a path that definitely exceeds the composed limit must fail loudly, not truncate
VERY="$T/very"; Q="$VERY"
for i in $(seq 1 70); do Q="$Q/padding_component_number_$i"; done
mkdir -p "$Q" 2>/dev/null && echo x > "$Q/leaf.txt" 2>/dev/null
if [ -f "$Q/leaf.txt" ]; then
  msg=$(hl put "$VERY" "$T/verydst" 2>&1); rc=$?
  case "$msg" in
    *"path too long"*) ok "over-limit path reports 'path too long'";;
    *) [ "$rc" != "0" ] && ok "over-limit path fails (message: $(echo "$msg"|tail -1|cut -c1-50))" \
                        || bad "path limit" "silently succeeded";;
  esac
  [ -z "$(find "$T/verydst" -name leaf.txt 2>/dev/null)" ] && ok "nothing written at a truncated path" \
    || bad "truncated write" "a file landed somewhere wrong"
fi

echo "--- streaming path inside a directory put ---"
rm -rf "$T/strm"; hl put "$SRC" "$T/strm" --stream >/dev/null 2>&1
check_eq "--stream forces sha256-verified streaming for every file" "$(find "$T/strm" -type f | wc -l)" "$NREG"
cmp -s "$SRC/assets/big.bin" "$T/strm/assets/big.bin" && ok "streamed binary byte-identical" || bad "stream big" "differs"
n=$(hl put "$SRC" "$T/strm2" --stream 2>&1 | grep -c 'put_stream ok')
check_eq "each file reports its own sha256 (an implicit manifest)" "$n" "$NREG"

echo "--- trailing slash on the source is harmless ---"
rm -rf "$T/dstS"; hl put "$SRC/" "$T/dstS" >/dev/null 2>&1
check_eq "put dir/ (trailing slash) same file count" "$(find "$T/dstS" -type f | wc -l)" "$NREG"

####################################################################
# v1.6.0 — jobs, per-target timeouts, verify manifest, remote df
####################################################################

echo
echo "=== F6.1: detached jobs ==="

# The id lands on STDOUT so it can be captured; the human line goes to stderr.
JID=$(hl -D exec 'echo out-1; echo err-1 >&2; sleep 1; echo out-2; exit 7' 2>/dev/null)
case "$JID" in
  [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])
    ok "-D returns a 12-hex job id on stdout" ;;
  *) bad "-D job id" "got [$JID]" ;;
esac

state() { hl -j job status "$1" 2>/dev/null | sed 's/.*"state":"\([a-z]*\)".*/\1/'; }
check_eq "a job in flight reports running" "$(state "$JID")" "running"

hl job wait "$JID" >/dev/null 2>&1
check_eq "job wait exits with the job's own status" "$?" "7"
check_eq "job wait leaves it terminal"  "$(state "$JID")" "exited"
check_eq "tail returns the whole spool" "$(hl job tail "$JID" 2>/dev/null | tr '\n' ',')" "out-1,out-2,"
check_eq "tail --stderr reads the other stream" "$(hl job tail "$JID" --stderr 2>/dev/null)" "err-1"
hl job list 2>/dev/null | grep -q "$JID" && ok "job list includes it" || bad "job list" "missing $JID"

# Incremental tail: a second read from next_offset must return only what is new,
# which is what makes --follow cheap instead of re-sending the log every poll.
off=$(hl -j job tail "$JID" 2>/dev/null | sed 's/.*"next_offset":\([0-9]*\).*/\1/')
[ "$off" -gt 0 ] 2>/dev/null && ok "tail reports a next_offset" || bad "next_offset" "got [$off]"

echo "--- a job outlives the wire timeout that would kill an exec ---"
# With -D, -T stops meaning "how long this client waits" and starts meaning
# "how long the job may run". The client returns at once either way; the
# command below outlives the 2s an attached exec -T 2000 would have allowed.
start=$(date +%s)
JID2=$(hl -T 20000 -D exec 'sleep 3; echo lived' 2>/dev/null)
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -le 1 ] && ok "-D returns immediately, without waiting for the command" \
                     || bad "-D return" "took ${elapsed}s"
sleep 4
check_eq "and the job ran to completion under its own -T bound" \
         "$(hl job tail "$JID2" 2>/dev/null)" "lived"
JID3=$(hl -D exec 'sleep 0.1' 2>/dev/null)
tmo=$(hl -j job status "$JID3" 2>/dev/null | sed 's/.*"timeout_ms":\([0-9]*\).*/\1/')
[ "$tmo" -ge 3600000 ] 2>/dev/null && ok "a plain -D job gets the daemon's job default, not 30s" \
                                   || bad "job default timeout" "got [$tmo]"

echo "--- job timeout ---"
JID4=$(hl -T 800 -D exec 'echo before-timeout; sleep 30' 2>/dev/null)
hl job wait "$JID4" >/dev/null 2>&1
check_eq "an over-running job is killed and reports 124" "$?" "124"
check_eq "its state is timeout" "$(state "$JID4")" "timeout"
check_eq "output produced before the kill is kept" "$(hl job tail "$JID4" 2>/dev/null)" "before-timeout"

echo "--- cancel reaches the process group, not just the shell ---"
MARKER="$T/cancel-marker-$$"
JID5=$(hl -D exec "sleep 60 & echo \$! > $MARKER; wait" 2>/dev/null)
sleep 0.6
CHILD=$(cat "$MARKER" 2>/dev/null)
hl job cancel "$JID5" >/dev/null 2>&1
check_eq "cancel is accepted" "$?" "0"
sleep 0.6
check_eq "the job is terminal after cancel" "$(state "$JID5")" "exited"
if [ -n "$CHILD" ]; then
  kill -0 "$CHILD" 2>/dev/null && bad "cancel reach" "grandchild $CHILD survived" \
                               || ok "a grandchild of the job was killed too"
fi
hl job cancel "$JID5" >/dev/null 2>&1
check_eq "cancelling a finished job fails cleanly" "$?" "1"

echo "--- a job id is a path component, so it is validated ---"
for bad_id in '../../etc/passwd' 'abc' 'ABCDEF012345' '0123456789abx' '' ; do
  hl job status "$bad_id" >/dev/null 2>&1
  check_eq "rejected job id [$bad_id]" "$?" "125"
done
hl job status 0123456789ab >/dev/null 2>&1
check_eq "well-formed but unknown id -> not found" "$?" "1"
hl job frobnicate 0123456789ab >/dev/null 2>&1
check_eq "unknown job action -> 125" "$?" "125"
hl job tail "$JID" --nonsense >/dev/null 2>&1
check_eq "unknown job flag -> 125 (not ignored)" "$?" "125"

echo "--- the spool is capped, and says so ---"
JID6=$(hl -D exec 'seq 1 200000; exit 5' 2>/dev/null)
hl job wait "$JID6" >/dev/null 2>&1
check_eq "the command still finishes normally past the cap" "$?" "5"
check_eq "the spool stops exactly at job_max_spool_bytes" \
         "$(stat -c%s "$T/out/jobs/$JID6/stdout" 2>/dev/null)" "65536"
# Not `hl ... | grep -q`: under pipefail the pipeline inherits hl's exit status,
# which for a job that exited 5 is 5 — a passing grep would read as a failure.
cap_out=$(hl job status "$JID6" 2>&1)
case "$cap_out" in
  *CAPPED*) ok "truncation is reported, with the produced size" ;;
  *)        bad "cap report" "no CAPPED in: $(echo "$cap_out" | tail -1)" ;;
esac

echo "--- finished spools are collected, running ones are not ---"
touch -d '1 hour ago' "$T/out/jobs/$JID6/status"
JID7=$(hl -D exec 'true' 2>/dev/null)     # submission triggers the sweep
sleep 0.3
[ ! -d "$T/out/jobs/$JID6" ] && ok "an aged finished job is swept" || bad "gc" "$JID6 still present"
[ -d "$T/out/jobs/$JID7" ]   && ok "the fresh job survives the sweep" || bad "gc overreach" "swept $JID7"

echo
echo "=== F6.2: per-target default timeout ==="
cat > "$T/targets.conf" <<TEOF
[slow]
transport = unix
socket = $T/hl.sock
token = $TOKEN
timeout_ms = 4000

[quick]
transport = unix
socket = $T/hl.sock
token = $TOKEN
timeout_ms = 300
TEOF
# HOSTLINK_TOKEN, if the environment has one, silently overrides every
# per-target token — the pitfall the bin/ wrappers all guard against with
# `env -u`. A test that reads tokens from a file has to do the same.
tgt() { local t="$1"; shift; env -u HOSTLINK_TOKEN "$CLI" --targets-file "$T/targets.conf" -t "$t" "$@"; }
tgt slow exec 'sleep 1; echo patient' >/dev/null 2>&1
check_eq "a target default above 1s lets a 1s command finish" "$?" "0"
tgt quick exec 'sleep 1; echo nope' >/dev/null 2>&1
check_eq "a short target default times out (300ms < 1s)" "$?" "124"
tgt quick -T 4000 exec 'sleep 1; echo override' >/dev/null 2>&1
check_eq "an explicit -T overrides the target default" "$?" "0"

echo "--- the daemon's clamp is no longer silent ---"
hl -T 999000 exec 'true' 2>&1 | grep -q 'exceeds this daemon' \
  && ok "-T above max_timeout_ms warns and reports the effective value" \
  || bad "clamp warning" "no warning printed"
hl -T 1000 exec 'true' 2>&1 | grep -q 'exceeds this daemon' \
  && bad "clamp warning" "warned when nothing was clamped" \
  || ok "no warning when the requested timeout is honoured"

echo
echo "=== F5: verify manifest on a directory put ==="
rm -rf "$T/vdst"
man_out=$(hl put "$SRC" "$T/vdst" --verify 2>"$T/verify.err")
check_eq "one sha256 line per file" "$(echo "$man_out" | grep -c '^[0-9a-f]\{64\}  ')" "$NREG"
grep -q "$NREG of $NREG files sha256-verified" "$T/verify.err" \
  && ok "summary counts every file" || bad "manifest summary" "$(cat "$T/verify.err" | tail -2)"
# The digests must be the real thing, not something we made up: check the
# manifest against the local files with sha256sum's own -c.
(cd "$SRC" && echo "$man_out" | sha256sum -c --status -) \
  && ok "every digest matches the local file (sha256sum -c)" \
  || bad "manifest digests" "sha256sum -c rejected the manifest"
check_eq "and the files actually landed" "$(find "$T/vdst" -type f | wc -l)" "$NREG"
# Wrappers pass transfer flags BEFORE the subcommand (hl-put does), so --verify
# has to work there too — accepting a flag in only one position is how F2
# happened in the first place.
rm -rf "$T/vdst2"
n=$(hl --verify put "$SRC" "$T/vdst2" 2>/dev/null | grep -c '^[0-9a-f]\{64\}  ')
check_eq "--verify works before the subcommand as well" "$n" "$NREG"

echo
echo "=== remote free space (stat_fs) ==="
free=$("$CLI" -s "$T/hl.sock" -k "$TOKEN" df "$T" 2>/dev/null | sed 's/ free.*//')
[ "$free" -gt 0 ] 2>/dev/null && ok "df reports free bytes for an existing path" || bad "df" "got [$free]"
deep=$("$CLI" -s "$T/hl.sock" -k "$TOKEN" df "$T/does/not/exist/yet" 2>/dev/null)
echo "$deep" | grep -q 'nearest existing ancestor' \
  && ok "df walks up to the nearest existing ancestor (the put case)" \
  || bad "df ancestor walk" "got [$deep]"
free2=$(echo "$deep" | sed 's/ free.*//')
check_eq "and reports that filesystem's free space" "$free2" "$free"

# /dev/shm is 64 MiB in this container — small enough to refuse a put against,
# without needing root to mount anything.
if [ -d /dev/shm ] && [ "$(df -k /dev/shm | awk 'NR==2{print $2}')" -lt 1048576 ]; then
  BIG="$T/big"; mkdir -p "$BIG"
  truncate -s 80M "$BIG/huge.bin"        # sparse: costs no disk, real st_size
  rm -rf /dev/shm/hltest
  out=$(hl put "$BIG" /dev/shm/hltest 2>&1); rc=$?
  check_eq "a put larger than the remote filesystem is refused" "$rc" "125"
  echo "$out" | grep -q 'not enough space' && ok "and says so before transferring" \
                                           || bad "space refusal" "got [$(echo "$out"|head -1)]"
  [ ! -e /dev/shm/hltest ] && ok "nothing was written on the far side" \
                           || bad "space refusal" "it started transferring anyway"
  rm -rf /dev/shm/hltest "$BIG"
else
  echo "  SKIP  no small filesystem available to test the refusal path"
fi
rm -rf "$T/okdst"
hl put "$SRC" "$T/okdst" >/dev/null 2>&1
check_eq "a put that fits is not refused (no false positive)" "$?" "0"

echo
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
