/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * storage.h — Persistence backend interface of /db (PLAN.md 3.3).
 *
 * The callers of this layer (the ops engine, the trivfs hooks, the
 * verification REPL) never see libpq: they see statuses.  The
 * expected constraints of the database are data (DBT_ST_DUPLICATE,
 * DBT_ST_INVALID), only the transport layer fails
 * (DBT_ST_TRANSPORT, surfaced as EIO by the callers).
 *
 * One connection, one fixed set of prepared statements per table
 * and per operation, prepared once after each (re)connection — the
 * single connection is the documented MVP choice of PLAN.md
 * section 4; a pool would only come measured.
 */

#ifndef DBT_STORAGE_H
#define DBT_STORAGE_H

#include <stdbool.h>

#include "contract.h"

enum dbt_storage_status
  {
    DBT_ST_OK = 0,     /* row written, id attributed by the base */
    DBT_ST_DUPLICATE,  /* UNIQUE checksum: the EXISTING id is returned */
    DBT_ST_INVALID,    /* the base refused the row (expected constraint) */
    DBT_ST_EMPTY,      /* query without result (phase 2) */
    DBT_ST_TRANSPORT,  /* server unreachable or connection lost */
  };

struct dbt_counters
  {
    long runs;
    long run_instances;
    long training_data;
    long duplicates;
    long invalid;
  };

/* One shot initialization (at mount, or at REPL startup). */
void dbt_storage_init (const char *conninfo);
void dbt_storage_shutdown (void);

/* Writes.  On DBT_ST_OK and DBT_ST_DUPLICATE, *ID receives the
 * attributed (resp. existing) identifier. */
enum dbt_storage_status dbt_insert_runs (const struct dbt_row_runs *row,
                                         long long *id);
enum dbt_storage_status dbt_insert_run_instances (
                                const struct dbt_row_instances *row,
                                long long *id);
enum dbt_storage_status dbt_insert_training_data (
                                const struct dbt_row_training *row,
                                long long *id);

/* Status report of the /db/status kind. */
bool dbt_storage_connected (void);
int  dbt_storage_server_version (void);    /* 0: unknown */
bool dbt_storage_schema_ok (void);         /* schema applied at connect */
bool dbt_storage_have_libpq (void);        /* driver compiled in */
const char *dbt_storage_last_error (void); /* last transport diagnostic */

/* Counters since mount, and the "invalid" increment used by the
 * ops engine for contract violations. */
const struct dbt_counters *dbt_storage_counters (void);
void dbt_storage_note_invalid (void);

/* Used by the libpq backend to record a transport diagnostic. */
void dbt_storage_set_error (const char *msg);

/* The embedded copy of schema.sql — the DDL executed at connect,
 * idempotently.  The test suite checks that it matches the
 * schema.sql file byte for byte: the two can never drift apart. */
const char *dbt_schema_ddl (void);

#endif /* DBT_STORAGE_H */
