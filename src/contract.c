/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * contract.c — The frozen JSON line contract of /db (PLAN.md 3.3).
 *
 * A hand written, allocation free, single pass JSON scanner.  Only
 * what the contract needs is accepted; everything else is a
 * readable "invalid" status, never a crash and never a guess.  The
 * discipline of the stack applies here: zero allocation in the hot
 * path — the caller provides the line, the scratch arena and the
 * request structure, all pre-allocated.
 */

#include "contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 *  Reasons
 * ------------------------------------------------------------------- */

const char *
dbt_invalid_reason (enum dbt_invalid reason)
{
  switch (reason)
    {
    case DBT_INVALID_LINE:     return "line";
    case DBT_INVALID_JSON:     return "json";
    case DBT_INVALID_TABLE:   return "table";
    case DBT_INVALID_FIELD:   return "field";
    case DBT_INVALID_TYPE:    return "type";
    case DBT_INVALID_KEY:      return "key";
    case DBT_INVALID_CHECKSUM: return "checksum";
    case DBT_INVALID_SELECT:  return "select";
    case DBT_INVALID_TOOLONG: return "toolong";
    }
  return "line";
}

/* Copy an offending key into the request detail, bounded by the
 * contract's key limit: the detail is data for the caller's error
 * path, never a buffer hazard. */
static void
set_detail (struct dbt_request *req, const char *key)
{
  size_t n = strlen (key);

  if (n > DBT_MAX_KEY)
    n = DBT_MAX_KEY;
  memcpy (req->invalid_detail, key, n);
  req->invalid_detail[n] = '\0';
}

/* ---------------------------------------------------------------------
 *  Scanner state
 * ---------------------------------------------------------------------
 *  The scanner walks LINE by position.  Decoded strings and raw
 *  JSON spans are appended to the bump allocated SCRATCH arena,
 *  owned by the caller.  ERR remembers the first anomaly: the scan
 *  unwinds immediately, the caller gets ONE reason per line. */

struct scanner
  {
    const char *s;            /* the line */
    size_t      pos;
    size_t      len;
    char       *scratch;      /* arena base */
    size_t      scratch_cap;
    size_t      scratch_len;  /* bump cursor */
    int         err;          /* 0 ok, 1 JSON error, 2 arena full */
  };

static void
fail (struct scanner *sc, int err)
{
  if (sc->err == 0)
    sc->err = err;
}

/* Reserve N bytes in the arena.  The caller must set scratch_len to
 * the bytes actually used before the next reservation (reservations
 * are upper bounds, never left dangling). */
static char *
arena_take (struct scanner *sc, size_t n)
{
  char *p;

  if (sc->scratch_len + n > sc->scratch_cap)
    {
      fail (sc, 2);
      return NULL;
    }
  p = sc->scratch + sc->scratch_len;
  return p;
}

static void
skip_ws (struct scanner *sc)
{
  while (sc->pos < sc->len)
    {
      char c = sc->s[sc->pos];

      if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        sc->pos++;
      else
        break;
    }
}

static int
peek (struct scanner *sc)
{
  return sc->pos < sc->len ? (unsigned char) sc->s[sc->pos] : -1;
}

static int
next (struct scanner *sc)
{
  return sc->pos < sc->len ? (unsigned char) sc->s[sc->pos++] : -1;
}

/* Length of one code point encoded as UTF-8. */
static size_t
utf8_len (unsigned long cp)
{
  if (cp < 0x80)
    return 1;
  if (cp < 0x800)
    return 2;
  if (cp < 0x10000)
    return 3;
  return 4;
}

