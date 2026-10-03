/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * ops.c — The one line engine of /db (SPEC.md sections 3 to 5).
 *
 *     one instruction line  -->  one response line
 *
 * The engine owns the response slot: the answer to the last
 * completed instruction, or the status at mount.  Reads serve
 * that slot with one cursor per reader — no shared mutable state
 * beyond the slot itself, which the single threaded servers
 * (trivfs loop, REPL) update between complete instructions.
 *
 * All buffers are static and pre-allocated: the write path never
 * allocates, in the discipline of the stack.
 */

#include "ops.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "contract.h"
#include "storage.h"

/* The response slot and the parse arena, allocated once. */
static char  ops_slot[DBT_MAX_RESPONSE];
static size_t ops_slot_len;
static char  ops_scratch[DBT_SCRATCH];

/* Build the status line into the slot: the state of the base is
 * data, always readable, even when the server is down. */
static void
build_status (void)
{
  const struct dbt_counters *c = dbt_storage_counters ();
  size_t n = 0;
  int w;

  w = snprintf (ops_slot, sizeof ops_slot,
                "{\"connected\": %s, \"server_version\": %d, "
                "\"schema\": %s, \"libpq\": %s, \"writes\": {",
                dbt_storage_connected () ? "true" : "false",
                dbt_storage_server_version (),
                dbt_storage_schema_ok () ? "true" : "false",
                dbt_storage_have_libpq () ? "true" : "false");
  if (w < 0 || (size_t) w >= sizeof ops_slot)
    {
      ops_slot_len = 0;
      return;
    }
  n = (size_t) w;

  n = dbt_json_number (ops_slot, n, sizeof ops_slot, "runs",
                       c->runs);
  if (n + 1 < sizeof ops_slot)
    ops_slot[n++] = ',';
  n = dbt_json_number (ops_slot, n, sizeof ops_slot, "run_instances",
                       c->run_instances);
  if (n + 1 < sizeof ops_slot)
    ops_slot[n++] = ',';
  n = dbt_json_number (ops_slot, n, sizeof ops_slot, "training_data",
                       c->training_data);
  if (n + 1 < sizeof ops_slot)
    ops_slot[n++] = ',';
  n = dbt_json_number (ops_slot, n, sizeof ops_slot, "duplicates",
                       c->duplicates);
  if (n + 1 < sizeof ops_slot)
    ops_slot[n++] = ',';
  n = dbt_json_number (ops_slot, n, sizeof ops_slot, "invalid",
                       c->invalid);
  w = snprintf (ops_slot + n, sizeof ops_slot - n, "}}");
  if (w < 0 || (size_t) w >= sizeof ops_slot - n)
    {
      ops_slot_len = 0;
      return;
    }
  ops_slot_len = n + (size_t) w;
}

/* Install a response in the slot (built by the contract layer). */
static void
set_response (size_t len)
{
  ops_slot_len = len;
}

void
ops_init (const char *conninfo)
{
  dbt_storage_init (conninfo);
  build_status ();
}

void
ops_shutdown (void)
{
  dbt_storage_shutdown ();
}

int
ops_process_line (const char *line, size_t len)
{
  struct dbt_request req;
  long long id = 0;
  enum dbt_storage_status st;

  /* Tolerate the CRLF of redirected pipes, then apply the limit
   * of the contract (large paginated contents arrive in phase 3). */
  while (len > 0 && line[len - 1] == '\r')
    len--;
  if (len >= DBT_MAX_LINE)
    {
      dbt_storage_note_invalid ();
      set_response (dbt_response_invalid (ops_slot, sizeof ops_slot,
                                           DBT_INVALID_TOOLONG, NULL));
      return 0;
    }

  dbt_parse (line, len, ops_scratch, sizeof ops_scratch, &req);

  switch (req.kind)
    {
    case DBT_REQ_STATUS:
      build_status ();
      return 0;

    case DBT_REQ_INVALID:
      dbt_storage_note_invalid ();
      set_response (dbt_response_invalid (ops_slot, sizeof ops_slot,
                                           req.invalid,
                                           req.invalid_detail));
      return 0;

    case DBT_REQ_INSERT:
      break;
    }

  switch (req.table)
    {
    case DBT_TABLE_RUNS:
      st = dbt_insert_runs (&req.row.runs, &id);
      break;
    case DBT_TABLE_RUN_INSTANCES:
      st = dbt_insert_run_instances (&req.row.instances, &id);
      break;
    case DBT_TABLE_TRAINING_DATA:
      st = dbt_insert_training_data (&req.row.training, &id);
      break;
    default:
      st = DBT_ST_INVALID;               /* unreachable by the parser */
      break;
    }

  switch (st)
    {
    case DBT_ST_OK:
      set_response (dbt_response_ok (ops_slot, sizeof ops_slot, id));
      return 0;
    case DBT_ST_DUPLICATE:
      set_response (dbt_response_duplicate (ops_slot,
                                             sizeof ops_slot, id));
      return 0;
    case DBT_ST_EMPTY:
      set_response (dbt_response_empty (ops_slot, sizeof ops_slot));
      return 0;
    case DBT_ST_INVALID:
      /* A constraint the base refused (foreign key, range): the
       * offending column is the useful part for the caller. */
      dbt_storage_note_invalid ();
      set_response (dbt_response_invalid (
                      ops_slot, sizeof ops_slot, DBT_INVALID_FIELD,
                      req.table == DBT_TABLE_RUN_INSTANCES
                        ? "run_id" : "row"));
      return 0;
    case DBT_ST_TRANSPORT:
    default:
      errno = EIO;
      return -1;
    }
}

const char *
ops_response (void)
{
  return ops_slot;
}

size_t
ops_response_len (void)
{
  return ops_slot_len;
}
