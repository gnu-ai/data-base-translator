# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

bin_PROGRAMS = db-translator

# --- Phase 0: the compilable skeleton --------------------------------
#
# The translator /db (idempotent schema, JSON lines in, streamed
# lines out — PLAN.md section 3) arrives with phase 1 built on
# trivfs and libpq.  The skeleton answers the GNU base commands so
# that the binary is inspectable on any POSIX system, Hurd or not.

db_translator_SOURCES = main.c

AM_CPPFLAGS = -I$(top_srcdir)
AM_CFLAGS = $(C23_CFLAGS) -Wall -Wextra

# libpq is detected by configure; the phase 0 skeleton does not
# link it, the flags are wired so phase 1 only adds its sources.
if HAVE_LIBPQ
db_translator_CPPFLAGS = $(AM_CPPFLAGS) $(LIBPQ_CFLAGS)
db_translator_LDADD = $(LIBPQ_LIBS)
else
db_translator_LDADD =
endif
