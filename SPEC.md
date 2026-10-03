<!--
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Data Base Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# data-base-translator — SPEC (contrat tel qu'implémenté, phase 1)

Ce document reprend, tel qu'implémenté, le contrat gelé par le
`PLAN.md` (sections 3.3, 5 phase 0–1 et 6). Il est la référence
croisée pour `orchestrator-translator` (tables d'orchestration) et
`inference-translator` (registre de clés SSH, phase 5).

## 1. Montage

```
settrans -a /db db-translator --conninfo "dbname=gnuai"
```

- `--conninfo STRING` : la chaîne `conninfo` libpq (dbname, host,
  port, user, password, connect_timeout…) ; c'est le seul argument
  de connexion, jamais de socket ouverte par l'appelant.
- Au démarrage : connexion libpq, création **idempotente** du schéma
  (`CREATE TABLE IF NOT EXISTS`, texte de `schema.sql`), une
  connexion unique avec un jeu fixe de requêtes préparées (une par
  table et par opération du contrat).
- Un échec de connexion n'est pas fatal : le translator reste monté
  et lisible ; chaque écriture retente la connexion, seule la
  couche transport peut échouer (`EIO`).

## 2. Le nœud `/db` en phase 1 (trivfs)

Le MVP est un translator `trivfs` : un seul nœud, monté sur `/db`,
qui parle le protocole « une ligne JSON en, une ligne JSON en
réponse ». L'arborescence complète gelée (`/db/status`,
`/db/schema`, `/db/runs/<id>`, …) est servie par le même contrat
dès que la navigation par identifiant impose `netfs` (phase 2+) ;
en phase 1, chaque rôle de l'arborescence s'obtient par le protocole :

| Rôle | En phase 1 (trivfs, un nœud) | En netfs (phase 2+) |
|---|---|---|
| État et diagnostic | `read` de `/db` au montage, ou `write` de `{"status": true}` puis `read` | `cat /db/status` |
| Insertion | `write` de `{"table": …, "row": …}` puis `read` de la réponse | `write` sur `/db/<table>` |
| Requête (phase 2) | `write` de `{"select": …}` puis `read` | `cat /db/<table>/<id>` |

Un lecteur ne voit jamais la réponse d'un autre : chaque fichier
ouvert porte son propre curseur de lecture ; la réponse à la
dernière instruction terminée est la donnée servie.

## 3. Écritures (gelées)

Une instruction = **une ligne JSON complète** (terminée par `\n`),
un objet unique par ligne :

```json
{"table": "runs", "row": {"descriptor": {"instances": 3}, "aggregate_strategy": "majority"}}
{"table": "run_instances", "row": {"run_id": 42, "topology": "10,20,5", "input": [0.5], "status": "ok"}}
{"table": "training_data", "row": {"source_url": "https://…", "http_status": 200, "content": "…", "checksum": "64 hex chars"}}
{"status": true}
```

Champs par table (les types réfèrent JSON) :

- **`runs`** : `descriptor` (objet, requis, servi brut à `jsonb`),
  `aggregate_strategy` (chaîne, requis), `final_output`
  (objet, optionnel).
- **`run_instances`** : `run_id` (entier, requis), `topology`
  (chaîne, requis), `input` (objet ou tableau, requis, brut),
  `status` (chaîne, requis) ; optionnels et nullable : `seed`
  (entier), `output` (objet ou tableau), `score` (nombre).
- **`training_data`** : `source_url` (chaîne, requis),
  `content` (chaîne, requise), `checksum` (chaîne de 64
  caractères hexadécimaux minuscules, requise) ; `http_status`
  (entier, optionnel, nullable).

Règles :

- Un champ inconnu, un type inattendu ou une clé `row` manquante
  rendent la ligne `invalid` (réponse lisible, jamais une erreur
  POSIX fatale) ; `checksum` est vérifié (longueur, hexadécimal)
  par le translator avant d'atteindre la base.
- `null` et l'absence sont équivalents pour les champs optionnels.
- Ligne non terminée par `\n` ou trop longue : réponse `invalid`,
  tampon remis à zéro. La limite en phase 1 est de 256 Kio par
  instruction (les gros contenus paginés arrivent en phase 3).
- `"select"` est reconnu mais refuse en phase 1 (`invalid`) : les
  lectures arrivent en phase 2.

## 4. Réponses (gelées)

Toujours une ligne JSON, lisible par `read` :

| Réponse | Signification |
|---|---|
| `{"ok": true, "id": 42}` | Insertion effectuée, identifiant attribué par la base. |
| `{"duplicate": 17}` | Contrainte `UNIQUE` sur `checksum` : l'identifiant existant est rendu, l'appelant décide de la politique. |
| `{"invalid": "checksum"}` | Ligne rejetée par le contrat (aussi : `line`, `json`, `table`, `field`, `type`, `key`, `run_id`, `descriptor`). |
| `{"empty": true}` | Requête sans résultat (phase 2). |
| `{"status": …}` | État : `{"connected": …, "server_version": …, "schema": …, "libpq": …, "writes": {…}}`. |

- Les contraintes **attendues** de la base (doublon, foreign key,
  JSON malformé) sont des statuts lisibles (`duplicate`,
  `invalid`), pas des erreurs POSIX.
- Seuls les échecs de transport (serveur injoignable, connexion
  perdue) remontent en `EIO` au `write` ; le translator retente
  la connexion au coup suivant et ne meurt jamais.

## 5. Statut

`read` au montage, ou instruction `{"status": true}` :

```json
{"connected": true, "server_version": 170002, "schema": true, "libpq": true,
 "writes": {"runs": 0, "run_instances": 0, "training_data": 0, "duplicates": 0, "invalid": 0}}
```

Les compteurs sont ceux du translator depuis son montage. `libpq`
vaut `false` quand le binaire a été compilé sans le driver (build
de vérification) : toute écriture répond alors `EIO` (transport),
le statut reste lisible.

## 6. Mode vérification (non-Hurd)

Sur un POSIX sans Hurd, le même binaire lit des lignes sur
l'entrée standard et imprime les réponses sur la sortie standard
(erreurs de transport sur `stderr`, code de sortie 1) : le contrat
est testable partout, exactement comme le cœur de
`neuron-translator`. C'est ce mode qu'utilisent les tests
`test_protocol.sh` et `test_pg.sh`.

## 7. Limites assumées en phase 1

- Une seule connexion libpq (PLAN section 4) ; le pool de
  connexions n'arrivera que si plusieurs translators concurrents
  l'exigent, mesuré avant d'être construit.
- Le serveur trivfs traite les RPC dans un fil unique : pas d'état
  mutable partagé sans verrou, un curseur par fichier ouvert et
  par lecteur.
- `incidents`, `users`, `access_keys`, `auth_failures` sont créés
  au montage (schéma gelé entier) mais pas encore addressables en
  écriture : phases 2 et 5.
