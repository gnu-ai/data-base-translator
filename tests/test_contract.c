/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_contract.c — Unit tests of the frozen /db contract.
 *
 * Home made harness: CHECK macros and counters, C23/POSIX only,
 * zero external framework — the convention of the stack
 * (PLAN.md section 7, like tests/test_neuron.c and the httpfs
 * suite).  The tests derive from the CONTRACT (SPEC.md), never
 * from the implementation: they feed instruction lines and
 * assert the parsed requests and the response lines.
 *
 * No server, no libpq call: everything here is pure contract
 * logic, runnable on any POSIX system.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "contract.h"
#include "storage.h"

/* ---------------------------------------------------------------------
 *  Minimal home made harness
 * ------------------------------------------------------------------- */

static int checks_run = 0;
static int checks_failed = 0;

#define CHECK(cond)                                                     \
  do                                                                    \
    {                                                                   \
      checks_run++;                                                     \
      if (!(cond))                                                      \
        {                                                               \
          checks_failed++;                                              \
          printf ("test_contract: FAIL: %s (%s:%d)\n", #cond,          \
                  __FILE__, __LINE__);                                  \
        }                                                               \
    }                                                                   \
  while (0)

#define CHECK_STR(a, b)                                                 \
  do                                                                    \
    {                                                                   \
      checks_run++;                                                     \
      if ((a) == NULL || strcmp ((a), (b)) != 0)                        \
        {                                                               \
          checks_failed++;                                              \
          printf ("test_contract: FAIL: %s == \"%s\""                   \
                  " (got \"%s\", %s:%d)\n",                             \
                  #a, (b), (a) ? (a) : "(null)",                        \
                  __FILE__, __LINE__);                                  \
        }                                                               \
    }                                                                   \
  while (0)

/* ---------------------------------------------------------------------
 *  Fixtures
 * ------------------------------------------------------------------- */

static char scratch[DBT_SCRATCH];
static struct dbt_request req;

static void
parse (const char *line)
{
  dbt_parse (line, strlen (line), scratch, sizeof scratch, &req);
}

/* ---------------------------------------------------------------------
 *  Insert requests
 * ------------------------------------------------------------------- */

static void
test_runs_valid (void)
{
  parse ("{\"table\": \"runs\", \"row\": {\"descriptor\":"
         " {\"instances\": 3}, \"aggregate_strategy\":"
         " \"majority\"}}");
  CHECK (req.kind == DBT_REQ_INSERT);
  CHECK (req.table == DBT_TABLE_RUNS);
  CHECK_STR (req.row.runs.descriptor, "{\"instances\": 3}");
  CHECK_STR (req.row.runs.aggregate_strategy, "majority");
  CHECK (req.row.runs.final_output == NULL);
}

static void
test_runs_optional_final_output (void)
{
  parse ("{\"table\":\"runs\",\"row\":{\"descriptor\":{},"
         "\"aggregate_strategy\":\"mean\",\"final_output\":"
         "{\"answer\":42}}}");
  CHECK (req.kind == DBT_REQ_INSERT);
  CHECK_STR (req.row.runs.final_output, "{\"answer\":42}");
}

static void
test_instances_valid (void)
{
  parse ("{\"table\": \"run_instances\", \"row\":"
         " {\"run_id\": 42, \"topology\": \"10,20,5\","
         " \"seed\": 123456789012, \"input\": [0.5, 0.3],"
         " \"output\": {\"out\": 1.0}, \"score\": 0.875,"
         " \"status\": \"ok\"}}");
  CHECK (req.kind == DBT_REQ_INSERT);
  CHECK (req.table == DBT_TABLE_RUN_INSTANCES);
  CHECK (req.row.instances.run_id == 42);
  CHECK_STR (req.row.instances.topology, "10,20,5");
  CHECK (req.row.instances.has_seed);
  CHECK (req.row.instances.seed == 123456789012LL);
  CHECK_STR (req.row.instances.input, "[0.5, 0.3]");
  CHECK_STR (req.row.instances.output, "{\"out\": 1.0}");
  CHECK (req.row.instances.has_score);
  CHECK (req.row.instances.score > 0.874 && req.row.instances.score < 0.876);
  CHECK_STR (req.row.instances.status, "ok");
}

static void
test_instances_null_optionals (void)
{
  parse ("{\"table\":\"run_instances\",\"row\":{\"run_id\":1,"
         "\"topology\":\"2\",\"input\":{},\"seed\":null,"
         "\"output\":null,\"score\":null,\"status\":\"timeout\"}}");
  CHECK (req.kind == DBT_REQ_INSERT);
  CHECK (!req.row.instances.has_seed);
  CHECK (req.row.instances.output == NULL);
  CHECK (!req.row.instances.has_score);
  CHECK_STR (req.row.instances.status, "timeout");
}