/* Encode one code point as UTF-8 at OUT, advancing it. */
static void
put_utf8 (char **out, unsigned long cp)
{
  char *p = *out;

  if (cp < 0x80)
    p[0] = (char) cp;
  else if (cp < 0x800)
    {
      p[0] = (char) (0xC0 | (cp >> 6));
      p[1] = (char) (0x80 | (cp & 0x3F));
    }
  else if (cp < 0x10000)
    {
      p[0] = (char) (0xE0 | (cp >> 12));
      p[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
      p[2] = (char) (0x80 | (cp & 0x3F));
    }
  else
    {
      p[0] = (char) (0xF0 | (cp >> 18));
      p[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
      p[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
      p[3] = (char) (0x80 | (cp & 0x3F));
    }
  *out = p + utf8_len (cp);
}

/* Read exactly 4 hex digits (after \u).  Returns the code point, or
 * (unsigned long) -1 on error. */
static unsigned long
read_hex4 (struct scanner *sc)
{
  unsigned long cp = 0;

  for (int i = 0; i < 4; i++)
    {
      int c = next (sc);
      int v;

      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      else
        {
          fail (sc, 1);
          return (unsigned long) -1;
        }
      cp = cp * 16 + (unsigned long) v;
    }
  return cp;
}

/* ---------------------------------------------------------------------
 *  parse_string — decode a JSON string into the arena.
 *
 *  The decoded form is never longer than the encoded form, so the
 *  whole remaining span is reserved once, then the reservation is
 *  trimmed to the bytes actually produced: at most one reservation
 *  is live at a time, and (decoded so far) + (reservation) never
 *  exceeds the line itself — the arena is sized with the line.
 * ------------------------------------------------------------------- */

static const char *
parse_string (struct scanner *sc, size_t *len_out)
{
  size_t base = sc->scratch_len;
  char *start;
  char *out;

  if (next (sc) != '"')
    {
      fail (sc, 1);
      return NULL;
    }
  start = arena_take (sc, sc->len - sc->pos + 1);
  if (start == NULL)
    return NULL;
  out = start;

  for (;;)
    {
      int c = peek (sc);

      if (c < 0)
        {
          fail (sc, 1);                   /* unterminated string */
          return NULL;
        }
      if (c < 0x20)
        {
          fail (sc, 1);                   /* raw control character */
          return NULL;
        }
      if (c == '"')
        {
          sc->pos++;
          *out = '\0';
          sc->scratch_len = base + (size_t) (out - start) + 1;
          if (len_out)
            *len_out = (size_t) (out - start);
          return start;
        }
      if (c == '\\')
        {
          sc->pos++;
          int e = next (sc);

          switch (e)
            {
            case '"':  *out++ = '"';  break;
            case '\\': *out++ = '\\'; break;
            case '/':  *out++ = '/';  break;
            case 'b':  *out++ = '\b'; break;
            case 'f':  *out++ = '\f'; break;
            case 'n':  *out++ = '\n'; break;
            case 'r':  *out++ = '\r'; break;
            case 't':  *out++ = '\t'; break;
            case 'u':
              {
                unsigned long cp = read_hex4 (sc);

                if (sc->err)
                  return NULL;
                /* Surrogate pair \uD8xx\uDCxx, per JSON. */
                if (cp >= 0xD800 && cp <= 0xDBFF
                    && sc->pos + 1 < sc->len
                    && sc->s[sc->pos] == '\\'
                    && sc->s[sc->pos + 1] == 'u')
                  {
                    unsigned long lo;

                    sc->pos += 2;
                    lo = read_hex4 (sc);
                    if (sc->err)
                      return NULL;
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                      cp = 0x10000
                           + ((cp - 0xD800) << 10)
                           + (lo - 0xDC00);
                    else
                      {
                        fail (sc, 1);
                        return NULL;
                      }
                  }
                /* A lone surrogate is passed as its code point:
                 * the contract is faithful, not a validator. */
                put_utf8 (&out, cp);
                break;
              }
            default:
              fail (sc, 1);
              return NULL;
            }
          continue;
        }
      *out++ = (char) c;
      sc->pos++;
    }
}

/* ---------------------------------------------------------------------
 *  skip_value — walk one JSON value without decoding it; the span
 *  [start, end) lies inside the LINE.  Used for raw jsonb fields:
 *  the server parses them, we carry them verbatim.
 * ------------------------------------------------------------------- */

static int
skip_value (struct scanner *sc, size_t *start_out, size_t *end_out)
{
  size_t start = sc->pos;
  int c = peek (sc);

  if (c == '"')
    {
      size_t ignored;

      if (parse_string (sc, &ignored) == NULL)
        return -1;                        /* arena scratchpad only */
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }

  if (c == '{' || c == '[')
    {
      int depth = 0;

      while (sc->pos < sc->len)
        {
          int d = peek (sc);

          if (d == '"')
            {
              size_t ignored;

              if (parse_string (sc, &ignored) == NULL)
                return -1;
              continue;
            }
          sc->pos++;
          if (d == '{' || d == '[')
            depth++;
          else if (d == '}' || d == ']')
            {
              depth--;
              if (depth == 0)
                {
                  *start_out = start;
                  *end_out = sc->pos;
                  return 0;
                }
              if (depth < 0)
                {
                  fail (sc, 1);
                  return -1;
                }
            }
        }
      fail (sc, 1);                       /* unbalanced */
      return -1;
    }

  /* Literals and numbers. */
  if (c == 't' && sc->len - sc->pos >= 4
      && memcmp (sc->s + sc->pos, "true", 4) == 0)
    {
      sc->pos += 4;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == 'f' && sc->len - sc->pos >= 5
      && memcmp (sc->s + sc->pos, "false", 5) == 0)
    {
      sc->pos += 5;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == 'n' && sc->len - sc->pos >= 4
      && memcmp (sc->s + sc->pos, "null", 4) == 0)
    {
      sc->pos += 4;
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }
  if (c == '-' || (c >= '0' && c <= '9'))
    {
      sc->pos++;                          /* sign or first digit */
      for (;;)
        {
          int d = peek (sc);

          if ((d >= '0' && d <= '9') || d == '.' || d == 'e'
              || d == 'E' || d == '+' || d == '-')
            sc->pos++;
          else
            break;
        }
      *start_out = start;
      *end_out = sc->pos;
      return 0;
    }

  fail (sc, 1);
  return -1;
}

/* Copy the span [start, end) of the LINE into the arena, verbatim
 * and NUL terminated: the raw text handed to the server for the
 * jsonb columns. */
static const char *
copy_raw (struct scanner *sc, size_t start, size_t end)
{
  char *p = arena_take (sc, end - start + 1);

  if (p == NULL)
    return NULL;
  memcpy (p, sc->s + start, end - start);
  p[end - start] = '\0';
  sc->scratch_len += end - start + 1;
  return p;
}

/* ---------------------------------------------------------------------
 *  Generic row field
 * ------------------------------------------------------------------- */

enum field_type
  {
    F_STR,      /* decoded string in the arena */
    F_NUM,      /* number span, NUL terminated in the arena */
    F_NULL,     /* JSON null */
    F_RAW,      /* object or array, verbatim span in the arena */
    F_BOOL,     /* true or false */
  };

struct field
  {
    char           key[DBT_MAX_KEY + 1];
    enum field_type type;
    const char    *str;     /* F_STR: decoded, NUL terminated */
    size_t         str_len;
    const char    *num;     /* F_NUM: NUL terminated span */
    const char    *raw;     /* F_RAW: verbatim, NUL terminated */
    bool           boolean;
  };

/* ---------------------------------------------------------------------
 *  Span helpers (spans are NUL terminated in the arena)
 * ------------------------------------------------------------------- */

/* A pure integer: optional '-', then digits only. */
static bool
span_is_integer (const char *s)
{
  if (*s == '\0')
    return false;
  if (*s == '-')
    s++;
  if (*s == '\0')
    return false;
  for (; *s != '\0'; s++)
    if (*s < '0' || *s > '9')
      return false;
  return true;
}

static bool
span_to_integer (const char *s, long long *out)
{
  if (!span_is_integer (s))
    return false;
  *out = strtoll (s, NULL, 10);
  return true;
}

/* checksum: exactly 64 characters, all lowercase hex. */
static bool
span_is_checksum (const char *s)
{
  size_t n = strlen (s);

  if (n != 64)
    return false;
  for (size_t i = 0; i < n; i++)
    {
      char c = s[i];

      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return false;
    }
  return true;
}

/* ---------------------------------------------------------------------
 *  Row object scan: fills FIELDS[0..*nfields).
 * ------------------------------------------------------------------- */

static int
parse_row_fields (struct scanner *sc, struct dbt_request *req,
                  struct field *fields, int *nfields)
{
  *nfields = 0;
  skip_ws (sc);
  if (next (sc) != '{')
    {
      fail (sc, 1);
      return -1;
    }
  skip_ws (sc);
  if (peek (sc) == '}')                   /* empty object: legal */
    {
      sc->pos++;
      return 0;
    }
  for (;;)
    {
      const char *key;
      size_t keylen = 0;
      struct field *f;

      skip_ws (sc);
      key = parse_string (sc, &keylen);
      if (key == NULL)
        return -1;

      if (*nfields >= DBT_MAX_FIELDS)
        {
          fail (sc, 1);
          return -1;
        }
      f = &fields[(*nfields)++];
      memset (f, 0, sizeof *f);

      /* A key longer than the contract's limit can only be an
       * unknown key: reported as such, truncated for the detail. */
      if (keylen > DBT_MAX_KEY)
        {
          req->invalid = DBT_INVALID_KEY;
          memcpy (req->invalid_detail, key, DBT_MAX_KEY);
          req->invalid_detail[DBT_MAX_KEY] = '\0';
          return -1;
        }
      memcpy (f->key, key, keylen);
      f->key[keylen] = '\0';

      skip_ws (sc);
      if (next (sc) != ':')
        {
          fail (sc, 1);
          return -1;
        }
      skip_ws (sc);

      {
        int c = peek (sc);

        if (c == '"')
          {
            f->type = F_STR;
            f->str = parse_string (sc, &f->str_len);
            if (f->str == NULL)
              return -1;
          }
        else if (c == '{' || c == '[')
          {
            size_t s0, e0;

            f->type = F_RAW;
            if (skip_value (sc, &s0, &e0) < 0)
              return -1;
            f->raw = copy_raw (sc, s0, e0);
            if (f->raw == NULL)
              return -1;
          }
        else
          {
            size_t s0, e0;

            if (skip_value (sc, &s0, &e0) < 0)
              return -1;
            if (e0 - s0 == 4 && memcmp (sc->s + s0, "null", 4) == 0)
              f->type = F_NULL;
            else if (e0 - s0 == 4 && memcmp (sc->s + s0, "true", 4) == 0)
              {
                f->type = F_BOOL;
                f->boolean = true;
              }
            else if (e0 - s0 == 5 && memcmp (sc->s + s0, "false", 5) == 0)
              {
                f->type = F_BOOL;
                f->boolean = false;
              }
            else if (sc->s[s0] == '-' || (sc->s[s0] >= '0'
                                          && sc->s[s0] <= '9'))
              {
                f->type = F_NUM;
                f->num = copy_raw (sc, s0, e0);
                if (f->num == NULL)
                  return -1;
              }
            else
              {
                fail (sc, 1);
                return -1;
              }
          }
      }

      skip_ws (sc);
      {
        int c2 = next (sc);

        if (c2 == ',')
          continue;
        if (c2 == '}')
          return 0;
        fail (sc, 1);
        return -1;
      }
    }
}

/* ---------------------------------------------------------------------
 *  Field lookup used by the per table bindings.
 * ------------------------------------------------------------------- */

static const struct field *
find (const struct field *fields, int nfields, const char *key)
{
  for (int i = 0; i < nfields; i++)
    if (strcmp (fields[i].key, key) == 0)
      return &fields[i];
  return NULL;
}

/* Every binding failure sets the reason AND the offending key, so
 * the caller learns exactly what to fix in its next line. */

static int
bind_runs (const struct field *fields, int nfields,
           struct dbt_request *req)
{
  const struct field *d = find (fields, nfields, "descriptor");
  const struct field *a = find (fields, nfields, "aggregate_strategy");
  const struct field *fo = find (fields, nfields, "final_output");

  if (d == NULL)
    {
      req->invalid = DBT_INVALID_FIELD;
      strcpy (req->invalid_detail, "descriptor");
      return -1;
    }
  if (d->type != F_RAW || d->raw[0] != '{')
    {
      req->invalid = DBT_INVALID_TYPE;
      strcpy (req->invalid_detail, "descriptor");
      return -1;
    }
  if (a == NULL)
    {
      req->invalid = DBT_INVALID_FIELD;
      strcpy (req->invalid_detail, "aggregate_strategy");
      return -1;
    }
  if (a->type != F_STR)
    {
      req->invalid = DBT_INVALID_TYPE;
      strcpy (req->invalid_detail, "aggregate_strategy");
      return -1;
    }
  if (fo != NULL)
    {
      if (fo->type == F_NULL)
        fo = NULL;                        /* null == absent */
      else if (fo->type != F_RAW)
        {
          req->invalid = DBT_INVALID_TYPE;
          strcpy (req->invalid_detail, "final_output");
          return -1;
        }
    }

  /* Unknown keys are refused: the contract is frozen, a typo must
   * never silently drop a field (PLAN.md section 3.3). */
  for (int i = 0; i < nfields; i++)
    {
      const char *k = fields[i].key;

      if (strcmp (k, "descriptor") != 0
          && strcmp (k, "aggregate_strategy") != 0
          && strcmp (k, "final_output") != 0)
        {
          req->invalid = DBT_INVALID_KEY;
          set_detail (req, k);
          return -1;
        }
    }

  req->row.runs.descriptor = d->raw;
  req->row.runs.aggregate_strategy = a->str;
  req->row.runs.final_output = fo != NULL ? fo->raw : NULL;
  return 0;
}

static int
bind_instances (const struct field *fields, int nfields,
                struct dbt_request *req)
{
  const struct field *rid = find (fields, nfields, "run_id");
  const struct field *top = find (fields, nfields, "topology");
  const struct field *in = find (fields, nfields, "input");
  const struct field *out = find (fields, nfields, "output");
  const struct field *seed = find (fields, nfields, "seed");
  const struct field *score = find (fields, nfields, "score");
  const struct field *st = find (fields, nfields, "status");

  if (rid == NULL)
    {
      req->invalid = DBT_INVALID_FIELD;
      strcpy (req->invalid_detail, "run_id");
      return -1;
    }
  if (rid->type != F_NUM
      || !span_to_integer (rid->num, &req->row.instances.run_id))
    {
      req->invalid = DBT_INVALID_TYPE;
      strcpy (req->invalid_detail, "run_id");
      return -1;
    }

  if (top == NULL)
    {
      req->invalid = DBT_INVALID_FIELD;
      strcpy (req->invalid_detail, "topology");
      return -1;
    }
  if (top->type != F_STR)
    {
      req->invalid = DBT_INVALID_TYPE;
      strcpy (req->invalid_detail, "topology");
      return -1;
    }

  if (in == NULL)
    {
      req->invalid = DBT_INVALID_FIELD;
      strcpy (req->invalid_detail, "input");
      return -1;
    }
  if (in->type != F_RAW)
    {
      req->invalid = DBT_INVALID_TYPE;
      strcpy (req->invalid_detail, "input");
      return -1;
    }

  if (st == NULL)
    {
      req->invalid = DBT_INVALID_FIELD;
      strcpy (req->invalid_detail, "status");
      return -1;
    }
  if (st->type != F_STR)
    {
      req->invalid = DBT_INVALID_TYPE;
      strcpy (req->invalid_detail, "status");
      return -1;
    }

  /* Optional fields: null and absent are equivalent. */
  if (seed != NULL)
    {
      if (seed->type == F_NULL)
        seed = NULL;
      else if (seed->type == F_NUM
               && span_to_integer (seed->num, &req->row.instances.seed))
        req->row.instances.has_seed = true;
      else
        {
          req->invalid = DBT_INVALID_TYPE;
          strcpy (req->invalid_detail, "seed");
          return -1;
        }
    }
  if (out != NULL)
    {
      if (out->type == F_NULL)
        out = NULL;
      else if (out->type != F_RAW)
        {
          req->invalid = DBT_INVALID_TYPE;
          strcpy (req->invalid_detail, "output");
          return -1;
        }
    }
  if (score != NULL)
    {
      if (score->type == F_NULL)
        score = NULL;
      else if (score->type == F_NUM)
        {
          req->row.instances.score = strtod (score->num, NULL);
          req->row.instances.has_score = true;
        }
      else
        {
          req->invalid = DBT_INVALID_TYPE;
          strcpy (req->invalid_detail, "score");
          return -1;
        }
    }

  for (int i = 0; i < nfields; i++)
    {
      const char *k = fields[i].key;

      if (strcmp (k, "run_id") != 0 && strcmp (k, "topology") != 0
          && strcmp (k, "input") != 0 && strcmp (k, "output") != 0
          && strcmp (k, "seed") != 0 && strcmp (k, "score") != 0
          && strcmp (k, "status") != 0)
        {
          req->invalid = DBT_INVALID_KEY;
          set_detail (req, k);
          return -1;
        }
    }

  req->row.instances.topology = top->str;
  req->row.instances.input = in->raw;
  req->row.instances.output = out != NULL ? out->raw : NULL;
  req->row.instances.status = st->str;
  return 0;
}

static int
bind_training (const struct field *fields, int nfields,
               struct dbt_request *req)
{
  const struct field *url = find (fields, nfields, "source_url");
  const struct field *hs = find (fields, nfields, "http_status");
  const struct field *ct = find (fields, nfields, "content");
  const struct field *cs = find (fields, nfields, "checksum");

  if (url == NULL)
    {
      req->invalid = DBT_INVALID_FIELD;
      strcpy (req->invalid_detail, "source_url");
      return -1;
    }
  if (url->type != F_STR)
    {
      req->invalid = DBT_INVALID_TYPE;
      strcpy (req->invalid_detail, "source_url");
      return -1;
    }
  if (ct == NULL)
    {
      req->invalid = DBT_INVALID_FIELD;
      strcpy (req->invalid_detail, "content");
      return -1;
    }
  if (ct->type != F_STR)
    {
      req->invalid = DBT_INVALID_TYPE;
      strcpy (req->invalid_detail, "content");
      return -1;
    }
  if (cs == NULL)
    {
      req->invalid = DBT_INVALID_FIELD;
      strcpy (req->invalid_detail, "checksum");
      return -1;
    }
  if (cs->type != F_STR)
    {
      req->invalid = DBT_INVALID_TYPE;
      strcpy (req->invalid_detail, "checksum");
      return -1;
    }
  /* The translator stays ignorant of the contents but verifies the
   * shape of the unique key before it reaches the constraint. */
  if (!span_is_checksum (cs->str))
    {
      req->invalid = DBT_INVALID_CHECKSUM;
      strcpy (req->invalid_detail, "checksum");
      return -1;
    }
  if (hs != NULL)
    {
      if (hs->type == F_NULL)
        hs = NULL;
      else if (hs->type == F_NUM && span_is_integer (hs->num))
        {
          req->row.training.has_http_status = true;
          req->row.training.http_status = strtol (hs->num, NULL, 10);
        }
      else
        {
          req->invalid = DBT_INVALID_TYPE;
          strcpy (req->invalid_detail, "http_status");
          return -1;
        }
    }

  for (int i = 0; i < nfields; i++)
    {
      const char *k = fields[i].key;

      if (strcmp (k, "source_url") != 0 && strcmp (k, "content") != 0
          && strcmp (k, "checksum") != 0
          && strcmp (k, "http_status") != 0)
        {
          req->invalid = DBT_INVALID_KEY;
          set_detail (req, k);
          return -1;
        }
    }

  req->row.training.source_url = url->str;
  req->row.training.content = ct->str;
  req->row.training.checksum = cs->str;
  return 0;
}

/* ---------------------------------------------------------------------
 *  dbt_parse — one instruction line to one request.
 * ------------------------------------------------------------------- */

void
dbt_parse (const char *line, size_t len,
           char *scratch, size_t scratch_cap,
           struct dbt_request *req)
{
  struct scanner sc;
  struct field fields[DBT_MAX_FIELDS];
  int nfields = 0;
  const char *table_name = NULL;
  bool have_row = false;
  bool have_status = false;

  memset (req, 0, sizeof *req);
  req->kind = DBT_REQ_INVALID;
  req->invalid = DBT_INVALID_LINE;

  sc.s = line;
  sc.pos = 0;
  sc.len = len;
  sc.scratch = scratch;
  sc.scratch_cap = scratch_cap;
  sc.scratch_len = 0;
  sc.err = 0;

  /* The line must be exactly one JSON object. */
  skip_ws (&sc);
  if (peek (&sc) != '{')
    return;
  sc.pos++;

  skip_ws (&sc);
  if (peek (&sc) == '}')
    {
      sc.pos++;
      skip_ws (&sc);
      if (sc.pos == sc.len)
        req->invalid = DBT_INVALID_TABLE;  /* no table, no status */
      return;
    }

  for (;;)
    {
      const char *key;
      size_t keylen = 0;

      skip_ws (&sc);
      key = parse_string (&sc, &keylen);
      if (key == NULL)
        {
          req->invalid = DBT_INVALID_JSON;
          return;
        }
      if (keylen > DBT_MAX_KEY)
        {
          req->invalid = DBT_INVALID_KEY;
          memcpy (req->invalid_detail, key, DBT_MAX_KEY);
          req->invalid_detail[DBT_MAX_KEY] = '\0';
          return;
        }

      skip_ws (&sc);
      if (next (&sc) != ':')
        {
          req->invalid = DBT_INVALID_JSON;
          return;
        }
      skip_ws (&sc);

      if (strncmp (key, "table", 6) == 0)
        {
          size_t l = 0;

          table_name = parse_string (&sc, &l);
          if (table_name == NULL)
            {
              req->invalid = DBT_INVALID_JSON;
              return;
            }
        }
      else if (strncmp (key, "row", 4) == 0)
        {
          if (parse_row_fields (&sc, req, fields, &nfields) < 0)
            {
              if (req->invalid == DBT_INVALID_LINE)
                req->invalid = DBT_INVALID_JSON;
              return;
            }
          have_row = true;
        }
      else if (strncmp (key, "status", 7) == 0)
        {
          size_t s0, e0;

          if (skip_value (&sc, &s0, &e0) < 0
              || !(e0 - s0 == 4 && memcmp (sc.s + s0, "true", 4) == 0))
            {
              req->invalid = DBT_INVALID_TYPE;
              strcpy (req->invalid_detail, "status");
              return;
            }
          have_status = true;
        }
      else if (strncmp (key, "select", 7) == 0)
        {
          /* Recognized on purpose: the read half of the contract
           * arrives with phase 2, and a caller using it early gets
           * a precise answer, not a generic one. */
          req->invalid = DBT_INVALID_SELECT;
          strcpy (req->invalid_detail, "select");
          return;
        }
      else
        {
          req->invalid = DBT_INVALID_KEY;
          set_detail (req, key);
          return;
        }

      skip_ws (&sc);
      {
        int c = next (&sc);

        if (c == ',')
          continue;
        if (c == '}')
          break;
        req->invalid = DBT_INVALID_JSON;
        return;
      }
    }

  /* Nothing but whitespace may follow the object. */
  skip_ws (&sc);
  if (sc.pos != sc.len)
    {
      req->invalid = DBT_INVALID_LINE;
      return;
    }

  if (have_status)
    {
      if (table_name != NULL || have_row)
        {
          req->invalid = DBT_INVALID_KEY;
          strcpy (req->invalid_detail, "status");
          return;
        }
      req->kind = DBT_REQ_STATUS;
      return;
    }

  if (!have_row || table_name == NULL)
    {
      req->invalid = DBT_INVALID_TABLE;
      return;
    }

  /* "table" decides the binding; every table has its own row shape
   * (PLAN.md section 6), and unknown tables are refused with the
   * offending name as detail. */
  if (strcmp (table_name, "runs") == 0)
    {
      req->table = DBT_TABLE_RUNS;
      if (bind_runs (fields, nfields, req) < 0)
        return;
    }
  else if (strcmp (table_name, "run_instances") == 0)
    {
      req->table = DBT_TABLE_RUN_INSTANCES;
      if (bind_instances (fields, nfields, req) < 0)
        return;
    }
  else if (strcmp (table_name, "training_data") == 0)
    {
      req->table = DBT_TABLE_TRAINING_DATA;
      if (bind_training (fields, nfields, req) < 0)
        return;
    }
  else
    {
      req->invalid = DBT_INVALID_TABLE;
      return;
    }

  req->kind = DBT_REQ_INSERT;
}

/* ---------------------------------------------------------------------
 *  Response builders
 * ------------------------------------------------------------------- */

size_t
dbt_response_ok (char *buf, size_t cap, long long id)
{
  int n = snprintf (buf, cap, "{\"ok\": true, \"id\": %lld}", id);

  return n < 0 || (size_t) n >= cap ? 0 : (size_t) n;
}

size_t
dbt_response_duplicate (char *buf, size_t cap, long long id)
{
  int n = snprintf (buf, cap, "{\"duplicate\": %lld}", id);

  return n < 0 || (size_t) n >= cap ? 0 : (size_t) n;
}

size_t
dbt_response_invalid (char *buf, size_t cap,
                      enum dbt_invalid reason, const char *detail)
{
  const char *r = detail != NULL && detail[0] != '\0'
                    ? detail : dbt_invalid_reason (reason);
  int n = snprintf (buf, cap, "{\"invalid\": \"%s\"}", r);

  return n < 0 || (size_t) n >= cap ? 0 : (size_t) n;
}

size_t
dbt_response_empty (char *buf, size_t cap)
{
  int n = snprintf (buf, cap, "{\"empty\": true}");

  return n < 0 || (size_t) n >= cap ? 0 : (size_t) n;
}

size_t
dbt_json_number (char *buf, size_t len, size_t cap,
                 const char *key, long long value)
{
  size_t room = cap > len ? cap - len : 0;
  int n = snprintf (buf + len, room, "\"%s\": %lld", key, value);

  return n < 0 || (size_t) n >= room ? len : len + (size_t) n;
}
