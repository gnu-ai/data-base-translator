/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * contract.h — The frozen JSON line contract of /db (PLAN.md 3.3).
 *
 * One instruction is ONE complete JSON line (newline terminated);
 * the answer is ONE JSON line.  Everything the caller needs to know
 * is in the data: a constraint of the database is a readable status
 * ("duplicate"), never a silent overwrite and never a fatal POSIX
 * error (PLAN.md section 4).
 *
 * This layer is pure C23/POSIX: no libpq, no Hurd library, no
 * allocation — the caller provides the buffers.  It is therefore
 * testable on any POSIX system, exactly like the neuron core.
 */

#ifndef DBT_CONTRACT_H
#define DBT_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>

/* --- Limits (phase 1) ------------------------------------------------
 * Large paginated contents arrive with phase 3; until then one
 * instruction line is bounded, and so is the memory of every open
 * file of the translator. */
#define DBT_MAX_LINE     (256u * 1024u)     /* bytes per instruction */
#define DBT_MAX_FIELDS   16                 /* fields per "row" object */
#define DBT_MAX_KEY      31                 /* bytes per key, NUL excl. */
/* Scratch arena: decoded strings and raw JSON spans never exceed the
 * line itself plus one NUL per span, with a wide margin. */
#define DBT_SCRATCH      (DBT_MAX_LINE + 4096u)
#define DBT_MAX_RESPONSE 256                /* responses are tiny */

/* --- Instruction kinds ---------------------------------------------- */
enum dbt_kind
  {
    DBT_REQ_INVALID = 0,   /* contract violated; see req->invalid */
    DBT_REQ_INSERT,        /* {"table": ..., "row": {...}} */
    DBT_REQ_STATUS,        /* {"status": true} */
  };

/* --- Tables addressable in phase 1 ---------------------------------- */
enum dbt_table
  {
    DBT_TABLE_NONE = 0,
    DBT_TABLE_RUNS,
    DBT_TABLE_RUN_INSTANCES,
    DBT_TABLE_TRAINING_DATA,
  };

/* --- Machine readable reasons of the "invalid" response -------------- */
enum dbt_invalid
  {
    DBT_INVALID_LINE = 0,  /* not one single complete JSON object */
    DBT_INVALID_JSON,      /* malformed JSON */
    DBT_INVALID_TABLE,     /* unknown table, or "table" missing */
    DBT_INVALID_FIELD,     /* required field missing */
    DBT_INVALID_TYPE,      /* field present with the wrong JSON type */
    DBT_INVALID_KEY,       /* unknown key, at top level or in the row */
    DBT_INVALID_CHECKSUM,  /* not 64 lowercase hexadecimal characters */
    DBT_INVALID_SELECT,    /* "select" arrives with phase 2 */
    DBT_INVALID_TOOLONG,   /* instruction line exceeds DBT_MAX_LINE */
  };

/* Short stable token of a reason ("checksum", "json", ...), as it
 * appears in the response. */
const char *dbt_invalid_reason (enum dbt_invalid reason);

/* --- Typed rows ------------------------------------------------------
 * String pointers reference the caller's scratch arena and stay valid
 * as long as the arena does.  Raw JSON text (jsonb columns) is kept
 * verbatim: the server parses it, not us. */
struct dbt_row_runs
  {
    const char *descriptor;          /* raw JSON object, required */
    const char *aggregate_strategy;  /* decoded string, required */
    const char *final_output;        /* raw JSON, NULL when absent */
  };

struct dbt_row_instances
  {
    long long  run_id;               /* required */
    const char *topology;            /* required */
    bool        has_seed;
    long long   seed;                /* optional integer */
    const char *input;               /* raw JSON, required */
    const char *output;              /* raw JSON, NULL when absent */
    bool        has_score;
    double      score;               /* optional number */
    const char *status;              /* required */
  };

struct dbt_row_training
  {
    const char *source_url;           /* required */
    bool        has_http_status;
    long        http_status;         /* optional integer */
    const char *content;             /* required */
    const char *checksum;            /* 64 lowercase hex, required */
  };

struct dbt_request
  {
    enum dbt_kind   kind;
    enum dbt_table  table;
    /* When kind is DBT_REQ_INVALID: the reason and, when relevant, the
     * offending key ("checksum", "run_id", ...). */
    enum dbt_invalid invalid;
    char             invalid_detail[DBT_MAX_KEY + 1];
    union
      {
        struct dbt_row_runs       runs;
        struct dbt_row_instances  instances;
        struct dbt_row_training   training;
      } row;
  };

/* Parse one complete instruction line (without its newline).
 *
 * LINE is read only; SCRATCH is the arena where decoded strings and
 * raw JSON spans are stored.  REQ is always filled: on failure its
 * kind is DBT_REQ_INVALID and invalid/invalid_detail explain why.
 * The function itself never allocates and never fails. */
void dbt_parse (const char *line, size_t len,
                char *scratch, size_t scratch_cap,
                struct dbt_request *req);

/* --- Response builders ------------------------------------------------
 * Write one JSON line (no trailing newline) into BUF and return its
 * length.  If the output does not fit, an empty response is written
 * and 0 returned: responses are tiny by construction. */
size_t dbt_response_ok (char *buf, size_t cap, long long id);
size_t dbt_response_duplicate (char *buf, size_t cap, long long id);
size_t dbt_response_invalid (char *buf, size_t cap,
                             enum dbt_invalid reason,
                             const char *detail); /* NULL: generic */
size_t dbt_response_empty (char *buf, size_t cap);

/* Append one field `"key": number` to a JSON object being built at
 * BUF (used by the status line in ops.c); returns the new length. */
size_t dbt_json_number (char *buf, size_t len, size_t cap,
                        const char *key, long long value);

#endif /* DBT_CONTRACT_H */
