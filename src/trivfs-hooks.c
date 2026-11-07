/*
 * trivfs-hooks.c - Hurd Translator Server Routines
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
 /*
  * Developer: Alperen ERKAN 2026 <erkan [at] gnu-ai.org>
 */
/** @file trivfs-hooks.c
 *  @brief Implementation of the trivfs server routines
 *
 *  libtrivfs demultiplexes incoming RPCs (via its trivfs_demuxer) to the
 *  trivfs_S_* functions defined in this file.  This structure follows
 *  the canonical trivfs translators in the Hurd sources (trans/null.c,
 *  trans/random.c).
 *
 *  Because libtrivfs's default trivfs_S_io_read, trivfs_S_io_write,
 *  trivfs_S_io_readable, trivfs_S_io_seek, trivfs_S_io_select and
 *  openmode functions abort with an assertion when
 *  trivfs_support_read/trivfs_support_write are set, we must provide
 *  our own definitions of all of them.
 */

#if ON_HURD == 1
/* GNU Mach 1.8+git20260224 (the 2026-03-14 preinstalled image):
 * its installed headers drop the processor_name_array_t
 * typedef, so the MIG-generated mach_host.h - and with it
 * <mach.h> itself - does not compile.  The current ports
 * package (2:1.8+git20260805-4) carries the typedef in
 * <mach/mach_types.h> as
 *   typedef mach_port_t *processor_name_array_t;
 * Define it the SAME way here: C tolerates the identical
 * redefinition, so this keeps compiling on fixed systems. */

#include <mach/port.h>                  /* mach_port_t */
typedef mach_port_t *processor_name_array_t;
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "trivfs-hooks.h"
#include "neuron.h"
#include "commands.h"
#include <stdint.h>
#include <ctype.h>
#include "debug.h"

/*****************************************************************************
 *  GLOBAL STATE
 *****************************************************************************/

/* The network served through the translator node */
CompactNeuralNetwork global_network = {0};

/* Help text */
char *fs_help = "LLM Sigmoid Neuron Translator for GNU Hurd\n"
                "Usage: settrans -a <node> /hurd/sigmoid-neuron-translator";

/* Placeholder used by the non-Hurd test harness */
mach_port_t trivfs_control = MACH_PORT_NULL;

#if ON_HURD == 1

/* <hurd/trivfs.h> pulls in <hurd/iohelp.h> and thus <hurd/hurd_types.h>,
 * which defines data_t, const_data_t and the SELECT_* bits.  The mig
 * generated server headers (hurd/io_S.h) are not installed on every
 * system; like trans/random.c we rely on trivfs.h alone. */
#include <hurd/trivfs.h>
#include <hurd/fsys.h>
#include <hurd/hurd_types.h>
#include <hurd/idvec.h>

/*****************************************************************************
 *  TRIVFS VARIABLES
 *
 *  These variables are read by libtrivfs to decide which operations to
 *  allow.  They are documented in <hurd/trivfs.h>.
 *****************************************************************************/

int trivfs_fstype = FSTYPE_MISC;
int trivfs_fsid = 0;

int trivfs_support_read = 1;
int trivfs_support_write = 1;
int trivfs_support_exec = 0;

int trivfs_allow_open = O_READ | O_WRITE;

/*****************************************************************************
 *  PER-OPEN STATE
 *
 *  Each open of the translator node gets its own file pointer, stored
 *  in the peropen hook.  The hook variables are function pointers that
 *  libtrivfs calls when peropens are created and destroyed.
 *****************************************************************************/

struct peropen_data
{
    loff_t file_pointer;    /* Current read/write position */
    char *snapshot;
    size_t snapshot_length;
};

static error_t
peropen_create(struct trivfs_peropen *po)
{
    po->hook = calloc(1, sizeof(struct peropen_data));
    return po->hook != NULL ? 0 : ENOMEM;
}

static void
peropen_destroy(struct trivfs_peropen *po)
{
    struct peropen_data *pod = po->hook;
    if (pod) free(pod->snapshot);
    free(po->hook);
    po->hook = NULL;
}

/* Install the peropen hooks before main runs, so they are in place
 * before trivfs_startup is called. */
static void __attribute__((constructor))
translator_init(void)
{
    trivfs_peropen_create_hook = peropen_create;
    trivfs_peropen_destroy_hook = peropen_destroy;
}

/*****************************************************************************
 *  NETWORK ACCESS
 *****************************************************************************/

/* Initialize the network with the default topology on first use */
static error_t
ensure_network(void)
{
    if (!global_network.initialized) {
        uint16_t layers[MAX_LAYERS] = DEFAULT_LAYER_SIZES;
        if (network_init(&global_network, DEFAULT_LAYER_COUNT, layers) != 0) {
            return errno;
        }
    }
    return 0;
}

