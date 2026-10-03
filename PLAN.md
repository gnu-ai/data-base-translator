<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Data Base Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# Data Base Translator — Projet et feuille de route

`data-base-translator` est la **couche de persistance** de la pile
GNU AI pour GNU/Hurd. Il ne fait aucun calcul neuronal lui-même : il
expose une base PostgreSQL comme un système de fichiers Hurd, afin
que les autres translators — en premier lieu
[orchestrator-translator](https://github.com/gnu-ai/orchestrator-translator)
— écrivent et relisent leurs données (données d'entraînement,
exécutions, résultats, incidents, registre des clés publiques SSH du
mode distant
d'[inference-translator](https://github.com/gnu-ai/inference-translator))
par de simples `write`/`read` POSIX, sans jamais lier une
bibliothèque cliente SQL ni ouvrir de socket vers le serveur.

Licence : GPLv3 ou version ultérieure. Langage : C23, POSIX.1-2008,
interfaces Hurd (`trivfs`), client PostgreSQL via `libpq`.

---

## 1. Rôle et positionnement

L'esprit Hurd est respecté : chaque responsabilité reste dans un
translator dédié, la persistance ne fait que **stocker et servir**.

| Composant | Responsabilité | Lien |
|---|---|---|
| `orchestrator-translator` | coordination : scheduler, supervisor, evaluator, aggregator ; écrit et lit via `/db` | gnu-ai/orchestrator-translator |
| `neuron-translator` | unité de calcul : réseau sigmoïde feedforward, piloté par POSIX | gnu-ai/neuron-translator |
| `inference-translator` | interface de dialogue : prompts, requêtes structurées, mode distant authentifié par clé SSH | gnu-ai/inference-translator |
| `httpfs-translator` | transport pur HTTP → système de fichiers (`content`, `headers`, `status`) | gnu-ai/httpfs-translator |
| `data-base-translator` | persistance PostgreSQL : données d'entraînement, exécutions, résultats, historique, clés SSH | ce dépôt |

### Ce que ce translator garantit aux autres

- **Un seul point d'entrée SQL** : `data-base-translator` est
  l'unique composant de la pile autorisé à parler à PostgreSQL. Les
  autres ne connaissent que `/db` et son contrat d'échange.
- **Durabilité dès la première exécution** : toute donnée écrite par
  l'orchestrateur est persistée immédiatement ; aucun chemin "sans
  BDD" ne peut se creuser dans la pile.
- **Rejouabilité** : ce qui a été acquis sur le réseau ou calculé
  peut être relu plus tard, à l'identique, réseau coupé.
- **Clés privées jamais vues** : le registre du mode distant
  d'`inference-translator` ne stocke que des **clés publiques SSH**
  et leurs empreintes ; une clé privée ne traverse jamais `/db`.

### Ce qu'il ne fait pas

- Pas de logique d'orchestration, d'évaluation ni d'agrégation : il
  stocke des scores, il n'en calcule pas.
- Pas d'interprétation des contenus : un `content` HTTP est un
  `TEXT` opaque, y compris une page d'erreur 404.
- Pas de décision sur les données : un doublon (`checksum`) est
  signalé à l'appelant, jamais silencieusement écrasé ni ignoré.
- Pas de décision d'autorisation : il sert le registre des clés
  publiques ; c'est sshd (via `authorized_keys` généré depuis le
  registre) et le serveur `inference` qui acceptent ou refusent la
  connexion.

---

## 2. Fonctionnalités principales

1. **Translator monté sur `/db`** : pilotable par les commandes POSIX
   (`settrans`, `write`, `read`), interrogeable avec `cat`.
2. **Création idempotente du schéma** : au montage, les tables
   `training_data`, `runs`, `run_instances`, `incidents`,
   `users`, `access_keys` et `auth_failures` (section 6)
   sont créées si absentes, sans jamais détruire des données
   existantes.
3. **Écriture d'enregistrements** : une ligne JSON soumise par
   `write` devient une ligne de table (`insert into training_data`,
   `insert into runs`, …) via requêtes préparées.
4. **Lecture de requêtes** : une requête soumise par `write` (filtres,
   tris, pagination) produit un flux de lignes JSON lisible par
   `read` : jeux d'entraînement, historique d'exécutions, incidents.
5. **Anti-doublon SHA-256** : la contrainte `UNIQUE` sur
   `training_data.checksum` est exposée à l'appelant comme un statut
   lisible (`duplicate`), pas comme une erreur POSIX fatale.
6. **Navigation par arborescence** : `/db/runs/<id>`,
   `/db/runs/<id>/instances`, `/db/training_data/<id>`,
   `/db/users`, `/db/access_keys` … lisibles directement, en
   miroir des tables SQL.
7. **État et diagnostic** : `/db/status` (connexion au serveur,
   version du schéma, compteurs), `/db/schema` (DDL en lecture).
8. **Registre des clés SSH** : enregistrement des clés **publiques**
   SSH nominatives du mode distant d'`inference-translator`,
   empreintes SHA-256 anti-doublon, révocation, et journal des
   tentatives d'authentification refusées (`auth_failures`).

---

## 3. Architecture et flux de données

### 3.1 Vue d'ensemble

```
        ┌──────────────────────────────────────┐
        │       orchestrator-translator        │
        │  scheduler / supervisor /           │
        │  evaluator / aggregator             │
        └───┬────────────────────────────▲───┘
            │ write (enregistrements,      │ read (requêtes,
            │      requêtes)               │      résultats)
            ▼                             │
        ┌───────────────────────┐
        │  data-base-translator │  /db : POSIX ↔ SQL
        │  trivfs / netfs       │
        └──────────┬────────────┘
                   │ libpq (préparé, une connexion
                   │ par translator, pas par requête)
                   ▼
             ┌────────────┐
             │ PostgreSQL │
             └────────────┘
```

### 3.2 Flux nominal d'un enregistrement

1. L'orchestrateur monte `data-base-translator` sur `/db` (par ex.
   `settrans /db /hurd/db-translator --conninfo "dbname=gnuai"`).
