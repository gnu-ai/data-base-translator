/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * ops.h — The one line engine of /db.
 *
 * This is the whole translator behind its two faces: the trivfs
 * server on GNU/Hurd and the verification REPL on the other POSIX
 * systems.  One complete instruction line goes in, one response
 * line comes out; the response to the last completed instruction
 * is the data served by reads, with one cursor per reader.
 */

#ifndef DBT_OPS_H
#define DBT_OPS_H

#include <stddef.h>

/* One shot initialization (at mount, or at REPL startup); the
 * initial response slot is the status line, so `cat /db` right
 * after mount answers the state of the base. */
void ops_init (const char *conninfo);
void ops_shutdown (void);

/* Process one complete instruction line (WITHOUT its newline; a
 * trailing CR is tolerated and dropped).
 *
 * Returns 0, or -1 with errno == EIO when the transport layer
 * failed: only the transport fails as a POSIX error (PLAN.md
 * section 4) — every contract or constraint issue is a response
 * line.  The response slot is left unchanged on transport
 * failures. */
int ops_process_line (const char *line, size_t len);

/* The response to the last completed instruction (or the
 * mount-time status): served by reads, NUL terminated, with its
 * length.  Each reader keeps its own cursor into it. */
const char *ops_response (void);
size_t ops_response_len (void);

#endif /* DBT_OPS_H */
