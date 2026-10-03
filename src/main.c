/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * main.c — Entry point of the data-base-translator (phase 0
 * skeleton).
 *
 * USAGE
 * -----
 *      db-translator --version
 *      db-translator --help
 *      settrans -a /db db-translator --conninfo "dbname=gnuai"   (phase 1)
 *
 * data-base-translator is the persistence layer of the GNU AI
 * stack: it exposes a PostgreSQL database as a Hurd filesystem, so
 * that the other translators read and write their data (training
 * data, runs, incidents, the SSH key registry of the remote mode)
 * by plain POSIX writes and reads — never by linking a SQL client
 * library or opening a socket (PLAN.md section 1).
 *
 * The phase 0 skeleton answers the GNU base commands and refuses
 * everything else: the /db translator itself arrives with phase 1.
 * The build architecture is the one shared by the whole stack
 * (see httpfs-translator).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- The GNU base commands ----------------------------------------
   Every binary of the GNU AI stack answers --version and --help
   before anything else, so a translator stays inspectable like any
   other GNU tool — from a shell or from a script.  The answers
   follow the GNU coding standards; the version number comes from
   configure (config.h), the single source of truth of the release. */

static void
print_version (void)
{
    printf ("db-translator (GNU AI) %s\n", VERSION);
    printf ("License GPLv3+: GNU GPL version 3 or later"
            " <https://gnu.org/licenses/gpl.html>.\n");
    printf ("This is free software: you are free to change"
            " and redistribute it.\n");
    printf ("There is NO WARRANTY, to the extent permitted by law.\n");
}

static void
print_help (void)
{
    printf ("Usage: db-translator [OPTION]... [--conninfo STRING]\n");
    printf ("Persistence layer of the GNU AI stack for GNU/Hurd.\n");
    printf ("\n");
    printf ("  -h, --help     display this help and exit\n");
    printf ("  -V, --version  output version information and exit\n");
    printf ("\n");
    printf ("Phase 1 mounts the translator:\n");
    printf ("  settrans -a /db db-translator --conninfo \"dbname=gnuai\"\n");
    printf ("Then a row is plain POSIX:\n");
    printf ("  echo '{\"table\": \"runs\", \"row\": {…}}' > /db/runs\n");
    printf ("  cat /db/runs\n");
    printf ("\n");
    printf ("Report bugs at"
            " <https://github.com/gnu-ai/data-base-translator/issues>.\n");
}

/* Scan the command line; return true when one of the two base
 * commands was recognized and answered (the caller exits 0),
 * false when the normal startup should proceed. */
static bool
handle_gnu_options (int argc, char *argv[])
{
    for (int i = 1; i < argc; i++)
        {
            if (strcmp (argv[i], "--version") == 0
                || strcmp (argv[i], "-V") == 0)
                {
                    print_version ();
                    return true;
                }
            if (strcmp (argv[i], "--help") == 0
                || strcmp (argv[i], "-h") == 0)
                {
                    print_help ();
                    return true;
                }
        }
    return false;
}

int
main (int argc, char *argv[])
{
    /* The base commands are answered before any Hurd library or
     * libpq call is made. */
    if (handle_gnu_options (argc, argv))
        return EXIT_SUCCESS;

    /* Phase 0: nothing is mounted yet.  Refuse cleanly instead of
     * dying deep inside a library — the same exit path that
     * settrans will report as "Translator died". */
    fprintf (stderr, "db-translator: phase 0 skeleton —"
             " the /db translator arrives with phase 1"
             " (see PLAN.md)\n");
    return EXIT_FAILURE;
}