2. Au démarrage, le translator vérifie la connexion, crée ou
   réutilise le schéma (section 6), expose `/db/status`.
3. L'orchestrateur écrit une ligne JSON :
   `{ "table": "runs", "row": { "descriptor": …,
   "aggregate_strategy": "majority" } }`.
4. Le translator traduit la ligne en requête préparée, l'exécute,
   et rend visible l'identifiant attribué :
   `{ "ok": true, "id": 42 }`.
5. Pour relire : l'orchestrateur écrit une requête
   (`{ "select": "run_instances", "where": { "run_id": 42 } }`)
   puis lit le résultat ligne par ligne, en JSON.

### 3.2 bis Flux nominal d'une clé SSH (mode distant d'inference)

1. L'opérateur du cluster enregistre la clé **publique** SSH d'un
   utilisateur : il écrit une ligne JSON sur `/db` :
   `{ "table": "access_keys", "row": { "user": "claire",
   "key_pub": "ssh-ed25519 AAAA…", "label": "portable" } }`.
2. Le translator calcule l'empreinte SHA-256 de la clé publique,
   l'écrit avec `key_pub` (l'unicité porte sur `fingerprint`) et
   rend l'identifiant attribué : `{ "ok": true, "id": 7 }`.
   Une clé publique n'est pas un secret : elle est stockée en clair.
3. Le serveur `inference` lit le registre actif
   (`{ "select": "access_keys", "where": { "revoked": false } }`)
   et **génère `authorized_keys` depuis ce registre** — seule
   source de vérité des accès distants ; sshd authentifie
   l'utilisateur à la connexion SSH.
4. Chaque échec d'authentification SSH est journalisé par le
   serveur `inference` :
   `{ "table": "auth_failures", "row": { "fingerprint": "…",
   "origin": "10.0.0.4" } }`.
