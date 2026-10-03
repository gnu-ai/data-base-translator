# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

# The contract test runs the REAL engine on any POSIX system, with
# or without libpq, with or without a server: dbt_parse and the
# response builders are pure code (test_contract.c).  The CLI and
# protocol tests exercise the built verification binary
# (test_cli.sh, test_protocol.sh).  test_pg.sh runs the writes
# against a disposable PostgreSQL cluster and skips (77) when the
# tools are not available.

AM_CPPFLAGS = -I$(top_srcdir)/src
AM_CFLAGS = $(C23_CFLAGS) -Wall -Wextra

TESTS = test_contract test_cli.sh test_protocol.sh test_pg.sh
check_PROGRAMS = test_contract

test_contract_SOURCES = test_contract.c
test_contract_CPPFLAGS = $(AM_CPPFLAGS) \
    -DDBT_SCHEMA_SQL_PATH='"$(top_srcdir)/schema.sql"'
test_contract_LDADD = $(top_builddir)/src/libdbtcore.a $(LIBPQ_LIBS)

EXTRA_DIST = test_cli.sh test_protocol.sh test_pg.sh
