<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

Kanban du Data Base Translator, dérivé du PLAN.md : une carte par
tâche. Déplacer une carte = la déplacer entre les sections
ci-dessous. Le détail des livrables et des critères d'acceptation
reste dans PLAN.md.
-->

# Data Base Translator — Kanban

Dérivé du [`PLAN.md`](PLAN.md). Chaque carte est préfixée par sa
phase ; les critères d'acceptation de chaque phase sont dans le plan.

## À faire

### Phase 2 — Incidents et lectures
- [ ] Écrire les incidents (`crash`, `timeout`, `restart`), persistés dès leur détection
- [ ] Requêtes de lecture : `select` avec filtres simples et tri (`where`, `order`), flux de lignes JSON
- [ ] Navigation directe `/db/runs/<id>` et `/db/runs/<id>/instances`

### Phase 3 — Acquisition réseau servie : `training_data`
- [ ] Support des gros contenus : écriture et lecture paginées du champ `content`, curseur POSIX, empreinte vérifiée
- [ ] Archiver les contenus récupérés via httpfs-translator (y compris les 404, avec `http_status`)
- [ ] Requêtes par plage de date, statut HTTP, absence en base (`checksum`) pour l'anti-doublon avant téléchargement
- [ ] Mode cluster : écritures et lectures concurrentes multi-nœuds (verrou libpq, tables `users`/`access_keys` anticipées)

### Phase 4 — Rejouabilité : exports et historique
- [ ] Export d'un jeu d'entraînement archivé en flux de vecteurs d'entrée rejouables
- [ ] Historique et statistiques : performances par topologie calculées en SQL, servies en lignes JSON
- [ ] Pagination et limites de mémoire bornées sur toutes les lectures

### Phase 5 — Registre des clés SSH (mode distant d'inference-translator)
- [ ] Écriture de `users` et `access_keys` : clé publique nominative, empreinte SHA-256, unicité sur `fingerprint`
- [ ] Génération d'`authorized_keys` depuis le registre actif (seule source de vérité des accès distants)
- [ ] Révocation par écriture d'une date (`revoked_at`) puis re-génération d'`authorized_keys`
- [ ] Journal des échecs d'authentification SSH (`auth_failures` : empreinte, origine, horodatage)
- [ ] Navigation `/db/users` et `/db/access_keys` lisibles directement

### Phase 6 — Durcissement, tests, CI
- [ ] Reconnexion automatique PostgreSQL, lecture seule documentée, requêtes bornées, journal des échecs transport
- [ ] Suite de tests déterministes sur instance PostgreSQL jetable (doublons, pagination, reconnexion, clés SSH, charge)
- [ ] Documentation utilisateur et architecture (`docs/architecture.md`)

## En cours

_(rien)_

## Fait

### Phase 6 — Durcissement, tests, CI (entamée)
- [x] CI sous QEMU GNU/Hurd, pilotée par [gnu-ai/mistral-vm-debian-hurd](https://github.com/gnu-ai/mistral-vm-debian-hurd) (`.github/workflows/hurd.yml`)

### Phase 1 — MVP : schéma + écritures
- [x] Connexion libpq au montage, création idempotente du schéma (`CREATE TABLE IF NOT EXISTS`), `/db/status` lisible
- [x] Écriture de `runs`, `run_instances` et `training_data` via requêtes préparées, avec retour de l'identifiant attribué
- [x] Signalement du doublon `checksum` comme statut `duplicate`

### Phase 0 — Spécification et contrats
- [x] Reprise du contrat `orchestrator → database` et gel du format d'échange (lignes JSON, une instruction et une réponse par ligne)
- [x] Reprise du contrat `inference → database` (registre des clés SSH : `users`, `access_keys`, `auth_failures`)
- [x] Gel de l'arborescence `/db` (status, schema, tables, navigation par identifiant)
- [x] Implémentation de référence du schéma SQL (`schema.sql`, source de vérité partagée)
- [x] Convention d'arguments de montage (`conninfo` libpq, options `create`/`verify`/`readonly`)
- [x] Livrable : `SPEC.md` + squelette compilable
