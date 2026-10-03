#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# Protocol test over the verification binary: the same engine as
# the mounted translator, over standard streams (SPEC.md section
# 6).  It runs on any POSIX system, with or without libpq:
# transport dependent assertions are only made when the binary
# was built without the driver.

set -u

BINARY="${1:-../src/db-translator}"
FAIL=0

say() { printf '%s\n' "$*"; }
fail() { say "test_protocol: FAIL: $*"; FAIL=1; }

if [ ! -x "$BINARY" ]; then
    say "test_protocol: SKIP: $BINARY not built"
    exit 77
fi

# --- 1. The status request answers the state of the base ----------
OUT=$(printf '%s\n' '{"status": true}' | "$BINARY" 2>/dev/null)
[ "$?" = 0 ] || fail "status request must exit 0"
printf '%s' "$OUT" | grep -q '^{"connected":' \
    || fail "status must be one JSON line starting with connected"
printf '%s' "$OUT" | grep -q '"schema":' \
    || fail "status must report the schema"
printf '%s' "$OUT" | grep -q '"writes":' \
    || fail "status must report the counters"
printf '%s' "$OUT" | grep -q '"libpq":' \
    || fail "status must report the driver"

# --- 2. Contract violations are answers, never crashes -----------
OUT=$(printf '%s\n' '{"table": "runs", "row":' | "$BINARY" 2>/dev/null)
printf '%s' "$OUT" | grep -q '{"invalid": "json"}' \
    || fail "malformed JSON must answer invalid json"

OUT=$(printf '%s\n' \
    '{"table": "training_data", "row": {"source_url": "u", "content": "c", "checksum": "abc"}}' \
    | "$BINARY" 2>/dev/null)
printf '%s' "$OUT" | grep -q '{"invalid": "checksum"}' \
    || fail "bad checksum must answer invalid checksum"

OUT=$(printf '%s\n' '{"select": "runs", "where": {}}' | "$BINARY" 2>/dev/null)
printf '%s' "$OUT" | grep -q '{"invalid": "select"}' \
    || fail "select must be refused until phase 2"

OUT=$(printf '%s\n' \
    '{"table": "incidents", "row": {"kind": "crash"}}' \
    | "$BINARY" 2>/dev/null)
printf '%s' "$OUT" | grep -q '{"invalid": "table"}' \
    || fail "unknown table must answer invalid table"

# --- 3. Counters accumulate across the session --------------------
# Two invalid lines, then a status: the counter must read 2.
IN='{"table": "runs", "row": {}}
{"table": "training_data", "row": {"source_url": "u", "content": "c", "checksum": "zz"}}
{"status": true}'
OUT=$(printf '%s\n' "$IN" | "$BINARY" 2>/dev/null)
printf '%s' "$OUT" | grep -q '"invalid": 2}' \
    || fail "the status must count the two invalid lines"

# --- 4. The toolong line is refused, not crash --------------------
# DBT_MAX_LINE is 256 KiB: a line of 256 KiB + 16 without newline
# must answer invalid toolong and the session must continue.
LONG=$(head -c 262160 /dev/zero | tr '\0' 'a')
OUT=$(printf '{"table": "runs", "row": {"descriptor": {"a": "%s"}, "aggregate_strategy": "m"}}\n{"status": true}\n' \
    "$LONG" | "$BINARY" 2>/dev/null)
printf '%s' "$OUT" | grep -q '{"invalid": "toolong"}' \
    || fail "an oversized line must answer invalid toolong"
printf '%s' "$OUT" | grep -q '"invalid": 1}' \
    || fail "the oversized line must be counted once"

# --- 5. Writes without the libpq driver fail as EIO ---------------
# Only when the binary was built without the driver: with the
# driver the same lines belong to test_pg.sh (real persistence).
OUT=$(printf '%s\n' '{"status": true}' | "$BINARY" 2>/dev/null)
if printf '%s' "$OUT" | grep -q '"libpq": false'; then
    printf '%s\n' \
        '{"table": "runs", "row": {"descriptor": {}, "aggregate_strategy": "m"}}' \
        | "$BINARY" >/dev/null 2>/tmp/dbt_stderr.$$
    RC=$?
    grep -q 'EIO' /tmp/dbt_stderr.$$ \
        || fail "a write without the driver must report EIO"
    [ "$RC" = 1 ] || fail "a session with a transport failure must exit 1"
    rm -f /tmp/dbt_stderr.$$
fi

if [ "$FAIL" = 0 ]; then
    say "test_protocol: OK"
    exit 0
fi
exit 1
