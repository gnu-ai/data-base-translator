# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

# --- Le cœur du contrat ------------------------------------------------
# contract.c (parseur JSON de la ligne, réponses), storage.c (états,
# compteurs, DDL embarqué) et ops.c (moteur une ligne -> une
# réponse) sont du C23/POSIX pur.  Le driver libpq (storage_pg.c)
# n'est lié que quand configure l'a trouvé — sans lui le binaire
# reste compilable, le statut reste lisible et honnête.  La
# bibliothèque est aussi ce que lie la suite de tests.
noinst_LIBRARIES = libdbtcore.a

libdbtcore_a_SOURCES = \
    contract.c \
    storage.c \
    ops.c

# The internal headers are part of the core library: listing them
# here puts them in the distribution so the VPATH builds of
# `make distcheck` compile from the released tarball.
noinst_HEADERS = \
    contract.h \
    ops.h \
    storage.h \
    storage_pg.h

if HAVE_LIBPQ
libdbtcore_a_SOURCES += storage_pg.c
libdbtcore_a_CPPFLAGS = $(AM_CPPFLAGS) $(LIBPQ_CFLAGS)
endif

AM_CPPFLAGS = -I$(top_srcdir)/src
AM_CFLAGS = $(C23_CFLAGS) -Wall -Wextra

bin_PROGRAMS = db-translator

# --- La face du binaire, selon le système -------------------------------
# Sur GNU/Hurd, main-hurd.c suit la structure canonique d'un
# translator trivfs (trivfs_startup puis boucle serveur) et
# trivfs-hooks.c implémente les trivfs_S_* du protocole de lignes ;
# le binaire de vérification (main.c, le même moteur sur les flux
# standards) est construit sous le nom db-translator-repl : c'est
# lui que la suite de tests pilote (make check doit rester vert
# sous Hurd comme partout).
if ON_HURD
bin_PROGRAMS += db-translator-repl

db_translator_SOURCES = trivfs-hooks.c main-hurd.c
db_translator_CPPFLAGS = $(AM_CPPFLAGS) -DON_HURD $(LIBPQ_CFLAGS)
db_translator_LDADD = libdbtcore.a -ltrivfs -lfshelp -lports \
                      -lshouldbeinlibc $(LIBPQ_LIBS)

db_translator_repl_SOURCES = main.c
db_translator_repl_CPPFLAGS = $(AM_CPPFLAGS) $(LIBPQ_CFLAGS)
db_translator_repl_LDADD = libdbtcore.a $(LIBPQ_LIBS)
else
db_translator_SOURCES = main.c
db_translator_CPPFLAGS = $(AM_CPPFLAGS) $(LIBPQ_CFLAGS)
db_translator_LDADD = libdbtcore.a $(LIBPQ_LIBS)
endif
