/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * trivfs-hooks.c — Hurd trivfs server routines of /db.
 *
 * libtrivfs demultiplexes the incoming RPCs to the trivfs_S_*
 * functions defined here (the canonical structure of the Hurd
 * translators, like trans/null.c and neuron-translator).
 *
 * The node speaks the frozen line protocol (SPEC.md):
 *
 *   - io_write assembles the bytes into complete lines; each
 *     newline terminated line is one instruction handed to the
 *     engine (ops.c), which installs the response in the slot.
 *   - io_read serves the response slot from the reader's own
 *     cursor: `cat /db` after mount answers the status line, and
 *     after an instruction the response to it.  One cursor per
 *     open file per reader, never shared.
 *
 * Because libtrivfs's defaults abort with an assertion when
 * trivfs_support_read and trivfs_support_write are both set, all
 * of io_read, io_write, io_readable, io_seek, io_select and the
 * open mode checks are provided here.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "contract.h"
#include "ops.h"

#if ON_HURD == 1

#include <hurd/trivfs.h>
#include <hurd/fsys.h>
#include <hurd/hurd_types.h>

/* ---------------------------------------------------------------------
 *  TRIVFS VARIABLES (read by libtrivfs, documented in trivfs.h)
 * ------------------------------------------------------------------- */

int trivfs_fstype = FSTYPE_MISC;
int trivfs_fsid = 0;

int trivfs_support_read = 1;
int trivfs_support_write = 1;
int trivfs_support_exec = 0;

int trivfs_allow_open = O_READ | O_WRITE;

char *fs_help = "db-translator -- GNU AI persistence layer (/db)\n"
                "Usage: settrans -a /db db-translator"
                " --conninfo \"dbname=gnuai\"";

/* ---------------------------------------------------------------------
 *  PER-OPEN STATE
 *
 *  Each open of the translator node carries its own line assembly
 *  buffer and its own read cursor: the multi-user rule of the
 *  stack is per open file per reader, never a shared mutable
 *  cursor.  The buffer is as large as one instruction line of the
 *  contract (256 KiB in phase 1; the paginated contents of phase 3
 *  will revisit this sizing).
 * ------------------------------------------------------------------- */

struct peropen_data
  {
    char   line[DBT_MAX_LINE];
    size_t line_len;         /* bytes assembled so far */
    size_t read_offset;      /* this reader's cursor in the slot */
  };

static error_t
peropen_create (struct trivfs_peropen *po)
{
  po->hook = calloc (1, sizeof (struct peropen_data));
  return po->hook != NULL ? 0 : ENOMEM;
}

static void
peropen_destroy (struct trivfs_peropen *po)
{
  free (po->hook);
  po->hook = NULL;
}

/* Install the peropen hooks before main runs, so they are in
 * place before trivfs_startup is called. */
static void __attribute__ ((constructor))
translator_init (void)
{
  trivfs_peropen_create_hook = peropen_create;
  trivfs_peropen_destroy_hook = peropen_destroy;
}

/* ---------------------------------------------------------------------
 *  MANDATORY TRIVFS HOOKS
 * ------------------------------------------------------------------- */

/* Present the node as a regular file whose size is the response
 * slot: stat stays honest for the callers that probe it. */
void
trivfs_modify_stat (struct trivfs_protid *cred, io_statbuf_t *st)
{
  (void) cred;

  st->st_mode &= ~((mode_t) S_IFMT);
  st->st_mode |= S_IFREG;
  st->st_size = (loff_t) ops_response_len ();
}

/* settrans -g or shutdown: go away cleanly. */
error_t
trivfs_goaway (struct trivfs_control *cntl, int flags)
{
  (void) cntl;
  (void) flags;

  ops_shutdown ();
  exit (0);
}

/* ---------------------------------------------------------------------
 *  IO SERVER ROUTINES
 * ------------------------------------------------------------------- */

kern_return_t
trivfs_S_io_read (struct trivfs_protid *cred,
                  mach_port_t reply,
                  mach_msg_type_name_t replytype,
                  data_t *data,
                  mach_msg_type_number_t *datalen,
                  loff_t offs,
                  vm_size_t amount)
{
  struct peropen_data *pod;
  const char *slot;
  size_t slot_len;
  loff_t position;

  (void) reply;
  (void) replytype;

  if (cred == NULL)
    return EOPNOTSUPP;
  if (!(cred->po->openmodes & O_READ))
    return EBADF;

  pod = cred->po->hook;
  if (pod == NULL)
    return EOPNOTSUPP;

  slot = ops_response ();
  slot_len = ops_response_len ();

  /* OFFSET -1: read from this reader's own cursor. */
  position = offs;
  if (position == -1)
    position = (loff_t) pod->read_offset;
  if (position < 0)
    position = 0;

  if ((size_t) position >= slot_len || amount == 0)
    {
      *datalen = 0;
      return 0;                       /* end of the response */
    }

  if (amount > (vm_size_t) (slot_len - (size_t) position))
    amount = (vm_size_t) (slot_len - (size_t) position);

  /* Enlarge the reply buffer when the inline one is too small, as
   * in trans/random.c; mig deallocates it after the reply. */
  if (*datalen < amount)
    {
      *data = mmap (0, amount, PROT_READ | PROT_WRITE,
                    MAP_ANON | MAP_PRIVATE, -1, 0);
      if (*data == MAP_FAILED)
        return ENOMEM;
    }

  memcpy (*data, slot + position, amount);
  *datalen = (mach_msg_type_number_t) amount;

  if (offs == -1)
    pod->read_offset = (size_t) (position + (loff_t) amount);

  return 0;
}

