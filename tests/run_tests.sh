#!/usr/bin/env bash
# Run all hostlink tests: unit tests and (optionally) integration tests.
# Usage: ./tests/run_tests.sh [--integration]
set -euo pipefail
cd "$(dirname "$0")/.."

RUN_INTEGRATION=0
for arg in "$@"; do
    [ "$arg" = "--integration" ] && RUN_INTEGRATION=1
done

PASS=0
FAIL=0

run_test() {
    local bin="$1" name="$2"
    if "$bin"; then
        echo "PASS: $name"; PASS=$((PASS + 1))
    else
        echo "FAIL: $name"; FAIL=$((FAIL + 1))
    fi
}

echo "=== Building unit tests ==="
CFLAGS="${CFLAGS:--std=c11 -O0 -g3 -Wall -Wextra -pedantic}"
mkdir -p build

touch tests/fixtures/empty.conf

# Build cJSON without strict warnings (it's external code)
gcc -std=c11 -O0 -w -Isrc -Isrc/common -c src/common/cjson/cJSON.c -o build/cJSON.o

gcc $CFLAGS -Isrc -Isrc/common \
    tests/test_protocol.c \
    src/common/protocol.c \
    src/common/log.c \
    build/cJSON.o \
    -o build/test_protocol

gcc $CFLAGS -Isrc -Isrc/common \
    tests/test_config.c \
    src/common/config.c \
    src/common/log.c \
    src/common/util.c \
    build/cJSON.o \
    -o build/test_config

echo ""
echo "=== Running unit tests ==="
run_test build/test_protocol "protocol"
run_test build/test_config   "config"

if [ "$RUN_INTEGRATION" -eq 1 ]; then
    echo ""
    echo "=== Running integration tests ==="
    if [ ! -f build/hostlinkd ] || [ ! -f build/hostlink-cli ]; then
        echo "FAIL: integration (binaries not found, run 'make all' first)"
        FAIL=$((FAIL + 1))
    else
        chmod +x tests/test_integration.sh
        if bash tests/test_integration.sh; then
            echo "PASS: integration"
            PASS=$((PASS + 1))
        else
            echo "FAIL: integration"
            FAIL=$((FAIL + 1))
        fi
    fi
fi

echo ""
echo "=== Summary ==="
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
