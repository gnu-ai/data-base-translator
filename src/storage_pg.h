/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * storage_pg.h — Internal libpq backend API.
 *
 * Compiled and linked only when configure found libpq (HAVE_LIBPQ).
 * The public interface the rest of the translator uses is
 * storage.h; these functions are its private implementation.
 *
 * The backend owns the single connection and the fixed set of
 * prepared statements (one per table and per operation of the
 * contract, PLAN.md section 4).  After any reconnection the
 * statements are prepared again: a prepared statement belongs to
 * its connection.
 */

#ifndef DBT_STORAGE_PG_H
#define DBT_STORAGE_PG_H

#include <stdbool.h>

#include "storage.h"

void pg_init (const char *conninfo);
void pg_shutdown (void);

enum dbt_storage_status pg_insert_runs (const struct dbt_row_runs *row,
                                        long long *id);
enum dbt_storage_status pg_insert_run_instances (
                               const struct dbt_row_instances *row,
                                       long long *id);
enum dbt_storage_status pg_insert_training_data (
                               const struct dbt_row_training *row,
                                       long long *id);

bool pg_connected (void);
bool pg_schema_ok (void);
int  pg_server_version (void);

#endif /* DBT_STORAGE_PG_H */