kern_return_t
trivfs_S_io_readable (struct trivfs_protid *cred,
                      mach_port_t reply,
                      mach_msg_type_name_t replytype,
                      vm_size_t *amount)
{
  struct peropen_data *pod;

  (void) reply;
  (void) replytype;

  if (cred == NULL)
    return EOPNOTSUPP;
  if (!(cred->po->openmodes & O_READ))
    return EBADF;

  pod = cred->po->hook;
  if (pod == NULL)
    return EOPNOTSUPP;

  {
    size_t remaining = ops_response_len () - pod->read_offset;

    *amount = (vm_size_t) remaining;
  }
  return 0;
}

/* Write: the instruction lines arrive here, possibly split over
 * several RPCs.  Each newline terminated line is one instruction;
 * a line that fills the buffer without a newline is refused as
 * the contract's "toolong" status and the rest is skipped until
 * the next newline. */
kern_return_t
trivfs_S_io_write (struct trivfs_protid *cred,
                   mach_port_t reply,
                   mach_msg_type_name_t replytype,
                   const_data_t data,
                   mach_msg_type_number_t datalen,
                   loff_t offs,
                   vm_size_t *amt)
{
  struct peropen_data *pod;
  size_t i;

  (void) reply;
  (void) replytype;
  (void) offs;

  if (cred == NULL)
    return EOPNOTSUPP;
  if (!(cred->po->openmodes & O_WRITE))
    return EBADF;

  pod = cred->po->hook;
  if (pod == NULL)
    return EOPNOTSUPP;

  for (i = 0; i < (size_t) datalen; i++)
    {
      if (data[i] == '\n')
        {
          /* One complete instruction: hand it to the engine and
           * reset this writer's own cursor, so it can read its
           * answer back on the same open file. */
          if (ops_process_line (pod->line, pod->line_len) < 0)
            {
              /* Only the transport fails as a POSIX error; the
               * assembled line is dropped, the next one starts
               * clean (PLAN.md section 4). */
              pod->line_len = 0;
              *amt = (vm_size_t) i;
              return EIO;
            }
          pod->line_len = 0;
          pod->read_offset = 0;
          continue;
        }

      if (pod->line_len >= DBT_MAX_LINE - 1)
        {
          /* Overflow without a newline: the status the contract
           * defines, then discard until the end of the runaway
           * line. */
          ops_process_line (pod->line, DBT_MAX_LINE);   /* toolong */
          pod->line_len = 0;
          while (i < (size_t) datalen && data[i] != '\n')
            i++;
          if (i < (size_t) datalen && data[i] == '\n')
            {
              pod->read_offset = 0;
              continue;
            }
          break;
        }

      pod->line[pod->line_len++] = (char) data[i];
    }

  *amt = (vm_size_t) datalen;
  return 0;
}

kern_return_t
trivfs_S_io_seek (struct trivfs_protid *cred,
                  mach_port_t reply,
                  mach_msg_type_name_t replytype,
                  loff_t offs,
                  int whence,
                  loff_t *new_offs)
{
  struct peropen_data *pod;

  (void) reply;
  (void) replytype;

  if (cred == NULL)
    return EOPNOTSUPP;

  pod = cred->po->hook;
  if (pod == NULL)
    return EOPNOTSUPP;

  switch (whence)
    {
    case SEEK_SET:
      break;
    case SEEK_CUR:
      offs += (loff_t) pod->read_offset;
      break;
    case SEEK_END:
      offs += (loff_t) ops_response_len ();
      break;
    default:
      return EINVAL;
    }

  if (offs < 0)
    return EINVAL;

  /* Seeking past the response is legal and reads as EOF; the
   * cursor of this reader only. */
  pod->read_offset = (size_t) offs;
  *new_offs = offs;
  return 0;
}

/* We are always ready to read (EOF included) and to write. */
kern_return_t
trivfs_S_io_select (struct trivfs_protid *cred,
                    mach_port_t reply,
                    mach_msg_type_name_t replytype,
                    int *type)
{
  (void) reply;
  (void) replytype;

  if (cred == NULL)
    return EOPNOTSUPP;

  if (*type & ~(SELECT_READ | SELECT_WRITE))
    return EINVAL;

  return 0;
}

kern_return_t
trivfs_S_io_select_timeout (struct trivfs_protid *cred,
                            mach_port_t reply,
                            mach_msg_type_name_t replytype,
                            int *type,
                            struct timespec *tsp)
{
  (void) tsp;

  return trivfs_S_io_select (cred, reply, replytype, type);
}

#endif /* ON_HURD == 1 */