static void
test_training_valid (void)
{
  parse ("{\"table\": \"training_data\", \"row\":"
         " {\"source_url\": \"https://example.org/a\","
         " \"http_status\": 404, \"content\": \"page not found\","
         " \"checksum\":"
         " \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}}");
  CHECK (req.kind == DBT_REQ_INSERT);
  CHECK (req.table == DBT_TABLE_TRAINING_DATA);
  CHECK_STR (req.row.training.source_url, "https://example.org/a");
  CHECK (req.row.training.has_http_status);
  CHECK (req.row.training.http_status == 404);
  CHECK_STR (req.row.training.content, "page not found");
  CHECK_STR (req.row.training.checksum,
             "0123456789abcdef0123456789abcdef"
             "0123456789abcdef0123456789abcdef");
}

static void
test_string_escapes (void)
{
  /* The decoded strings must reach the driver decoded, and the
   * \uXXXX forms must become UTF-8 (SPEC.md section 3). */
  parse ("{\"table\":\"training_data\",\"row\":"
         "{\"source_url\":\"a\\tb\","
         "\"content\":\"line1\\nline2 \\\"quoted\\\" \\u00e9 \\ud83d\\ude00\","
         "\"checksum\":"
         "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}}");
  CHECK (req.kind == DBT_REQ_INSERT);
  CHECK_STR (req.row.training.source_url, "a\tb");
  CHECK_STR (req.row.training.content,
             "line1\nline2 \"quoted\" \xc3\xa9 \xf0\x9f\x98\x80");
}

/* ---------------------------------------------------------------------
 *  Status requests
 * ------------------------------------------------------------------- */

static void
test_status_request (void)
{
  parse ("{\"status\": true}");
  CHECK (req.kind == DBT_REQ_STATUS);

  parse ("  { \"status\" : true }  ");
  CHECK (req.kind == DBT_REQ_STATUS);

  /* status mixed with an instruction is a contract violation */
  parse ("{\"status\": true, \"table\": \"runs\"}");
  CHECK (req.kind == DBT_REQ_INVALID);
  CHECK (req.invalid == DBT_INVALID_KEY);
}

/* ---------------------------------------------------------------------
 *  Contract violations
 * ------------------------------------------------------------------- */

static void
test_invalid_lines (void)
{
  parse ("");
  CHECK (req.kind == DBT_REQ_INVALID);
  CHECK (req.invalid == DBT_INVALID_LINE);

  parse ("garbage");
  CHECK (req.invalid == DBT_INVALID_LINE);

  parse ("{}");
  CHECK (req.invalid == DBT_INVALID_TABLE);

  parse ("{\"table\": \"runs\", \"row\": {}}  trailing");
  CHECK (req.invalid == DBT_INVALID_LINE);

  parse ("{\"table\": \"runs\"");
  CHECK (req.invalid == DBT_INVALID_JSON);

  parse ("{\"table\": \"runs\", \"row\":");
  CHECK (req.invalid == DBT_INVALID_JSON);

  parse ("{\"table\": \"runs\", \"row\": {\"descriptor\": {}}");
  CHECK (req.invalid == DBT_INVALID_JSON);
}

static void
test_invalid_tables (void)
{
  parse ("{\"table\": \"incidents\", \"row\": {}}");
  CHECK (req.kind == DBT_REQ_INVALID);
  CHECK (req.invalid == DBT_INVALID_TABLE);
  /* The reason token is stable; the table name is not the detail:
   * a caller must be able to switch on the reason alone. */
  CHECK_STR (req.invalid_detail, "");            /* phase 2 */

  parse ("{\"table\": \"users\", \"row\": {}}");
  CHECK (req.invalid == DBT_INVALID_TABLE);      /* phase 5 */

  parse ("{\"row\": {}}");
  CHECK (req.invalid == DBT_INVALID_TABLE);

  parse ("{\"table\": 42, \"row\": {}}");
  CHECK (req.invalid == DBT_INVALID_JSON);
}

static void
test_select_refused (void)
{
  parse ("{\"select\": \"runs\", \"where\": {}}");
  CHECK (req.kind == DBT_REQ_INVALID);
  CHECK (req.invalid == DBT_INVALID_SELECT);
  CHECK_STR (req.invalid_detail, "select");
}

static void
test_unknown_keys (void)
{
  parse ("{\"table\": \"runs\", \"row\": {\"descriptor\": {},"
         " \"aggregate_strategy\": \"majority\", \"oops\": 1}}");
  CHECK (req.kind == DBT_REQ_INVALID);
  CHECK (req.invalid == DBT_INVALID_KEY);
  CHECK_STR (req.invalid_detail, "oops");

  parse ("{\"table\": \"runs\", \"wrong\": 1}");
  CHECK (req.invalid == DBT_INVALID_KEY);
  CHECK_STR (req.invalid_detail, "wrong");
}

