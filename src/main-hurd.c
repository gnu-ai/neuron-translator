/*
 * main-hurd.c - Entry point for the GNU/Hurd translator
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026  GNU AI Project
 * Author: Claire Ivanenka <claire@gnu-ai.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

/** @file main-hurd.c
 *  @brief Entry point for the sigmoid neuron translator on GNU/Hurd
 *
 *  This follows the canonical trivfs translator structure (see
 *  trans/null.c in the Hurd sources).  A translator MUST:
 *
 *    1. Get the bootstrap port that settrans passed us.
 *    2. Call trivfs_startup() to reply to settrans and obtain the
 *       control port (fsys).
 *    3. Enter a server loop with ports_manage_port_operations_one_thread.
 *
 *  libtrivfs provides the demuxer (trivfs_demuxer) which dispatches
 *  incoming RPCs to our trivfs_S_* functions defined in trivfs-hooks.c.
 *  A translator that returns from main() immediately dies, and settrans
 *  reports "Translator died".
 */

/* GNU Mach 1.8+git20260224 (the Debian forky/sid snapshot): the
 * installed mach_host.h uses processor_name_array_t, but no
 * installed header defines it — the MIG header generation of
 * this snapshot drops the typedef.  Provide the canonical
 * definition BEFORE the Hurd headers so they compile; C tolerates
 * the identical redefinition the day the snapshot is fixed. */
#include <mach/processor_info.h>
typedef processor_info_t *processor_name_array_t;

#include <hurd.h>
#include <hurd/ports.h>
#include <hurd/trivfs.h>
#include <hurd/fsys.h>

#include <argp.h>
#include <error.h>
#include <stdio.h>
#include <stdlib.h>

/* trivfs requirement: the control structure filled in by trivfs_startup(),
 * which holds the port buckets libtrivfs serves our RPCs through. */
struct trivfs_control *fsys;

const char *argp_program_version = "sigmoid-neuron-translator (GNU AI) 0.1.0";
const char *argp_program_bug_address = "<claire@gnu-ai.org>";
static char doc[] = "GNU/Hurd sigmoid neuron translator.";

/* The standard GNU --version answer.  The default argp answer
 * prints only the version line; the hook prints the full block
 * (project, version, license) so the Hurd entry point answers
 * exactly like the verification build of main.c on other POSIX
 * systems. */
static void
print_version_hook (FILE *stream, struct argp_state *state)
{
  (void) state;                  /* Unused: no state needed here */

  fprintf (stream, "%s\n", argp_program_version);
  fprintf (stream, "License GPLv3+: GNU GPL version 3 or later"
           " <https://gnu.org/licenses/gpl.html>.\n");
  fprintf (stream, "This is free software: you are free to change"
           " and redistribute it.\n");
  fprintf (stream, "There is NO WARRANTY, to the extent permitted by law.\n");
}

void (*argp_program_version_hook) (FILE *, struct argp_state *)
    = print_version_hook;

/* Command-line options accepted by the translator.
 *
 * -h is declared explicitly: glibc's argp answers --help natively
 * but NOT its short form -h (it reserves only '?'), and the GNU
 * convention used across the GNU AI stack is that both spellings
 * exit 0 with the same answer. */
static struct argp_option options[] = {
  {"bias", 'b', "FLOAT", 0, "Initial bias value for the neuron", 0},
  {"help", 'h', 0, 0, "display this help and exit", 0},
  { 0 }
};

/* Initial neuron parameter, set by the --bias option */
float global_bias = 0.0f;

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'b':
      global_bias = atof (arg);
      break;
    case 'h':
      /* The same standard help as --help, on stdout, exit 0. */
      argp_state_help (state, stdout, ARGP_HELP_STD_HELP);
      exit (EXIT_SUCCESS);
    case ARGP_KEY_SUCCESS:
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

  /* Parse the command line: settrans passes any arguments that follow
     the translator path on to us. */
  argp_parse (&argp, argc, argv, 0, 0, 0);

  /* Get the bootstrap port that the parent (settrans) handed us */
  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "Must be started as a translator");

  /* Start the trivfs server: this replies to settrans, telling it the
     translator is up, and returns our control port in FSYS. */
  err = trivfs_startup (bootstrap, 0, 0, 0, 0, 0, &fsys);
  mach_port_deallocate (mach_task_self (), bootstrap);
  if (err)
    error (3, err, "Contacting parent failed");

  /* Enter the server loop: receive incoming RPCs forever and let
     trivfs_demuxer dispatch them to our trivfs_S_* hooks. */
  ports_manage_port_operations_one_thread (fsys->pi.bucket,
                                            trivfs_demuxer, 0);

  return 0;
}
