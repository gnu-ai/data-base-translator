-- SPDX-License-Identifier: GPL-3.0-or-later
-- SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
--
-- Frozen schema of the GNU AI persistence layer (PLAN.md section 6).
-- Created idempotently by data-base-translator at mount: every table
-- is CREATE TABLE IF NOT EXISTS, nothing is ever dropped or altered.
--
-- This file is the single source of truth shared with
-- orchestrator-translator (orchestration tables) and
-- inference-translator (SSH key registry).  The same text is embedded
-- in src/storage.c; the test suite checks that both copies match
-- exactly, so a unilateral edit on either side fails `make check`.

-- Training data fetched from the network
CREATE TABLE IF NOT EXISTS training_data (
    id          BIGSERIAL PRIMARY KEY,
    source_url  TEXT NOT NULL,
    fetched_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    http_status INT,
    content     TEXT NOT NULL,
    checksum    CHAR(64) NOT NULL UNIQUE      -- SHA-256, anti-duplicate
);

-- Each orchestrated run
CREATE TABLE IF NOT EXISTS runs (
    id          BIGSERIAL PRIMARY KEY,
    started_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    descriptor  JSONB NOT NULL,               -- task descriptor
    final_output JSONB,
    aggregate_strategy TEXT NOT NULL
);

-- One row per neuron-translator instance in a run
CREATE TABLE IF NOT EXISTS run_instances (
    id          BIGSERIAL PRIMARY KEY,
    run_id      BIGINT NOT NULL REFERENCES runs(id),
    topology    TEXT NOT NULL,                -- e.g. "10,20,5"
    seed        BIGINT,
    input       JSONB NOT NULL,
    output      JSONB,
    score       REAL,
    status      TEXT NOT NULL                 -- ok | failed | timeout
);

-- Failure history (supervisor)
CREATE TABLE IF NOT EXISTS incidents (
    id          BIGSERIAL PRIMARY KEY,
    run_id      BIGINT REFERENCES runs(id),
    instance_id BIGINT REFERENCES run_instances(id),
    kind        TEXT NOT NULL,                -- crash | timeout | restart
    detected_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Users allowed in the remote mode (inference-translator)
CREATE TABLE IF NOT EXISTS users (
    id          BIGSERIAL PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Nominative public SSH keys (inference-translator): a public key is
-- not a secret, it lives in clear; the SHA-256 fingerprint carries
-- uniqueness
CREATE TABLE IF NOT EXISTS access_keys (
    id          BIGSERIAL PRIMARY KEY,
    user_id     BIGINT NOT NULL REFERENCES users(id),
    key_pub     TEXT NOT NULL,                -- public SSH key
    fingerprint CHAR(64) NOT NULL UNIQUE,    -- SHA-256 of the key
    label       TEXT,                         -- e.g. "Claire's laptop"
    issued_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    revoked_at  TIMESTAMPTZ                   -- NULL = active key
);

-- Journal of refused SSH authentication attempts (inference)
CREATE TABLE IF NOT EXISTS auth_failures (
    id          BIGSERIAL PRIMARY KEY,
    fingerprint CHAR(64),                     -- NULL if invalid key
    origin      TEXT,                         -- origin address
    refused_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
