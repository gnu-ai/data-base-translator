#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# Integration test against a DISPOSABLE PostgreSQL cluster: the
# suite creates and destroys its own instance (PLAN.md section 7),
# no test ever writes to a real base.  Skips (77) when the binary
# has no libpq driver or when the PostgreSQL client tools are not
# installed on this machine.

set -u

BINARY="${1:-../src/db-translator}"
FAIL=0
WORK=""

say() { printf '%s\n' "$*"; }
fail() { say "test_pg: FAIL: $*"; FAIL=1; }

cleanup()
{
    if [ -n "$WORK" ] && [ -d "$WORK/cluster" ]; then
        pg_ctl -D "$WORK/cluster" stop -m immediate >/dev/null 2>&1
    fi
    [ -n "$WORK" ] && rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

if [ ! -x "$BINARY" ]; then
    say "test_pg: SKIP: $BINARY not built"
    exit 77
fi

# The driver must be built in: without it the writes cannot reach
# any server and the whole test is meaningless.
OUT=$(printf '%s\n' '{"status": true}' | "$BINARY" 2>/dev/null)
printf '%s' "$OUT" | grep -q '"libpq": true' || {
    say "test_pg: SKIP: binary built without the libpq driver"
    exit 77
}

# The disposable cluster needs the PostgreSQL client tools.  Debian
# keeps them in a versioned directory outside PATH; other systems
# have them in /usr/bin or /usr/local/pgsql/bin.
if ! command -v initdb >/dev/null 2>&1; then
    for d in /usr/lib/postgresql/*/bin /usr/local/pgsql/bin; do
        if [ -x "$d/initdb" ]; then
            PATH="$d:$PATH"
            export PATH
            break
        fi
    done
fi
for tool in initdb pg_ctl createdb psql; do
    command -v "$tool" >/dev/null 2>&1 || {
        say "test_pg: SKIP: $tool not available"
        exit 77
    }
done

WORK=$(mktemp -d)
PORT=5499
# The socket lives in the work directory: no privilege on
# /var/run/postgresql, and no collision with any system cluster.
mkdir -p "$WORK/sock"
CONNINFO="host=127.0.0.1 port=$PORT dbname=gnuai user=gnuai"

initdb -D "$WORK/cluster" -U gnuai --auth=trust >/dev/null 2>&1 \
    || { say "test_pg: FAIL: initdb"; exit 1; }
pg_ctl -D "$WORK/cluster" -l "$WORK/postgres.log" \
    -o "-p $PORT -c listen_addresses=127.0.0.1 -c fsync=off -c unix_socket_directories=$WORK/sock" \
    -w start >/dev/null 2>&1 \
    || { say "test_pg: FAIL: starting the cluster"; exit 1; }
createdb -h 127.0.0.1 -p $PORT -U gnuai gnuai >/dev/null 2>&1 \
    || { fail "createdb"; }

# --- 1. A complete orchestrated run, written line by line ----------
# The acceptance scenario of PLAN.md phase 1: one run, two
# instances, one fetched training row.
CS1=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
CS2=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb

IN="{\"table\": \"runs\", \"row\": {\"descriptor\": {\"instances\": 2}, \"aggregate_strategy\": \"majority\"}}
{\"table\": \"run_instances\", \"row\": {\"run_id\": 1, \"topology\": \"10,20,5\", \"input\": [0.5, 0.3], \"status\": \"ok\"}}
{\"table\": \"run_instances\", \"row\": {\"run_id\": 1, \"topology\": \"10,30,5\", \"seed\": 7, \"input\": [0.5, 0.3], \"output\": [0.9], \"score\": 0.5, \"status\": \"ok\"}}
{\"table\": \"training_data\", \"row\": {\"source_url\": \"https://example.org/a\", \"http_status\": 200, \"content\": \"hello\", \"checksum\": \"$CS1\"}}
{\"status\": true}"

OUT=$(printf '%s\n' "$IN" | "$BINARY" --conninfo "$CONNINFO" 2>/dev/null)
[ "$?" = 0 ] || fail "the session must exit 0 against a live base"

printf '%s\n' "$OUT" | sed -n '1p' | grep -q '{"ok": true, "id": 1}' \
    || fail "the first run must be attributed id 1"
printf '%s\n' "$OUT" | sed -n '2p' | grep -q '{"ok": true, "id": 1}' \
    || fail "the first instance must be attributed id 1"
printf '%s\n' "$OUT" | sed -n '3p' | grep -q '{"ok": true, "id": 2}' \
    || fail "the second instance must be attributed id 2"
printf '%s\n' "$OUT" | sed -n '5p' | grep -q '"connected": true' \
    || fail "the status must report a live connection"

# The schema was created idempotently, the rows are in the base.
Q="psql -h 127.0.0.1 -p $PORT -U gnuai -d gnuai -tAc"
N=$($Q "select count(*) from runs" 2>/dev/null)
[ "$N" = "1" ] || fail "runs must hold one row (got '$N')"
N=$($Q "select count(*) from run_instances" 2>/dev/null)
[ "$N" = "2" ] || fail "run_instances must hold two rows (got '$N')"
N=$($Q "select content from training_data where http_status = 200" 2>/dev/null)
[ "$N" = "hello" ] || fail "training_data must hold the fetched content"

# --- 2. A duplicate checksum is a status, not an error -------------
OUT=$(printf '%s\n' \
    "{\"table\": \"training_data\", \"row\": {\"source_url\": \"u2\", \"content\": \"hello again\", \"checksum\": \"$CS1\"}}" \
    | "$BINARY" --conninfo "$CONNINFO" 2>/dev/null)
printf '%s' "$OUT" | grep -q '{"duplicate": 1}' \
    || fail "a known checksum must answer duplicate with the existing id"

# --- 3. A foreign key violation is a readable status ---------------
OUT=$(printf '%s\n' \
    '{"table": "run_instances", "row": {"run_id": 999, "topology": "1", "input": [], "status": "ok"}}' \
    | "$BINARY" --conninfo "$CONNINFO" 2>/dev/null)
printf '%s' "$OUT" | grep -q '{"invalid": "run_id"}' \
    || fail "an unknown run_id must answer invalid run_id"

# --- 4. A different checksum writes a second training row -----------
# The duplicate attempt above consumed sequence id 2 (BIGSERIAL
# increments even on a refused insert): the second distinct row is
# attributed id 3.  The contract promises an attributed id, the
# sequence is the base's business.
OUT=$(printf '%s\n' \
    "{\"table\": \"training_data\", \"row\": {\"source_url\": \"u3\", \"content\": \"world\", \"checksum\": \"$CS2\"}}" \
    | "$BINARY" --conninfo "$CONNINFO" 2>/dev/null)
printf '%s' "$OUT" | grep -q '{"ok": true, "id": 3}' \
    || fail "the second training row must be attributed the next id"

if [ "$FAIL" = 0 ]; then
    say "test_pg: OK"
    exit 0
fi
exit 1