static void
test_missing_and_mistyped_fields (void)
{
  parse ("{\"table\": \"runs\", \"row\": {\"aggregate_strategy\":"
         " \"majority\"}}");
  CHECK (req.invalid == DBT_INVALID_FIELD);
  CHECK_STR (req.invalid_detail, "descriptor");

  parse ("{\"table\": \"runs\", \"row\": {\"descriptor\": \"not an obj\","
         " \"aggregate_strategy\": \"majority\"}}");
  CHECK (req.invalid == DBT_INVALID_TYPE);
  CHECK_STR (req.invalid_detail, "descriptor");

  parse ("{\"table\": \"run_instances\", \"row\": {\"run_id\": 1.5,"
         " \"topology\": \"1\", \"input\": [], \"status\": \"ok\"}}");
  CHECK (req.invalid == DBT_INVALID_TYPE);
  CHECK_STR (req.invalid_detail, "run_id");

  parse ("{\"table\": \"run_instances\", \"row\": {\"run_id\": \"42\","
         " \"topology\": \"1\", \"input\": [], \"status\": \"ok\"}}");
  CHECK (req.invalid == DBT_INVALID_TYPE);
  CHECK_STR (req.invalid_detail, "run_id");

  parse ("{\"table\": \"run_instances\", \"row\": {\"topology\": \"1\","
         " \"input\": [], \"status\": \"ok\"}}");
  CHECK (req.invalid == DBT_INVALID_FIELD);
  CHECK_STR (req.invalid_detail, "run_id");
}

static void
test_checksum_rules (void)
{
  /* Too short. */
  parse ("{\"table\": \"training_data\", \"row\":"
         "{\"source_url\": \"u\", \"content\": \"c\","
         " \"checksum\": \"abc\"}}");
  CHECK (req.invalid == DBT_INVALID_CHECKSUM);

  /* Uppercase is refused: the checksum is normalized lowercase by
   * the caller (the orchestrator hashes, it does not format). */
  parse ("{\"table\": \"training_data\", \"row\":"
         "{\"source_url\": \"u\", \"content\": \"c\","
         " \"checksum\":"
         " \"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\"}}");
  CHECK (req.invalid == DBT_INVALID_CHECKSUM);

  /* Not hexadecimal. */
  parse ("{\"table\": \"training_data\", \"row\":"
         "{\"source_url\": \"u\", \"content\": \"c\","
         " \"checksum\":"
         " \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg\"}}");
  CHECK (req.invalid == DBT_INVALID_CHECKSUM);

  /* Missing entirely. */
  parse ("{\"table\": \"training_data\", \"row\":"
         "{\"source_url\": \"u\", \"content\": \"c\"}}");
  CHECK (req.invalid == DBT_INVALID_FIELD);
  CHECK_STR (req.invalid_detail, "checksum");
}

/* ---------------------------------------------------------------------
 *  Response builders
 * ------------------------------------------------------------------- */

static void
test_responses (void)
{
  char buf[DBT_MAX_RESPONSE];

  CHECK (dbt_response_ok (buf, sizeof buf, 42) > 0);
  CHECK_STR (buf, "{\"ok\": true, \"id\": 42}");

  CHECK (dbt_response_duplicate (buf, sizeof buf, 17) > 0);
  CHECK_STR (buf, "{\"duplicate\": 17}");

  CHECK (dbt_response_invalid (buf, sizeof buf, DBT_INVALID_CHECKSUM,
                               "checksum") > 0);
  CHECK_STR (buf, "{\"invalid\": \"checksum\"}");

  CHECK (dbt_response_invalid (buf, sizeof buf, DBT_INVALID_SELECT,
                               NULL) > 0);
  CHECK_STR (buf, "{\"invalid\": \"select\"}");

  CHECK (dbt_response_empty (buf, sizeof buf) > 0);
  CHECK_STR (buf, "{\"empty\": true}");

  /* Too small a buffer: refuses rather than truncating a JSON
   * line (a truncated line is a lie). */
  char tiny[8];
  CHECK (dbt_response_ok (tiny, sizeof tiny, 42) == 0);
}

/* ---------------------------------------------------------------------
 *  Schema synchronization
 *
 * The embedded DDL of storage.c and the schema.sql file are the
 * SAME text or `make check` fails: neither copy can ever drift
 * (PLAN.md section 6, the schema is frozen in review, not by
 * copy-paste).
 * ------------------------------------------------------------------- */

static void
test_schema_in_sync (void)
{
#ifdef DBT_SCHEMA_SQL_PATH
  FILE *f = fopen (DBT_SCHEMA_SQL_PATH, "rb");
  static char file_text[128 * 1024];
  size_t n;

  CHECK (f != NULL);
  if (f == NULL)
    return;
  n = fread (file_text, 1, sizeof file_text - 1, f);
  fclose (f);
  file_text[n] = '\0';

  CHECK_STR (dbt_schema_ddl (), file_text);
#else
  printf ("test_contract: schema sync check skipped"
          " (no DBT_SCHEMA_SQL_PATH)\n");
#endif
}

/* ---------------------------------------------------------------------
 *  Run
 * ------------------------------------------------------------------- */

int
main (void)
{
  test_runs_valid ();
  test_runs_optional_final_output ();
  test_instances_valid ();
  test_instances_null_optionals ();
  test_training_valid ();
  test_string_escapes ();
  test_status_request ();
  test_invalid_lines ();
  test_invalid_tables ();
  test_select_refused ();
  test_unknown_keys ();
  test_missing_and_mistyped_fields ();
  test_checksum_rules ();
  test_responses ();
  test_schema_in_sync ();

  printf ("test_contract: %d checks, %d failures\n",
          checks_run, checks_failed);
  return checks_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