5. Révoquer, c'est écrire une date : `{ "revoke": "access_keys",
   "id": 7 }` positionne `revoked_at` — puis re-générer
   `authorized_keys` ; jamais une suppression : l'audit reste
   possible.

### 3.3 Contrats d'interface (principe clé)

Le contrat `orchestrator → database` est gelé en phase 0 côté
orchestrateur ; le contrat `inference → database` (registre de
clés SSH) est gelé en phase 0 côté `inference-translator`. Ce dépôt
les implémente tels quels. Chaque interaction passe par le système
de fichiers, jamais par des sockets côté appelant, ni d'API
propriétaire :

| Contract | Échange |
|---|---|
| `appelant → /db` (écriture) | `write` d'une ligne JSON : insertion (`table` + `row`) ou requête (`select` + filtres). |
| `/db → appelant` (lecture) | `read` du résultat : lignes JSON (une par enregistrement) ou statut (`{ "ok": … }`, `{ "duplicate": … }`). |
| `appelant → /db` (navigation) | `read` direct de `/db/runs/<id>`, `/db/training_data/<id>`, `/db/incidents`, `/db/users`, `/db/access_keys`, … sans requête préalable. |
| `montage` | `settrans` de `/db` avec la chaîne `conninfo` libpq en argument ; nœuds gelés : `/db/status`, `/db/schema`, `/db/training_data`, `/db/runs`, `/db/run_instances`, `/db/incidents`, `/db/users`, `/db/access_keys`, `/db/auth_failures`. |
| `inference → /db` (clés) | `write` de l'enregistrement d'une clé publique SSH (`user` + `key_pub`), d'une révocation ou d'un échec d'authentification SSH ; `read` du registre actif pour générer `authorized_keys`. |

---

## 4. Décisions de conception

### Pourquoi un translator, et non une bibliothèque liée à l'orchestrateur ?

Parce que chaque responsabilité reste dans un translator dédié, et
que l'orchestrateur ne doit connaître ni les binaires ni les
détails d'implémentation de ses voisins. Un translator monté sur
`/db` apporte trois garanties qu'une bibliothèque n'a pas :

- **remplaçabilité** : le serveur PostgreSQL (hôte, version,
  base) peut changer sans recompiler l'orchestrateur — seul
  `/db` et sa `conninfo` bougent ;
- **partage** : plusieurs translators écrivent dans la même base
  sans dépendre d'un format de liaison ;
- **inspection** : l'état de la persistance est lisible par
  l'utilisateur avec `cat`, comme tout le reste de la pile.

### Zéro allocation dans les chemins chauds — et le driver SQL ?

La discipline de `neuron-translator` et de l'orchestrateur
s'applique, adaptée au driver : les requêtes préparées sont
**allouées une fois pour toutes au montage** (schéma connu et gelé),
les tampons de lignes JSON sont des zones contiguës pré-allouées,
et seules les réponses volumineuses (`content` de
`training_data`) sont servies par pages, avec un curseur de lecture.
Aucune allocation par ligne en lecture, aucune par enregistrement en
écriture.

### Erreurs SQL ≠ erreurs POSIX

Dans l'esprit `httpfs` ("un statut non-200 n'est pas une erreur
POSIX"), les contraintes **attendues** de la base sont des données
exploitables, pas des échecs : un doublon de `checksum` est rendu
comme `{ "duplicate": <id existant> }` et l'appelant décide de la
politique. Seuls les échecs de la couche transport (serveur
injoignable, connexion perdue) remontent comme `EIO` — et dans ce
cas le translator retente la connexion et redevient lisible, il ne
meurt pas.

### Verrou d'unicité des contenus

L'empreinte SHA-256 est calculée par le **client** (l'orchestrateur
possède déjà le contenu) et fournie dans la ligne JSON ; le
translator ne fait que la vérifier (longueur, hexadécimal) et la
confier à la contrainte `UNIQUE`. Le translator reste sans
connaissance des contenus.

### Clés SSH : publiques par nature, privées jamais vues

Le registre ne stocke que des **clés publiques** SSH — ce ne sont
pas des secrets : elles vivent en clair, avec une empreinte SHA-256
(`fingerprint`) qui porte l'unicité et le journal des échecs. La
partie privée ne quitte jamais la machine de l'utilisateur et ne
traverse jamais `/db`. La révocation est une date (`revoked_at`),
pas une suppression — l'audit reste possible. Le translator ne
décide pas de l'autorisation : il sert le registre ; sshd
authentifie via `authorized_keys` généré depuis ce registre, le
serveur `inference` décide du reste.

### Une connexion, des requêtes préparées

Le MVP maintient **une seule connexion libpq** avec un jeu fixe de
requêtes préparées (une par table et par type d'opération du
contrat). Le pool de connexions n'arrive que si plusieurs
translators concurrents l'exigent — mesuré avant d'être construit.

### Le cluster dès la phase 3 : concurrence mesurée, registre anticipé

Dès la phase 3, plusieurs nœuds d'un cluster orchestrateur
écrivent et lisent **simultanément** dans `/db`. Trois décisions :

1. Le **verrou sur la connexion libpq** est le mécanisme par
   défaut ; le pool n'arrive que si les mesures l'exigent. Le
   cluster ne force pas le pool — il le rend **mesurable**.
2. Les tables `users` et `access_keys` sont créées dès la phase 3
   (au lieu de la phase 5), parce que l'authentification des
   nœuds et des utilisateurs du cluster en a besoin dès le
   premier nœud distant. Leur exploitation complète
   (synchronisation `authorized_keys`, journal `auth_failures`)
   reste en phase 5.
3. Aucune écriture ne doit laisser d'état partiel visible, quel
   que soit le nombre d'écrivants — c'est la **transaction SQL**
   qui le garantit, pas la topologie du cluster.

---

## 5. Phases

Chaque phase a un livrable, des critères d'acceptation et une
dépendance explicite sur la précédente. Le schéma SQL (section 6)
est **gelé côté orchestrateur en phase 0** pour les tables
d'orchestration, et **gelé côté inference-translator en phase 0**
pour le registre de clés SSH ; ce dépôt les implémente tels quels
et ne les modifie pas sans revue conjointe.

### Phase 0 — Spécification et contrats (avant tout code)

- Reprise du contrat `orchestrator → database` gelé par
  `orchestrator-translator` (section 3.3) ; du même coup, gel du
  format d'échange : lignes JSON simples, une instruction par
  ligne, une réponse par ligne.
- Reprise du contrat `inference → database` (registre des clés
  SSH du mode distant) proposé par `inference-translator`
  (sa phase 0) : tables `users`, `access_keys`,
  `auth_failures` (section 6), opérations d'enregistrement de clé
  publique, de synchronisation `authorized_keys`, de révocation et
  de journalisation des échecs SSH.
- Gel de l'arborescence `/db` : `status`, `schema`,
  `training_data`, `runs`, `run_instances`, `incidents`,
  `users`, `access_keys`, `auth_failures`, puis
  navigation par identifiant.
- Implémentation de référence du schéma SQL de la section 6 (fichier
  `schema.sql`, source de vérité partagée avec l'orchestrateur et
  l'interface).
- Convention d'arguments de montage : `conninfo` libpq, options de
  migration (`create`, `verify`, `readonly`).
- **Livrable** : `SPEC.md` + squelette de code compilable.
- **Acceptation** : revue croisée du contrat avec
  `orchestrator-translator` — notamment que le périmètre minimal
  requis par sa phase 1 (création du schéma + écriture de `runs` et
  `run_instances`) est couvert sans réserve — et revue croisée du
  registre de clés SSH avec `inference-translator` — le périmètre
  requis par sa phase 5 (enregistrement, synchronisation
  `authorized_keys`, révocation, journal des échecs) est couvert
  sans réserve.

### Phase 1 — MVP : schéma + écritures

- Connexion libpq au montage, création idempotente du schéma
  (`CREATE TABLE IF NOT EXISTS`), `/db/status` lisible.
- Écriture de `runs`, `run_instances` et `training_data` via
  requêtes préparées, avec retour de l'identifiant attribué.
- Signalement du doublon `checksum` comme statut `duplicate`.
- **Livrable** : `data-base-translator` compilable sous Hurd, monté
  sur `/db`, capable d'enregistrer une exécution complète de
  l'orchestrateur.
- **Acceptation** : la phase 1 d'`orchestrator-translator`
  (exécution avec 2 et 8 instances de `neuron-translator`) persiste
  ses `runs`/`run_instances` via ce translator, et les lignes sont
  visibles côté SQL ; `make check` vert.

### Phase 2 — Incidents et lectures

- Écriture de `incidents` (crash, timeout, restart) — le supervisor
  de la phase 2 de l'orchestrateur persiste chaque incident dès sa
  détection.
- Requêtes en lecture : `select` avec filtres simples et tri
  (`where`, `order`), flux de lignes JSON en `read`.
- `/db/runs/<id>` et `/db/runs/<id>/instances` navigables
  directement.
- **Livrable** : contrat `read` complet du gel de phase 0.
- **Acceptation** : l'historique d'une exécution survivante à une
  perte d'instance (phase 2 orchestrateur) est relu intégralement
  depuis `/db` : le run, ses instances, l'incident.

### Phase 3 — Acquisition réseau servie : `training_data`

- Support des gros contenus : écriture et lecture paginées du champ
  `content`, curseur de lecture POSIX, empreinte vérifiée.
- Archivage des contenus récupérés via `httpfs-translator`
  (y compris les 404, avec `http_status`) avant leur utilisation.
- Requêtes par plage de date, par statut HTTP, par absence en base
  (`checksum` connu ?) pour permettre l'anti-doublon côté
  orchestrateur **avant** téléchargement.
- **Mode cluster dès cette phase** : écritures et lectures
  concurrentes de plusieurs nœuds d'un cluster — verrou sur la
  connexion libpq partagée, pool de connexions si les mesures
  l'exigent, une vue cohérente par lecteur. Création anticipée
  des tables `users` et `access_keys` (section 6) dès cette
  phase, pour l'authentification des nœuds et des utilisateurs
  du cluster ; leur exploitation complète (synchronisation
  `authorized_keys`, journal `auth_failures`) reste en phase 5.
- **Livrable** : une tâche complète de la phase 3 orchestrateur
  ("prompt → URL → contenu → N réseaux → résultat") archivée et
  relue uniquement via `/db`.
- **Acceptation** : les deux traces d'une démonstration orchestrateur
  (URL réelle + URL en 404) sont présentes dans `training_data`,
  relues depuis `/db` sans accès réseau. La démonstration tourne
  sur un cluster de **deux nœuds** écrivant simultanément dans
  `/db`, sans état partiel visible.

### Phase 4 — Rejouabilité : exports et historique

- Export d'un jeu d'entraînement archivé sous forme de flux de
  vecteurs d'entrée rejouables (`replay` de la phase 4
  orchestrateur, réseau coupé).
- Historique et statistiques : performances par topologie calculées
  **en SQL** à partir de `run_instances` (le translator agrège des
  lignes, il n'évalue pas), servies comme lignes JSON.
- Pagination et limites de mémoire bornées sur toutes les lectures.
- **Livrable** : commandes d'export et d'historique du contrat.
- **Acceptation** : rejouer une tâche de la phase 3 orchestrateur à
  partir des seules données lues via `/db`.

### Phase 5 — Registre des clés SSH (mode distant d'inference-translator)

- Écriture de `users` et `access_keys` : enregistrement d'une clé
  **publique** SSH nominative, empreinte SHA-256 calculée par le
  translator, unicité sur `fingerprint`.
- Lecture du registre actif par le serveur `inference` pour
  **générer `authorized_keys`** — le registre est la seule source
  de vérité des accès distants.
- Révocation par simple écriture d'une date (`revoked_at`) puis
  re-génération de `authorized_keys` : révoquer un utilisateur ne
  touche ni ses autres clés, ni les autres utilisateurs ; l'audit
  reste complet.
- Journal des échecs d'authentification SSH : chaque connexion
  refusée devient une ligne `auth_failures` (empreinte soumise,
  origine, horodatage).
- Navigation : `/db/users`, `/db/access_keys` lisibles
  directement.
- **Livrable** : registre de clés complet, servi uniquement via
  `/db`.
- **Acceptation** : une session SSH authentifiée par une clé du
  registre accède au serveur `inference` (phase 5
  d'`inference-translator`) ; une clé révoquée ou inconnue est
  refusée, chaque refus est journalisé, et aucune clé privée
  n'apparaît dans la base.

### Phase 6 — Durcissement, tests, CI

- Reconnexion automatique au serveur PostgreSQL, comportement en
  lecture seule documenté, requêtes bornées (limites imposées aux
  filtres), journalisation des échecs transport.
- Suite de tests déterministes : schéma embarqué sur instance
  PostgreSQL jetable (conteneur ou VM), tests de doublons, de
  pagination, de reconnexion, d'enregistrement/révocation de clés
  SSH, tests de charge (des dizaines de milliers de lignes).
- CI sous QEMU GNU/Hurd, pilotée par le sandbox
  [gnu-ai/mistral-vm-debian-hurd](https://github.com/gnu-ai/mistral-vm-debian-hurd).
- Documentation utilisateur et architecture (`docs/architecture.md`).
- **Livrable** : version 1.0.

---

## 6. Schéma PostgreSQL (gelé côté orchestrateur et inference, implémenté ici)

Ce schéma est la source de vérité partagée avec
`orchestrator-translator` (sa section 6) pour les tables
d'orchestration, et avec `inference-translator` (sa section 3.5)
pour le registre de clés SSH. Toute évolution est décidée en revue
conjointe, jamais unilatéralement.

```sql
-- Données d'entraînement récupérées sur le net
CREATE TABLE IF NOT EXISTS training_data (
    id          BIGSERIAL PRIMARY KEY,
    source_url  TEXT NOT NULL,
    fetched_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    http_status INT,
    content     TEXT NOT NULL,
    checksum    CHAR(64) NOT NULL UNIQUE      -- SHA-256, anti-doublon
);

