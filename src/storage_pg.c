/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * storage_pg.c — The libpq backend of /db.
 *
 * Design rules implemented here (PLAN.md sections 3 and 4):
 *
 *   - ONE connection, prepared ONCE per connection: the statement
 *     set is fixed by the frozen contract, never built by
 *     concatenation — every caller byte is a bound parameter.
 *   - The schema is applied idempotently at connect (the embedded
 *     copy of schema.sql), never destructively: CREATE TABLE IF
 *     NOT EXISTS, no DROP, no ALTER.
 *   - Expected constraints are statuses, not POSIX errors: a
 *     unique violation of checksum becomes DBT_ST_DUPLICATE with
 *     the existing id, a foreign key violation becomes
 *     DBT_ST_INVALID.  Only the transport layer (unreachable
 *     server, lost connection) fails, and the next attempt
 *     reconnects: the translator never dies on the database.
 *   - Zero allocation in the write path: the parameters are
 *     rendered into stack buffers bounded by the contract.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "storage_pg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

/* ---------------------------------------------------------------------
 *  State: the single connection of the MVP (PLAN.md section 4).
 * ------------------------------------------------------------------- */

static struct
  {
    PGconn *conn;
    bool    prepared;          /* statements ready on this connection */
    bool    schema_ok;        /* DDL applied on this connection */
    char    conninfo[512];
  } pg =
  {
    .conninfo = "dbname=gnuai",
  };

/* Statement names of the frozen set (see prepare_statements). */
#define STMT_INS_RUNS       "dbt_ins_runs"
#define STMT_INS_INSTANCES  "dbt_ins_run_instances"
#define STMT_INS_TRAINING   "dbt_ins_training_data"
#define STMT_SEL_CHECKSUM   "dbt_sel_checksum"

/* PostgreSQL type OIDs, so the server never has to infer: the
 * contract decides the types, not the values of the day. */
#define OID_JSONB 3802
#define OID_TEXT  25
#define OID_INT8  20
#define OID_INT4  23
#define OID_FLOAT4 700

/* ---------------------------------------------------------------------
 *  Diagnostics: the last transport message, kept for the status.
 * ------------------------------------------------------------------- */

static void
note_error (const char *msg)
{
  dbt_storage_set_error (msg);
}

static void
note_pg_error (void)
{
  note_error (pg.conn != NULL ? PQerrorMessage (pg.conn)
                              : "PostgreSQL unreachable");
}

/* ---------------------------------------------------------------------
 *  Connection: connect, apply the schema, prepare the statements.
 *  Returns true when the backend is ready to serve a write.
 * ------------------------------------------------------------------- */

static bool
prepare_statements (void)
{
  static const Oid ins_runs_types[3] = { OID_JSONB, OID_TEXT, OID_JSONB };
  static const Oid ins_ins_types[7] =
    { OID_INT8, OID_TEXT, OID_INT8, OID_JSONB, OID_JSONB, OID_FLOAT4,
      OID_TEXT };
  static const Oid ins_tr_types[4] =
    { OID_TEXT, OID_INT4, OID_TEXT, OID_TEXT };
  static const Oid sel_types[1] = { OID_TEXT };

  static const char ins_runs_sql[] =
    "INSERT INTO runs (descriptor, aggregate_strategy, final_output)"
    " VALUES ($1, $2, $3) RETURNING id";
  static const char ins_ins_sql[] =
    "INSERT INTO run_instances (run_id, topology, seed, input, output,"
    " score, status) VALUES ($1, $2, $3, $4, $5, $6, $7)"
    " RETURNING id";
  static const char ins_tr_sql[] =
    "INSERT INTO training_data (source_url, http_status, content,"
    " checksum) VALUES ($1, $2, $3, $4) RETURNING id";
  static const char sel_sql[] =
    "SELECT id FROM training_data WHERE checksum = $1";

  PGresult *res[5];
  bool ok = true;

  res[0] = PQprepare (pg.conn, STMT_INS_RUNS, ins_runs_sql, 3,
                      ins_runs_types);
  res[1] = PQprepare (pg.conn, STMT_INS_INSTANCES, ins_ins_sql, 7,
                      ins_ins_types);
  res[2] = PQprepare (pg.conn, STMT_INS_TRAINING, ins_tr_sql, 4,
                      ins_tr_types);
  res[3] = PQprepare (pg.conn, STMT_SEL_CHECKSUM, sel_sql, 1,
                      sel_types);

  for (int i = 0; i < 4; i++)
    {
      if (res[i] == NULL || PQresultStatus (res[i]) != PGRES_COMMAND_OK)
        ok = false;
      if (res[i] != NULL)
        PQclear (res[i]);
    }
  res[4] = NULL;
  (void) res[4];

  return ok;
}

