/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * main.c — Entry point of the verification build (non-Hurd POSIX).
 *
 * USAGE
 * -----
 *      db-translator --version
 *      db-translator --help
 *      db-translator [--conninfo STRING] < instructions.jsonl
 *
 * On GNU/Hurd the binary is the trivfs translator mounted by
 * settrans (see main-hurd.c); everywhere else it is the same
 * engine over standard streams: one instruction line in, one
 * response line out.  This is how the contract is tested on any
 * POSIX system without Hurd libraries and without a mounted
 * translator — exactly the split used by neuron-translator.
 *
 * Transport failures are reported on stderr with the EIO they
 * stand for, and make the exit status 1; they never stop the
 * loop: the translator retries on the next line, it does not die
 * (PLAN.md section 4).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ops.h"
#include "storage.h"

/* --- The GNU base commands ----------------------------------------
   Every binary of the GNU AI stack answers --version and --help
   before anything else, so a translator stays inspectable like
   any other GNU tool.  The answers follow the GNU coding
   standards; the version number comes from configure. */

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
    printf ("Persistence layer of the GNU AI stack.\n");
    printf ("\n");
    printf ("  -h, --help          display this help and exit\n");
    printf ("  -V, --version       output version information and exit\n");
    printf ("      --conninfo STR  libpq connection string"
            " (default: dbname=gnuai)\n");
    printf ("\n");
    printf ("On GNU/Hurd, the binary is the /db translator:\n");
    printf ("  settrans -a /db db-translator --conninfo \"dbname=gnuai\"\n");
    printf ("Elsewhere it reads instruction lines on stdin and\n");
    printf ("answers one response line on stdout per instruction.\n");
    printf ("\n");
    printf ("Report bugs at"
            " <https://github.com/gnu-ai/data-base-translator/issues>.\n");
}

/* Returns true when a base command was answered (exit 0), false
 * when the startup must proceed.  The option parsing is
 * deliberately hand written: the translator must parse its
 * arguments without glibc's argp, which is not available under
 * the Hurd bootstrap conditions the sibling entry point faces. */
static bool
handle_gnu_options (int argc, char *argv[], const char **conninfo)
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
            if (strcmp (argv[i], "--conninfo") == 0)
                {
                    if (i + 1 >= argc)
                        {
                            fprintf (stderr, "db-translator:"
                                     " --conninfo requires an argument\n");
                            exit (EXIT_FAILURE);
                        }
                    *conninfo = argv[++i];
                }
        }
    return false;
}

int
main (int argc, char *argv[])
{
    const char *conninfo = "dbname=gnuai";
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int transport_failures = 0;

    /* The base commands are answered before any library call. */
    if (handle_gnu_options (argc, argv, &conninfo))
        return EXIT_SUCCESS;

    ops_init (conninfo);

    /* One instruction line in, one response line out: the same
     * engine the trivfs hooks run, over standard streams. */
    while ((n = getline (&line, &cap, stdin)) >= 0)
        {
            size_t len = (size_t) n;

            if (len > 0 && line[len - 1] == '\n')
                len--;
            if (ops_process_line (line, len) < 0)
                {
                    fprintf (stderr, "db-translator: %s (EIO)\n",
                             dbt_storage_last_error ());
                    transport_failures++;
                }
            else
                {
                    fwrite (ops_response (), 1, ops_response_len (),
                            stdout);
                    fputc ('\n', stdout);
                    fflush (stdout);
                }
        }

    free (line);
    ops_shutdown ();
    return transport_failures > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