-- Chaque exécution orchestrée
CREATE TABLE IF NOT EXISTS runs (
    id          BIGSERIAL PRIMARY KEY,
    started_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    descriptor  JSONB NOT NULL,               -- descripteur de tâche
    final_output JSONB,
    aggregate_strategy TEXT NOT NULL
);

-- Une ligne par instance de neuron-translator dans une exécution
CREATE TABLE IF NOT EXISTS run_instances (
    id          BIGSERIAL PRIMARY KEY,
    run_id      BIGINT NOT NULL REFERENCES runs(id),
    topology    TEXT NOT NULL,                -- ex. "10,20,5"
    seed        BIGINT,
    input       JSONB NOT NULL,
    output      JSONB,
    score       REAL,
    status      TEXT NOT NULL                 -- ok | failed | timeout
);

-- Historique des défaillances (supervisor)
CREATE TABLE IF NOT EXISTS incidents (
    id          BIGSERIAL PRIMARY KEY,
    run_id      BIGINT REFERENCES runs(id),
    instance_id BIGINT REFERENCES run_instances(id),
    kind        TEXT NOT NULL,                -- crash | timeout | restart
    detected_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Utilisateurs autorisés au mode distant (inference-translator)
CREATE TABLE IF NOT EXISTS users (
    id          BIGSERIAL PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Clés publiques SSH nominatives (inference-translator) : une clé
-- publique n'est pas un secret, elle vit en clair ; l'empreinte
-- SHA-256 porte l'unicité
CREATE TABLE IF NOT EXISTS access_keys (
    id          BIGSERIAL PRIMARY KEY,
    user_id     BIGINT NOT NULL REFERENCES users(id),
    key_pub     TEXT NOT NULL,                -- clé publique SSH
    fingerprint CHAR(64) NOT NULL UNIQUE,     -- SHA-256 de la clé
    label       TEXT,                         -- ex. "portable de Claire"
    issued_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    revoked_at  TIMESTAMPTZ                   -- NULL = clé active
);

-- Journal des tentatives d'authentification SSH refusées (inference)
CREATE TABLE IF NOT EXISTS auth_failures (
    id          BIGSERIAL PRIMARY KEY,
    fingerprint CHAR(64),                      -- NULL si clé invalide
    origin      TEXT,                         -- adresse d'origine
    refused_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

---

## 7. Contraintes et conventions techniques

- **Langue du code et des commentaires** : anglais, style Claude
  Delannoy (commentaires abondants), en cohérence avec
  `neuron-translator` et `orchestrator-translator`.
- **C23 / POSIX.1-2008**, bibliothèques Hurd (`trivfs` pour le MVP,
  `netfs` dès que la navigation par identifiant s'étoffe).
- **`libpq` uniquement**, requêtes préparées, aucune construction de
  SQL par concaténation : les lignes JSON du contrat sont mappées
  sur des paramètres, jamais interpolées.
- **Zéro allocation dans les chemins chauds** : requêtes préparées
  au montage, zones contiguës pré-allouées, lectures paginées.
- **Multitâche et multi-utilisateurs** : le translator doit servir
  plusieurs appelants et plusieurs utilisateurs **simultanément** —
  requêtes concurrentes sérialisées proprement (verrou sur la
  connexion libpq partagée), curseur de lecture paginé propre à
  chaque lecteur, aucun état global non protégé ; c'est cette
  exigence qui décide, mesures à l'appui, du passage d'une
  connexion unique au pool de connexions.
- **Contraintes SQL ≠ erreurs POSIX** : doublon, absence de ligne,
  champ manquant sont des statuts lisibles (`duplicate`, `empty`,
  `invalid`), seuls les échecs de transport remontent en `EIO`.
- **Clés privées jamais vues** : le registre ne stocke que des clés
  publiques SSH (en clair — nature publique) et leurs empreintes
  SHA-256 ; une clé privée ne traverse jamais `/db`.
- Chaque translator reste remplaçable : l'orchestrateur ne connaît
  que `/db` et le contrat d'échange, jamais ce binaire.

### Stratégie de tests unitaires

- **Harnais maison minimal** : macros `CHECK` et compteurs en
  C23/POSIX, zéro framework externe — même convention que
  `tests/test_neuron.c` (`neuron-translator`) et la suite
  d'`httpfs-translator` ; `make check` est la cible standard, exigée
  par les critères d'acceptation.
- **Instance jetable** : la suite crée et détruit sa propre base de
  test (schéma idempotent, section 6) — aucun test n'écrit dans une
  base réelle ; ici pas de backend mémoire : c'est PostgreSQL ou
  rien, dès la phase 1.
- **Contraintes comme statuts** : doublon de `checksum`, absence de
  ligne, champ manquant sont testés comme des réponses lisibles
  (`duplicate`, `empty`, `invalid`), jamais comme des erreurs.
- **Transaction sur chaque écriture** : à la manière des tests de
  persistance de `neuron-translator` (fichier tronqué, magie
  corrompue, octets traînants), un échec en cours de traitement ne
  doit jamais laisser d'état partiel visible côté `/db`.
- **Pagination bornée** : lectures paginées vérifiées sur des
  contenus volumineux générés, mémoire mesurée.

---

## 8. Jalons synthétiques

| Phase | Contenu | Dépend de | Attendu par (pile) |
|---|---|---|---|
| 0 | Spécification, contrats, `schema.sql` | — | orchestrateur phase 0, inference phase 0 |
| 1 | MVP : schéma + écritures `runs`/`run_instances`/`training_data` | 0 | orchestrateur phase 1 |
| 2 | `incidents` + requêtes de lecture | 1 | orchestrateur phase 2 |
| 3 | Gros contenus, anti-doublon avant téléchargement | 1 | orchestrateur phase 3 |
| 4 | Rejouabilité, exports, historique SQL | 2, 3 | orchestrateur phase 4 |
| 5 | Registre de clés SSH : `users`, `access_keys`, `auth_failures` | 1 | inference phase 5 |
| 6 | Durcissement, CI Hurd, v1.0 | 1–5 | orchestrateur phase 6, inference phase 7 |

Les jalons 1→2 et 1→3 peuvent avancer en parallèle, comme les fils
de travail de l'orchestrateur. La base de données n'est pas un
chantier en soi : c'est une propriété permanente de la pile — ce
dépôt ne fait qu'implémenter fidèlement les contrats et le schéma
gelés ensemble, puis les servir vite, sans fuite, et sans jamais
décider à la place de l'appelant. Le registre de clés SSH suit la
même règle : le translator sert le registre, sshd authentifie, le
serveur `inference` décide.
