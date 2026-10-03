<!--
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Data Base Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# data-base-translator

The **persistence layer** of the GNU AI stack for GNU/Hurd: it
exposes a PostgreSQL database as a filesystem, so the other
translators — first of all
[orchestrator-translator](https://github.com/gnu-ai/orchestrator-translator) —
write and read their data (training data, runs, incidents, the SSH
key registry of the remote mode of
[inference-translator](https://github.com/gnu-ai/inference-translator))
by plain POSIX writes and reads, never by linking a SQL client
library or opening a socket.

License: GPLv3+ — Language: C23, POSIX.1-2008, Hurd `trivfs`,
PostgreSQL client via `libpq` only. The project and roadmap live in
[PLAN.md](PLAN.md); the frozen contract as implemented lives in
[SPEC.md](SPEC.md); the schema in [schema.sql](schema.sql).

## Build

```console
$ ./autogen.sh            # autoreconf, once
$ mkdir build && cd build && ../configure
$ make
$ make check
```

`configure` looks for `libpq` with pkg-config. Without it the binary
still builds: the status line stays readable and honest
(`"libpq": false`), every write fails as `EIO` — the contract stays
testable everywhere, the driver only matters for real persistence.

## Mount (GNU/Hurd)

```console
# settrans -a /db db-translator --conninfo "dbname=gnuai"
# cat /db
{"connected": true, "server_version": 170002, "schema": true, "libpq": true,
 "writes": {"runs": 0, "run_instances": 0, "training_data": 0, "duplicates": 0, "invalid": 0}}
```

At mount the translator connects, creates the frozen schema
idempotently (`CREATE TABLE IF NOT EXISTS`, never destructive) and
prepares the fixed statement set. A failed connection is not fatal:
the node stays readable, each write retries the connection.

## Use: one instruction line, one response line

```console
# echo '{"table": "runs", "row": {"descriptor": {"instances": 3}, "aggregate_strategy": "majority"}}' | tee /db
# cat /db
{"ok": true, "id": 1}

# echo '{"table": "training_data", "row": {"source_url": "https://example.org/", "http_status": 200, "content": "…", "checksum": "<64 hex>"}}' | tee /db
# cat /db
{"ok": true, "id": 1}

# echo '{"table": "training_data", "row": {"source_url": "…", "content": "…", "checksum": "<the same>"}}' | tee /db
# cat /db
{"duplicate": 1}
```

`read` serves the response to the last completed instruction with one
cursor per reader; at mount (or after `{"status": true}`) it serves
the status line. Expected constraints are statuses (`duplicate`,
`invalid`), only the transport layer fails as `EIO`. The complete
line protocol — fields per table, reason tokens, limits — is
specified in [SPEC.md](SPEC.md).

## Verification mode (any POSIX system)

On a system without Hurd, the same binary runs the same engine over
standard streams:

```console
$ echo '{"table": "runs", "row": {"descriptor": {}, "aggregate_strategy": "majority"}}' \
    | src/db-translator --conninfo "dbname=gnuai"
```

This is what the test suite exercises.

## Tests

`make check` runs, in order:

- `test_contract` — unit tests of the line contract (87 checks):
  parser, per-table bindings, checksum rules, escapes (`\uXXXX`
  becomes UTF-8), response builders, and the byte-for-byte sync
  between the embedded DDL and `schema.sql`;
- `test_cli.sh` — GNU `--version`/`--help` conformance;
- `test_protocol.sh` — end-to-end over the verification binary,
  without any server;
- `test_pg.sh` — writes against a **disposable PostgreSQL cluster**
  created and destroyed by the suite itself (full run scenario,
  duplicate status, foreign key status), skipped (77) when the
  PostgreSQL tools are not installed.

Home made harness (`CHECK` macros), zero external framework, `make
check` is the standard target, as required by the acceptance
criteria of every phase.

## Status

- Phase 1 — MVP: schema + writes: **done** (`runs`,
  `run_instances`, `training_data`, `/db/status`, duplicate as a
  status, disposable-cluster integration test).
- Phase 2 — incidents + reads: not started.
- Phase 5 — SSH key registry: not started.

See [PLAN.md](PLAN.md) for the phase gates and the ToDo of the whole
stack.
