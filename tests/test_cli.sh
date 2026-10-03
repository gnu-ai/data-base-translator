#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# CLI conformance test for the GNU base commands: the binary must
# answer --version and --help on stdout with exit status 0 — on
# any system, without Hurd libraries, mount points or libpq.
#
# Usage: tests/test_cli.sh [path/to/binary]
#        (default: the build tree next to this script)

set -u

BINARY="${1:-../src/db-translator}"
FAIL=0

say() { printf '%s\n' "$*"; }
fail() { say "test_cli: FAIL: $*"; FAIL=1; }

if [ ! -x "$BINARY" ]; then
    say "test_cli: SKIP: $BINARY not built"
    exit 77
fi

"$BINARY" --version >/dev/null 2>&1 || fail "--version must exit 0"
"$BINARY" --version 2>/dev/null | grep -q 'GNU AI' \
    || fail "--version must name the project"
"$BINARY" --version 2>/dev/null | grep -q 'GPLv3' \
    || fail "--version must name the license"
"$BINARY" -V >/dev/null 2>&1 || fail "-V must exit 0"
"$BINARY" --help >/dev/null 2>&1 || fail "--help must exit 0"
"$BINARY" --help 2>/dev/null | grep -q 'Usage:' \
    || fail "--help must show the usage"
"$BINARY" --help 2>/dev/null | grep -q 'Report bugs' \
    || fail "--help must tell where to report bugs"
"$BINARY" -h >/dev/null 2>&1 || fail "-h must exit 0"

if [ "$FAIL" = 0 ]; then
    say "test_cli: OK"
    exit 0
fi
exit 1
