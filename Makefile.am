# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

SUBDIRS = src tests

# schema.sql is the frozen schema (PLAN.md section 6): the embedded
# copy of src/storage.c and this file must never drift, and
# test_contract checks the match.  Distributing it keeps the test
# meaningful from the released tarball.
EXTRA_DIST = schema.sql