static bool
connect_and_prepare (void)
{
  PGresult *res;
  ExecStatusType st;

  if (pg.conn != NULL)
    {
      PQfinish (pg.conn);
      pg.conn = NULL;
    }
  pg.prepared = false;
  pg.schema_ok = false;

  pg.conn = PQconnectdb (pg.conninfo);
  if (pg.conn == NULL || PQstatus (pg.conn) != CONNECTION_OK)
    {
      note_pg_error ();
      if (pg.conn != NULL)
        {
          PQfinish (pg.conn);
          pg.conn = NULL;
        }
      return false;
    }

  /* Idempotent schema: the embedded copy of schema.sql, CREATE
   * TABLE IF NOT EXISTS only — existing data is never touched. */
  res = PQexec (pg.conn, dbt_schema_ddl ());
  st = res != NULL ? PQresultStatus (res) : PGRES_FATAL_ERROR;
  if (res != NULL)
    PQclear (res);
  if (st != PGRES_COMMAND_OK)
    {
      note_pg_error ();
      PQfinish (pg.conn);
      pg.conn = NULL;
      return false;
    }
  pg.schema_ok = true;

  if (!prepare_statements ())
    {
      note_pg_error ();
      PQfinish (pg.conn);
      pg.conn = NULL;
      pg.schema_ok = false;
      return false;
    }
  pg.prepared = true;
  return true;
}

/* Ensure the backend can serve: reconnect after a lost connection
 * (PLAN.md section 4: the translator retries and stays readable). */
static bool
ensure_connected (void)
{
  if (pg.conn != NULL && PQstatus (pg.conn) == CONNECTION_OK
      && pg.prepared)
    return true;
  return connect_and_prepare ();
}

/* ---------------------------------------------------------------------
 *  Result interpretation: expected constraints are statuses.
 * ------------------------------------------------------------------- */

/* Classify a failed statement.  SQLSTATE is the portable truth:
 *   23505 unique_violation   -> DBT_ST_DUPLICATE (training_data)
 *   23503 foreign_key_violation, 22P02/22P03 invalid_text,
 *   22003 numeric_out_of_range, 23502 not_null_violation
 *                           -> DBT_ST_INVALID (caller's row)
 *   anything else            -> the transport/server layer.
 */
static enum dbt_storage_status
classify_error (PGresult *res)
{
  const char *sqlstate = res != NULL
                           ? PQresultErrorField (res, PG_DIAG_SQLSTATE)
                           : NULL;

  if (sqlstate == NULL)
    {
      note_pg_error ();
      return DBT_ST_TRANSPORT;
    }
  if (strcmp (sqlstate, "23505") == 0)
    return DBT_ST_DUPLICATE;
  if (strcmp (sqlstate, "23503") == 0 || strcmp (sqlstate, "23502") == 0
      || strcmp (sqlstate, "22P02") == 0
      || strcmp (sqlstate, "22P03") == 0
      || strcmp (sqlstate, "22003") == 0)
    return DBT_ST_INVALID;
  note_pg_error ();
  return DBT_ST_TRANSPORT;
}

/* Run a prepared statement that returns a single id column.
 * On success returns DBT_ST_OK and stores the id. */
static enum dbt_storage_status
exec_prepared_id (const char *stmt, const char *const *values,
                  const int *lengths, int nparams, long long *id)
{
  PGresult *res;
  ExecStatusType st;
  enum dbt_storage_status out;

  if (!ensure_connected ())
    return DBT_ST_TRANSPORT;

  res = PQexecPrepared (pg.conn, stmt, nparams, values, lengths, NULL, 0);
  st = res != NULL ? PQresultStatus (res) : PGRES_FATAL_ERROR;

  if (st == PGRES_TUPLES_OK && res != NULL && PQntuples (res) == 1)
    {
      *id = strtoll (PQgetvalue (res, 0, 0), NULL, 10);
      out = DBT_ST_OK;
    }
  else if (st == PGRES_FATAL_ERROR || st == PGRES_NONFATAL_ERROR)
    out = classify_error (res);
  else
    {
      note_pg_error ();
      out = DBT_ST_TRANSPORT;
    }

  if (res != NULL)
    PQclear (res);
  return out;
}