/* Freeze a complete status view until this open is rewound or written. */
static error_t
ensure_snapshot(struct peropen_data *pod)
{
    if (!pod) return EIO;
    if (pod->snapshot) return 0;
    error_t error = ensure_network();
    if (error) return error;
    return network_status(&global_network, &pod->snapshot, &pod->snapshot_length);
}

/*****************************************************************************
 *  MANDATORY TRIVFS HOOKS
 *****************************************************************************/

/* Present the node as a regular file whose size is the status text.
 * io_statbuf_t (struct stat64) is the type used by the trivfs.h
 * prototype; using struct stat here conflicts without
 * -D_FILE_OFFSET_BITS=64. */
void
trivfs_modify_stat(struct trivfs_protid *cred, io_statbuf_t *st)
{
    st->st_mode &= ~((mode_t) S_IFMT);
    st->st_mode |= S_IFREG;
    struct peropen_data *pod = cred ? cred->po->hook : NULL;
    st->st_size = pod && !ensure_snapshot(pod) ? (loff_t)pod->snapshot_length : 0;
}

/* Someone (settrans -g, shutdown) wants us to go away */
error_t
trivfs_goaway(struct trivfs_control *cntl, int flags)
{
    (void) cntl;
    (void) flags;

    exit(0);
}

/*****************************************************************************
 *  IO SERVER ROUTINES
 *
 *  These override the libtrivfs defaults, which abort with assertions
 *  when trivfs_support_read/trivfs_support_write are set.
 *****************************************************************************/

/* Read data from the node.  OFFSET of -1 means read from the object
 * maintained file pointer. */
kern_return_t
trivfs_S_io_read(struct trivfs_protid *cred,
                 mach_port_t reply,
                 mach_msg_type_name_t replytype,
                 data_t *data,
                 mach_msg_type_number_t *datalen,
                 loff_t offs,
                 vm_size_t amount)
{
    size_t text_len;
    loff_t position;

    (void) reply;
    (void) replytype;

    if (cred == NULL)
        return EOPNOTSUPP;
    else if (!(cred->po->openmodes & O_READ))
        return EBADF;

    struct peropen_data *pod = cred->po->hook;
    error_t error = ensure_snapshot(pod);
    if (error) return error;
    text_len = pod->snapshot_length;

    /* Resolve the read position */
    position = offs;
    if (position == -1) {
        struct peropen_data *pod = cred->po->hook;
        position = pod != NULL ? pod->file_pointer : 0;
    }
    if (position < 0)
        return EINVAL;

    /* End of file */
    if ((uintmax_t) position >= text_len || amount == 0) {
        *datalen = 0;
        return 0;
    }

    if (amount > (vm_size_t)(text_len - (size_t) position))
        amount = (vm_size_t)(text_len - (size_t) position);

    /* Enlarge the reply buffer if the inline one is too small, as in
     * trans/random.c.  mig deallocates the buffer after the reply. */
    if (*datalen < amount) {
        *data = mmap(0, amount, PROT_READ | PROT_WRITE,
                      MAP_ANON | MAP_PRIVATE, -1, 0);
        if (*data == MAP_FAILED)
            return ENOMEM;
    }

    memcpy(*data, pod->snapshot + position, amount);
    *datalen = (mach_msg_type_number_t) amount;

    /* Advance the object-maintained file pointer */
    if (offs == -1) {
        struct peropen_data *pod = cred->po->hook;
        if (pod != NULL)
            pod->file_pointer = position + (loff_t) amount;
    }

    return 0;
}

/* Tell how much data can be read without blocking */
kern_return_t
trivfs_S_io_readable(struct trivfs_protid *cred,
                     mach_port_t reply,
                     mach_msg_type_name_t replytype,
                     vm_size_t *amount)
{
    (void) reply;
    (void) replytype;

    if (cred == NULL)
        return EOPNOTSUPP;
    else if (!(cred->po->openmodes & O_READ))
        return EBADF;

    struct peropen_data *pod = cred->po->hook;
    error_t error = ensure_snapshot(pod);
    if (error) return error;
    *amount = (uintmax_t)pod->file_pointer < pod->snapshot_length
        ? pod->snapshot_length - (size_t)pod->file_pointer : 0;
    return 0;
}

