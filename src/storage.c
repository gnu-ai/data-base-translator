/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * storage.c — Persistence backend of /db: state, counters, and the
 * fallback used when the binary was built without the libpq driver
 * (PLAN.md section 7: the stack stays compilable everywhere, and
 * the contract stays testable without a server).
 *
 * The public functions update the counters and delegate to the
 * libpq backend of storage_pg.c; the status of the base is a fact
 * the caller reads, never a reason to die (PLAN.md section 4).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "storage.h"

#include <stdio.h>
#include <string.h>

#ifdef HAVE_LIBPQ
#include "storage_pg.h"     /* the libpq backend, compiled only with the driver */
#endif

/* ---------------------------------------------------------------------
 *  State — one connection, one set of prepared statements (MVP).
 * ------------------------------------------------------------------- */

static struct
  {
    char                 conninfo[512];
    bool                 have_libpq;
    bool                 connected;
    bool                 schema_ok;
    int                  server_version;
    struct dbt_counters  counters;
    char                 last_error[256];
  } dbt_state =
  {
    .conninfo = "dbname=gnuai",
    .have_libpq = false,
  };

/* ---------------------------------------------------------------------
 *  The embedded DDL — generated from schema.sql, never edited by
 *  hand: test_contract.c fails `make check` if the two diverge by
 *  a single byte.  This is the text executed at every connection,
 *  so the schema is created idempotently at mount.
 * ------------------------------------------------------------------- */

const char *
dbt_schema_ddl (void)
{
  return
    "-- SPDX-License-Identifier: GPL-3.0-or-later\n"
    "-- SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>\n"
    "--\n"
    "-- Frozen schema of the GNU AI persistence layer (PLAN.md section 6).\n"
    "-- Created idempotently by data-base-translator at mount: every table\n"
    "-- is CREATE TABLE IF NOT EXISTS, nothing is ever dropped or altered.\n"
    "--\n"
    "-- This file is the single source of truth shared with\n"
    "-- orchestrator-translator (orchestration tables) and\n"
    "-- inference-translator (SSH key registry).  The same text is embedded\n"
    "-- in src/storage.c; the test suite checks that both copies match\n"
    "-- exactly, so a unilateral edit on either side fails `make check`.\n"
    "\n"
    "-- Training data fetched from the network\n"
    "CREATE TABLE IF NOT EXISTS training_data (\n"
    "    id          BIGSERIAL PRIMARY KEY,\n"
    "    source_url  TEXT NOT NULL,\n"
    "    fetched_at  TIMESTAMPTZ NOT NULL DEFAULT now(),\n"
    "    http_status INT,\n"
    "    content     TEXT NOT NULL,\n"
    "    checksum    CHAR(64) NOT NULL UNIQUE      -- SHA-256, anti-duplicate\n"
    ");\n"
    "\n"
    "-- Each orchestrated run\n"
    "CREATE TABLE IF NOT EXISTS runs (\n"
    "    id          BIGSERIAL PRIMARY KEY,\n"
    "    started_at  TIMESTAMPTZ NOT NULL DEFAULT now(),\n"
    "    descriptor  JSONB NOT NULL,               -- task descriptor\n"
    "    final_output JSONB,\n"
    "    aggregate_strategy TEXT NOT NULL\n"
    ");\n"
    "\n"
    "-- One row per neuron-translator instance in a run\n"
    "CREATE TABLE IF NOT EXISTS run_instances (\n"
    "    id          BIGSERIAL PRIMARY KEY,\n"
    "    run_id      BIGINT NOT NULL REFERENCES runs(id),\n"
    "    topology    TEXT NOT NULL,                -- e.g. \"10,20,5\"\n"
    "    seed        BIGINT,\n"
    "    input       JSONB NOT NULL,\n"
    "    output      JSONB,\n"
    "    score       REAL,\n"
    "    status      TEXT NOT NULL                 -- ok | failed | timeout\n"
    ");\n"
    "\n"
    "-- Failure history (supervisor)\n"
    "CREATE TABLE IF NOT EXISTS incidents (\n"
    "    id          BIGSERIAL PRIMARY KEY,\n"
    "    run_id      BIGINT REFERENCES runs(id),\n"
    "    instance_id BIGINT REFERENCES run_instances(id),\n"
    "    kind        TEXT NOT NULL,                -- crash | timeout | restart\n"
    "    detected_at TIMESTAMPTZ NOT NULL DEFAULT now()\n"
    ");\n"
    "\n"
    "-- Users allowed in the remote mode (inference-translator)\n"
    "CREATE TABLE IF NOT EXISTS users (\n"
    "    id          BIGSERIAL PRIMARY KEY,\n"
    "    name        TEXT NOT NULL UNIQUE,\n"
    "    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()\n"
    ");\n"
    "\n"
    "-- Nominative public SSH keys (inference-translator): a public key is\n"
    "-- not a secret, it lives in clear; the SHA-256 fingerprint carries\n"
    "-- uniqueness\n"
    "CREATE TABLE IF NOT EXISTS access_keys (\n"
    "    id          BIGSERIAL PRIMARY KEY,\n"
    "    user_id     BIGINT NOT NULL REFERENCES users(id),\n"
    "    key_pub     TEXT NOT NULL,                -- public SSH key\n"
    "    fingerprint CHAR(64) NOT NULL UNIQUE,    -- SHA-256 of the key\n"
    "    label       TEXT,                         -- e.g. \"Claire's laptop\"\n"
    "    issued_at   TIMESTAMPTZ NOT NULL DEFAULT now(),\n"
    "    revoked_at  TIMESTAMPTZ                   -- NULL = active key\n"
    ");\n"
    "\n"
    "-- Journal of refused SSH authentication attempts (inference)\n"
    "CREATE TABLE IF NOT EXISTS auth_failures (\n"
    "    id          BIGSERIAL PRIMARY KEY,\n"
    "    fingerprint CHAR(64),                     -- NULL if invalid key\n"
    "    origin      TEXT,                         -- origin address\n"
    "    refused_at  TIMESTAMPTZ NOT NULL DEFAULT now()\n"
    ");\n"
;
}

