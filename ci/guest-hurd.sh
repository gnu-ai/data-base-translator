#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# CI guest script: build and test data-base-translator on real
# GNU/Hurd, driven by
# gnu-ai/mistral-vm-debian-hurd (hurd_vm.py).
#
#   python3 hurd_vm.py <image> ci/guest-hurd.sh
#
# Runs as root inside the guest; the exit code of this script
# becomes the exit code of the driver.
#
# The pushed tree is served by the CI host over plain HTTP on the
# QEMU gateway (10.0.2.2:8000, see .github/workflows/hurd.yml):
# the guest tests exactly the commit that was pushed, and the
# guest's TLS stack is never in the way.
#
# Steps: build, run the whole suite (disposable PostgreSQL cluster
# included: PostgreSQL 18 is available on Debian hurd-amd64), then
# a smoke test on the REAL mounted translator: settrans /db, a run
# row written with tee and read back with cat, the duplicate
# checksum answered as a status, every row read back from SQL.

set -e

# A serial console reports zero rows: psql (aligned format) and
# friends would page even a one-line output and wedge the session
# at a "(END)" prompt.  No pagers in a CI guest.
export PAGER=cat PSQL_PAGER=cat

echo "=== guest: $(uname -a)"

echo "=== installing the build dependencies"
apt-get update -qq

# Debian ports (hurd-amd64): the ssl-cert postinst passes an
# empty GID to groupadd, which breaks the whole PostgreSQL
# dependency chain.  Pre-creating the group sidesteps it.  And
# update-alternatives stumbles on a stale alternatives link
# ("cannot stat ...: Invalid argument"), which breaks the
# automake postinst and takes aclocal with it.  Pre-creating
# the group below sidesteps the ssl-cert half of it.
if ! grep -q '^ssl-cert:' /etc/group; then
    GID=$(awk -F: 'BEGIN{m=100} {if($3>m && $3<65534)m=$3} END{print m+1}' /etc/group)
    echo "ssl-cert:x:$GID:" >> /etc/group
fi
dpkg --configure -a >/dev/null 2>&1 || true

DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential autoconf automake pkg-config \
    libpq-dev postgresql postgresql-client ca-certificates wget

# A stale alternatives link breaks the automake postinst and
# takes aclocal with it — but removing the links when automake
# is ALREADY configured breaks aclocal just as well.  Repair
# only when the toolchain is actually missing, then verify it.
if ! command -v aclocal >/dev/null 2>&1; then
    rm -f /etc/alternatives/automake /etc/alternatives/aclocal 2>/dev/null || true
    dpkg --configure -a >/dev/null 2>&1 || true
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        --reinstall automake >/dev/null 2>&1 || true
fi
for tool in autoreconf aclocal automake autoconf make gcc pkg-config; do
    command -v "$tool" >/dev/null 2>&1 \
        || { echo "FAIL: $tool is missing from the toolchain"; exit 1; }
done
dpkg -s postgresql >/dev/null 2>&1 \
    || { echo "FAIL: the PostgreSQL server did not configure"; exit 1; }

echo "=== fetching the pushed tree from the CI host"
# The build and the test suite run as the unprivileged "builder"
# user: initdb and pg_ctl refuse to run as root, and a suite
# closer to a developer machine is a more honest CI.
useradd -m -s /bin/sh builder 2>/dev/null || true
rm -rf /home/builder/data-base-translator /home/builder/repo.tar.gz
cd /home/builder
wget -q -O repo.tar.gz http://10.0.2.2:8000/data-base-translator.tar.gz \
    || { echo "FAIL: the CI host is not serving the tree on :8000"; exit 1; }
tar xzf repo.tar.gz
chown -R builder:builder /home/builder/data-base-translator

echo "=== building (GNU/Hurd: the real trivfs face is linked)"
echo "=== make check (disposable PostgreSQL cluster included)"
su builder -c 'cd data-base-translator \
               && ./autogen.sh && ./configure && make \
               && if ! make check; then
                      echo "=== the failing test logs:";
                      for l in tests/*.log; do
                          if grep -q "FAIL" "$l" 2>/dev/null; then
                              echo "--- $l";
                              tail -30 "$l";
                          fi;
                      done;
                      exit 1;
                  fi'

echo "=== smoke test: the real translator on /db"
cd /home/builder/data-base-translator
# Debian runs PostgreSQL 18 as the "main" cluster.  Hurd has no
# reliable peer credentials on the local socket, so the cluster
# of this TEST machine answers "trust" on local connections, and
# root gets the role the translator will connect as.
pg_ctlcluster 18 main start || true
# Hurd has no reliable peer credentials on the local socket, and
# the config of a cluster left half-written by an unclean shutdown
# of an earlier CI run can even be dropped by the next fsck.  The
# cluster of this TEST machine therefore gets a minimal,
# unconditionally rewritten trust-on-localhost pg_hba.conf.
cat > /etc/postgresql/18/main/pg_hba.conf <<'HBAEOF'
# CI test cluster: every local connection is trusted.
local   all             all                                     trust
host    all             all             127.0.0.1/32            trust
host    all             all             ::1/128                 trust
HBAEOF
pg_ctlcluster 18 main restart || pg_ctlcluster 18 main start
echo "step: the local hba lines now read:"
grep -E "^local" /etc/postgresql/18/main/pg_hba.conf
echo "step: creating the role and the base"
psql -U postgres -tAc "create role root login superuser" 2>&1 || true
dropdb -U postgres --if-exists gnuai 2>&1 || true
createdb -U postgres -O root gnuai 2>&1 || true
echo "step: connecting as root (this is what the translator does):"
psql -d gnuai -tAc "select current_user"

rm -rf /db
touch /db
echo "step: mount point ready"

settrans -a /db ./src/db-translator --conninfo "dbname=gnuai"
echo "step: settrans rc=$?"
sleep 2

echo "=== status of the mounted translator"
cat /db

echo "=== one run row, written with plain POSIX"
echo '{"table": "runs", "row": {"descriptor": {"instances": 2}, "aggregate_strategy": "majority"}}' | tee /db
cat /db
cat /db

echo "=== a training row, twice: the duplicate is a status, never an error"
CS=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
echo "{\"table\": \"training_data\", \"row\": {\"source_url\": \"https://example.org/\", \"http_status\": 200, \"content\": \"hello\", \"checksum\": \"$CS\"}}" | tee /db
cat /db
echo "{\"table\": \"training_data\", \"row\": {\"source_url\": \"https://example.org/\", \"http_status\": 200, \"content\": \"hello\", \"checksum\": \"$CS\"}}" | tee /db
cat /db

echo "=== the trace is in PostgreSQL"
N=$(psql -d gnuai -tAc "select count(*) from runs")
[ "$N" = "1" ] || { echo "FAIL: runs must hold one row, got '$N'"; exit 1; }
V=$(psql -d gnuai -tAc "select aggregate_strategy from runs where id = 1")
[ "$V" = "majority" ] || { echo "FAIL: wrong strategy '$V'"; exit 1; }
N=$(psql -d gnuai -tAc "select count(*) from training_data")
[ "$N" = "1" ] || { echo "FAIL: the duplicate must not create a second row, got '$N'"; exit 1; }
V=$(psql -d gnuai -tAc "select content from training_data where checksum = '$CS'")
[ "$V" = "hello" ] || { echo "FAIL: wrong content '$V'"; exit 1; }
echo "runs=1, training_data=1 (the duplicate was refused), content ok"

echo "=== unmounting"
settrans -g /db
sleep 1

echo "=== all good"
