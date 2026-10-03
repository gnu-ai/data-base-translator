/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * main-hurd.c — Entry point of the /db translator on GNU/Hurd.
 *
 * This follows the canonical trivfs translator structure (the
 * same as trans/null.c in the Hurd sources, and as
 * neuron-translator): a translator MUST
 *
 *    1. Get the bootstrap port that settrans passed us.
 *    2. Call trivfs_startup() to reply to settrans and obtain
 *       the control port (fsys).
 *    3. Enter a server loop with
 *       ports_manage_port_operations_one_thread.
 *
 * libtrivfs provides the demuxer (trivfs_demuxer) which dispatches
 * the incoming RPCs to the trivfs_S_* hooks of trivfs-hooks.c.
 * A translator that returns from main() immediately dies, and
 * settrans reports "Translator died".
 *
 * The server is single threaded: the RPCs of the several readers
 * and writers are serialized here, so the engine state (ops.c)
 * needs no lock in the MVP — one cursor per open file is kept in
 * each peropen, never shared.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <hurd.h>
#include <hurd/ports.h>
#include <hurd/trivfs.h>
#include <hurd/fsys.h>

#include <argp.h>
#include <error.h>
#include <stdio.h>
#include <stdlib.h>

#include "ops.h"
#include "storage.h"

/* trivfs requirement: the control structure filled by
 * trivfs_startup(), holding the port buckets our RPCs are served
 * through. */
struct trivfs_control *fsys;

const char *argp_program_version = "db-translator (GNU AI) " VERSION;
const char *argp_program_bug_address = "<claire@gnu-ai.org>";
static char doc[] = "Persistence layer of the GNU AI stack"
                    " for GNU/Hurd.";

/* The standard GNU --version answer, identical to the one of the
 * verification build (main.c). */
static void
print_version_hook (FILE *stream, struct argp_state *state)
{
  (void) state;

  fprintf (stream, "%s\n", argp_program_version);
  fprintf (stream, "License GPLv3+: GNU GPL version 3 or later"
           " <https://gnu.org/licenses/gpl.html>.\n");
  fprintf (stream, "This is free software: you are free to change"
           " and redistribute it.\n");
  fprintf (stream, "There is NO WARRANTY, to the extent permitted"
           " by law.\n");
}

void (*argp_program_version_hook) (FILE *, struct argp_state *)
    = print_version_hook;

/* -h is declared explicitly: argp answers --help natively but not
 * its short form, and the GNU convention of the stack is that
 * both spellings exit 0 with the same answer. */
static struct argp_option options[] = {
  {"conninfo", 'c', "STRING", 0,
   "libpq connection string (e.g. \"dbname=gnuai\")", 0},
  {"help", 'h', 0, 0, "display this help and exit", 0},
  { 0 }
};

/* The connection string handed to the storage layer at startup:
 * the single argument of the mount, the only configuration the
 * translator knows (PLAN.md section 3.3). */
static const char *conninfo = NULL;

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'c':
      conninfo = arg;
      break;
    case 'h':
      argp_state_help (state, stdout, ARGP_HELP_STD_HELP);
      exit (EXIT_SUCCESS);
    case ARGP_KEY_SUCCESS:
      if (conninfo == NULL)
        {
          fprintf (stderr, "db-translator:"
                   " --conninfo STRING is required at mount\n");
          argp_state_help (state, stderr, ARGP_HELP_STD_HELP);
          exit (EXIT_FAILURE);
        }
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

static struct argp argp = { options, parse_opt, 0, doc, 0, 0, 0 };

int
main (int argc, char *argv[])
{
  error_t err;
  mach_port_t bootstrap;

  /* Parse the command line: settrans passes the arguments that
   * follow the translator path on to us. */
  argp_parse (&argp, argc, argv, 0, 0, 0);

  /* Connect and prepare BEFORE replying to settrans: when the
   * mount returns, the schema exists and the prepared statements
   * are ready (a failed connection is not fatal — the translator
   * retries on each write, PLAN.md section 4). */
  ops_init (conninfo);

  /* Get the bootstrap port the parent (settrans) handed us. */
  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "Must be started as a translator");

  /* Reply to settrans and obtain our control port. */
  err = trivfs_startup (bootstrap, 0, 0, 0, 0, 0, &fsys);
  mach_port_deallocate (mach_task_self (), bootstrap);
  if (err)
    error (3, err, "Contacting parent failed");

  /* Serve RPCs forever: the demuxer dispatches to the
   * trivfs_S_* hooks of trivfs-hooks.c. */
  ports_manage_port_operations_one_thread (fsys->pi.bucket,
                                           trivfs_demuxer, 0);

  ops_shutdown ();
  return 0;
}
