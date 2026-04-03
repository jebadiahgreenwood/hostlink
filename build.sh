#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-release}"

case "$MODE" in
  release)
    CFLAGS="-std=c11 -O2 -DNDEBUG -Wall -Wextra -Werror -pedantic"
    LDFLAGS="-s"
    ;;
  debug)
    CFLAGS="-std=c11 -O0 -g3 -fsanitize=address,undefined -Wall -Wextra -Werror -pedantic"
    LDFLAGS="-fsanitize=address,undefined"
    ;;
  test)
    "$0" debug
    chmod +x tests/run_tests.sh tests/test_integration.sh
    exec tests/run_tests.sh --integration
    ;;
  unit)
    "$0" debug
    chmod +x tests/run_tests.sh
    exec tests/run_tests.sh
    ;;
  clean)
    make clean
    exit 0
    ;;
  *)
    echo "Usage: $0 [release|debug|test|unit|clean]" >&2
    exit 1
    ;;
esac

export CFLAGS LDFLAGS
make -j"$(nproc)" all
echo "Build complete: build/hostlinkd, build/hostlink-cli"