/* Write data to the node: this is how commands are sent */
kern_return_t
trivfs_S_io_write(struct trivfs_protid *cred,
                  mach_port_t reply,
                  mach_msg_type_name_t replytype,
                  const_data_t data,
                  mach_msg_type_number_t datalen,
                  loff_t offs,
                  vm_size_t *amt)
{
    (void) reply;
    (void) replytype;
    (void) offs;

    if (cred == NULL)
        return EOPNOTSUPP;
    else if (!(cred->po->openmodes & O_WRITE))
        return EBADF;

    if (datalen == 0) {
        *amt = 0;
        return 0;
    }

    /* Model paths resolve under the translator's credentials, not those
     * of a writer. Only its owner (or root) may perform filesystem I/O. */
    const char *command = data;
    size_t command_length = datalen;
    while (command_length && isspace((unsigned char)*command)) {
        command++;
        command_length--;
    }
    if (command_length > 4 && isspace((unsigned char)command[4]) &&
        (!memcmp(command, "save", 4) || !memcmp(command, "load", 4)) &&
        (!cred->user || (!idvec_contains(cred->user->uids, geteuid()) &&
                         !idvec_contains(cred->user->uids, 0)))) {
        *amt = 0;
        return EACCES;
    }

    *amt = 0;
    error_t error = ensure_network();
    if (!error) error = network_command(&global_network, data, datalen);
    if (error) return error;
    struct peropen_data *pod = cred->po->hook;
    if (pod) {
        free(pod->snapshot);
        pod->snapshot = NULL;
        pod->snapshot_length = 0;
        pod->file_pointer = 0;
    }
    *amt = datalen;
    return 0;
}

/* Change the current read/write offset */
kern_return_t
trivfs_S_io_seek(struct trivfs_protid *cred,
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

    loff_t base = 0;
    switch (whence) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = pod->file_pointer;
        break;
    case SEEK_END:
        {
            error_t error = ensure_snapshot(pod);
            if (error) return error;
            base = (loff_t)pod->snapshot_length;
        }
        break;
    default:
        return EINVAL;
    }

    if (offs > INT64_MAX - base || offs < -base)
        return EINVAL;
    offs += base;
    if (offs == 0) {
        free(pod->snapshot);
        pod->snapshot = NULL;
        pod->snapshot_length = 0;
    }
    pod->file_pointer = offs;
    *new_offs = offs;
    return 0;
}

/* SELECT_TYPE is the bitwise OR of SELECT_READ, SELECT_WRITE, and
 * SELECT_URG.  We are always ready for reading and writing. */
kern_return_t
trivfs_S_io_select(struct trivfs_protid *cred,
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
    if (!(cred->po->openmodes & O_READ)) *type &= ~SELECT_READ;
    if (!(cred->po->openmodes & O_WRITE)) *type &= ~SELECT_WRITE;

    return 0;
}

kern_return_t
trivfs_S_io_select_timeout(struct trivfs_protid *cred,
                           mach_port_t reply,
                           mach_msg_type_name_t replytype,
                           struct timespec ts,
                           int *type)
{
    (void) ts;
    return trivfs_S_io_select(cred, reply, replytype, type);
}

/* Truncate: accept and do nothing, the node has no fixed-size storage */
kern_return_t
trivfs_S_file_set_size(struct trivfs_protid *cred,
                       mach_port_t reply,
                       mach_msg_type_name_t replytype,
                       loff_t size)
{
    (void) reply;
    (void) replytype;

    if (cred == NULL)
        return EOPNOTSUPP;

    if (size < 0)
        return EINVAL;
    if (!(cred->po->openmodes & O_WRITE)) return EBADF;

    return 0;
}

/* These routines modify the O_APPEND, O_ASYNC, O_FSYNC, and O_NONBLOCK
 * bits for the IO object. */
kern_return_t
trivfs_S_io_set_all_openmodes(struct trivfs_protid *cred,
                             mach_port_t reply,
                             mach_msg_type_name_t replytype,
                             int mode)
{
    (void) reply;
    (void) replytype;

    if (cred == NULL)
        return EOPNOTSUPP;
    int mutable = O_APPEND | O_ASYNC | O_FSYNC | O_NONBLOCK;
    cred->po->openmodes = (cred->po->openmodes & ~mutable) | (mode & mutable);
    return 0;
}

kern_return_t
trivfs_S_io_set_some_openmodes(struct trivfs_protid *cred,
                               mach_port_t reply,
                               mach_msg_type_name_t replytype,
                               int bits)
{
    (void) reply;
    (void) replytype;

    if (cred == NULL)
        return EOPNOTSUPP;
    cred->po->openmodes |= bits & (O_APPEND | O_ASYNC | O_FSYNC | O_NONBLOCK);
    return 0;
}

kern_return_t
trivfs_S_io_clear_some_openmodes(struct trivfs_protid *cred,
                                 mach_port_t reply,
                                 mach_msg_type_name_t replytype,
                                 int bits)
{
    (void) reply;
    (void) replytype;

    if (cred == NULL)
        return EOPNOTSUPP;
    cred->po->openmodes &= ~(bits & (O_APPEND | O_ASYNC | O_FSYNC | O_NONBLOCK));
    return 0;
}

/*****************************************************************************
 *  NON-HURD FALLBACK
 *  The translator cannot run without libtrivfs.  These stubs exist only
 *  so that the main.c test harness links on non-Hurd systems.
 *****************************************************************************/

#else /* ON_HURD == 1 */

int trivfs_server_loop(void)
{
    log_debug_message("[DEBUG] trivfs_server_loop: STUB - not on Hurd!");
    return -1;
}

#endif /* ON_HURD */