/* The existing id behind a UNIQUE checksum violation. */
static enum dbt_storage_status
lookup_checksum (const char *checksum, long long *id)
{
  const char *values[1] = { checksum };
  const int lengths[1] = { 0 };
  PGresult *res;
  enum dbt_storage_status out;

  if (!ensure_connected ())
    return DBT_ST_TRANSPORT;

  res = PQexecPrepared (pg.conn, STMT_SEL_CHECKSUM, 1, values, lengths,
                        NULL, 0);
  if (res != NULL && PQresultStatus (res) == PGRES_TUPLES_OK)
    {
      if (PQntuples (res) == 1)
        {
          *id = strtoll (PQgetvalue (res, 0, 0), NULL, 10);
          out = DBT_ST_OK;
        }
      else
        {
          /* The constraint fired but the row vanished: the caller
           * is told what is certain — this is a duplicate. */
          *id = 0;
          out = DBT_ST_DUPLICATE;
        }
    }
  else
    {
      note_pg_error ();
      out = DBT_ST_TRANSPORT;
    }
  if (res != NULL)
    PQclear (res);
  return out;
}

/* ---------------------------------------------------------------------
 *  Public backend functions
 * ------------------------------------------------------------------- */

void
pg_init (const char *conninfo)
{
  snprintf (pg.conninfo, sizeof pg.conninfo, "%s",
            conninfo != NULL ? conninfo : "dbname=gnuai");
  /* Connect eagerly: at mount the schema must exist and the
   * statements must be ready, before the first write arrives. */
  connect_and_prepare ();
}

void
pg_shutdown (void)
{
  if (pg.conn != NULL)
    {
      PQfinish (pg.conn);
      pg.conn = NULL;
    }
  pg.prepared = false;
  pg.schema_ok = false;
}

enum dbt_storage_status
pg_insert_runs (const struct dbt_row_runs *row, long long *id)
{
  const char *values[3] =
    {
      row->descriptor,
      row->aggregate_strategy,
      row->final_output,        /* NULL: SQL NULL */
    };
  const int lengths[3] = { 0, 0, 0 };

  return exec_prepared_id (STMT_INS_RUNS, values, lengths, 3, id);
}

enum dbt_storage_status
pg_insert_run_instances (const struct dbt_row_instances *row,
                         long long *id)
{
  char run_id_buf[32];
  char seed_buf[32];
  char score_buf[48];
  const char *values[7];
  const int lengths[7] = { 0, 0, 0, 0, 0, 0, 0 };

  snprintf (run_id_buf, sizeof run_id_buf, "%lld", row->run_id);
  values[0] = run_id_buf;
  values[1] = row->topology;
  values[2] = NULL;
  if (row->has_seed)
    {
      snprintf (seed_buf, sizeof seed_buf, "%lld", row->seed);
      values[2] = seed_buf;
    }
  values[3] = row->input;
  values[4] = row->output;
  values[5] = NULL;
  if (row->has_score)
    {
      snprintf (score_buf, sizeof score_buf, "%.9g", row->score);
      values[5] = score_buf;
    }
  values[6] = row->status;

  return exec_prepared_id (STMT_INS_INSTANCES, values, lengths, 7, id);
}

enum dbt_storage_status
pg_insert_training_data (const struct dbt_row_training *row,
                         long long *id)
{
  char http_status_buf[16];
  const char *values[4];
  const int lengths[4] = { 0, 0, 0, 0 };
  enum dbt_storage_status st;

  values[0] = row->source_url;
  values[1] = NULL;
  if (row->has_http_status)
    {
      snprintf (http_status_buf, sizeof http_status_buf, "%ld",
                row->http_status);
      values[1] = http_status_buf;
    }
  values[2] = row->content;
  values[3] = row->checksum;

  st = exec_prepared_id (STMT_INS_TRAINING, values, lengths, 4, id);
  if (st == DBT_ST_DUPLICATE)
    {
      /* The constraint is a fact, not an error: give the caller
       * the id that already carries this checksum (PLAN.md 4). */
      long long existing = 0;
      enum dbt_storage_status lk = lookup_checksum (row->checksum,
                                                    &existing);

      *id = existing;
      if (lk == DBT_ST_TRANSPORT)
        return DBT_ST_TRANSPORT;
    }
  return st;
}

bool
pg_connected (void)
{
  return pg.conn != NULL && PQstatus (pg.conn) == CONNECTION_OK;
}

bool
pg_schema_ok (void)
{
  return pg.schema_ok;
}

int
pg_server_version (void)
{
  return pg.conn != NULL ? PQserverVersion (pg.conn) : 0;
}