/* ---------------------------------------------------------------------
 *  Init and shutdown
 * ------------------------------------------------------------------- */

void
dbt_storage_init (const char *conninfo)
{
  memset (&dbt_state.counters, 0, sizeof dbt_state.counters);
  dbt_state.connected = false;
  dbt_state.schema_ok = false;
  dbt_state.last_error[0] = '\0';
  if (conninfo != NULL && conninfo[0] != '\0')
    snprintf (dbt_state.conninfo, sizeof dbt_state.conninfo, "%s",
              conninfo);
#ifdef HAVE_LIBPQ
  dbt_state.have_libpq = true;
  pg_init (dbt_state.conninfo);
#else
  dbt_state.have_libpq = false;
  snprintf (dbt_state.last_error, sizeof dbt_state.last_error,
            "libpq driver not built in");
#endif
}

void
dbt_storage_shutdown (void)
{
#ifdef HAVE_LIBPQ
  pg_shutdown ();
#endif
  dbt_state.connected = false;
  dbt_state.schema_ok = false;
}

/* ---------------------------------------------------------------------
 *  Fallback backend (no libpq in this build): the status stays
 *  readable and honest, every write fails as a transport failure.
 * ------------------------------------------------------------------- */

#ifndef HAVE_LIBPQ

static enum dbt_storage_status
unavailable (void)
{
  snprintf (dbt_state.last_error, sizeof dbt_state.last_error,
            "libpq driver not built in");
  return DBT_ST_TRANSPORT;
}

#endif /* !HAVE_LIBPQ */

/* ---------------------------------------------------------------------
 *  Writes: delegate, then maintain the counters of the status line.
 * ------------------------------------------------------------------- */

enum dbt_storage_status
dbt_insert_runs (const struct dbt_row_runs *row, long long *id)
{
  enum dbt_storage_status st;

#ifdef HAVE_LIBPQ
  st = pg_insert_runs (row, id);
#else
  (void) row;
  (void) id;
  st = unavailable ();
#endif
  if (st == DBT_ST_OK)
    dbt_state.counters.runs++;
  else if (st == DBT_ST_DUPLICATE)
    dbt_state.counters.duplicates++;
  else if (st == DBT_ST_INVALID)
    dbt_state.counters.invalid++;
  return st;
}

enum dbt_storage_status
dbt_insert_run_instances (const struct dbt_row_instances *row,
                          long long *id)
{
  enum dbt_storage_status st;

#ifdef HAVE_LIBPQ
  st = pg_insert_run_instances (row, id);
#else
  (void) row;
  (void) id;
  st = unavailable ();
#endif
  if (st == DBT_ST_OK)
    dbt_state.counters.run_instances++;
  else if (st == DBT_ST_DUPLICATE)
    dbt_state.counters.duplicates++;
  else if (st == DBT_ST_INVALID)
    dbt_state.counters.invalid++;
  return st;
}

enum dbt_storage_status
dbt_insert_training_data (const struct dbt_row_training *row,
                          long long *id)
{
  enum dbt_storage_status st;

#ifdef HAVE_LIBPQ
  st = pg_insert_training_data (row, id);
#else
  (void) row;
  (void) id;
  st = unavailable ();
#endif
  if (st == DBT_ST_OK)
    dbt_state.counters.training_data++;
  else if (st == DBT_ST_DUPLICATE)
    dbt_state.counters.duplicates++;
  else if (st == DBT_ST_INVALID)
    dbt_state.counters.invalid++;
  return st;
}

/* ---------------------------------------------------------------------
 *  Status report
 * ------------------------------------------------------------------- */

bool
dbt_storage_connected (void)
{
#ifdef HAVE_LIBPQ
  return pg_connected ();
#else
  return false;
#endif
}

int
dbt_storage_server_version (void)
{
#ifdef HAVE_LIBPQ
  return pg_server_version ();
#else
  return 0;
#endif
}

bool
dbt_storage_schema_ok (void)
{
#ifdef HAVE_LIBPQ
  return pg_schema_ok ();
#else
  return false;
#endif
}

bool
dbt_storage_have_libpq (void)
{
  return dbt_state.have_libpq;
}

const char *
dbt_storage_last_error (void)
{
  return dbt_state.last_error[0] != '\0' ? dbt_state.last_error
                                         : "transport failure";
}

const struct dbt_counters *
dbt_storage_counters (void)
{
  return &dbt_state.counters;
}

/* Called by the libpq backend: keep the last transport diagnostic
 * for the status line and the EIO report of the callers. */
void
dbt_storage_set_error (const char *msg)
{
  if (msg == NULL)
    msg = "transport failure";
  snprintf (dbt_state.last_error, sizeof dbt_state.last_error, "%s",
            msg);
  /* Trim the trailing newline PQerrorMessage leaves behind. */
  {
    size_t n = strlen (dbt_state.last_error);

    while (n > 0 && dbt_state.last_error[n - 1] == '\n')
      dbt_state.last_error[--n] = '\0';
  }
}

void
dbt_storage_note_invalid (void)
{
  dbt_state.counters.invalid++;
}
