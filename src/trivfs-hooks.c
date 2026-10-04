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
/* GNU Mach 1.8+git20260224 (the Debian forky/sid snapshot): the
 * installed mach_host.h uses processor_name_array_t, but no
 * installed header defines it — the MIG header generation of
 * this snapshot drops the typedef.  Provide the canonical
 * definition (an array of processor_info_t) BEFORE every Mach
 * include (the project's own trivfs-hooks.h pulls <mach.h>);
 * C tolerates the identical redefinition the day the snapshot is
 * fixed. */
#include <mach/processor_info.h>
typedef processor_info_t *processor_name_array_t;
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

/* Size of the buffer used to render the status text */
#define INFO_BUFFER_SIZE 4096

/* Initialize the network with the default topology on first use */
static void
ensure_network(void)
{
    if (!global_network.initialized) {
        uint16_t layers[MAX_LAYERS] = DEFAULT_LAYER_SIZES;
        if (network_init(&global_network, DEFAULT_LAYER_COUNT, layers) != 0) {
            log_debug_message("[DEBUG] network_init failed");
        }
    }
}

/* Render the status text served by reads, return its length.
 * The text is always NUL-terminated within the buffer. */
static size_t
build_info_text(char *buffer, size_t buffer_size)
{
    size_t written = 0;
    int result;

    result = snprintf(buffer, buffer_size,
                      "LLM Sigmoid Neuron Translator - GNU Hurd\n"
                      "===========================================\n\n");
    if (result < 0) return 0;
    written = (size_t)result;

    result = snprintf(buffer + written, buffer_size - written,
                      "Network: %d layers",
                      (int) global_network.topology.layer_count);
    if (result < 0) return written;
    written += (size_t)result;

    for (uint8_t i = 0; i < global_network.topology.layer_count; i++) {
        result = snprintf(buffer + written, buffer_size - written,
                          ", %d", (int) global_network.topology.layer_sizes[i]);
        if (result < 0) return written;
        written += (size_t)result;
    }

    result = snprintf(buffer + written, buffer_size - written, "\n\n");
    if (result < 0) return written;
    written += (size_t)result;

    result = snprintf(buffer + written, buffer_size - written,
                      "Memory: %.2f KB, Neurons: %zu, Weights: %zu\n\n",
                      (double) global_network.memory_block_size / 1024.0,
                      global_network.total_neurons,
                      global_network.total_weights);
    if (result < 0) return written;
    written += (size_t)result;

    result = snprintf(buffer + written, buffer_size - written,
                      "Parameters:\n"
                      "  Reset Potential: %.2f mV\n\n",
                      (double) global_network.topology.reset_potential);
    if (result < 0) return written;
    written += (size_t)result;

    result = snprintf(buffer + written, buffer_size - written, "Output:\n");
    if (result < 0) return written;
    written += (size_t)result;

    for (size_t i = 0; i < global_network.topology.output_size; i++) {
        result = snprintf(buffer + written, buffer_size - written,
                          "  [%zu]: %.6f\n", i,
                          (double) global_network.output_buffer[i]);
        if (result < 0) return written;
        written += (size_t)result;
    }

    result = snprintf(buffer + written, buffer_size - written,
                      "\nStatistics:\n"
                      "  Forward Passes: %zu\n"
                      "  Neuron Activations: %zu\n\n",
                      global_network.forward_pass_count,
                      global_network.neuron_activations);
    if (result < 0) return written;
    written += (size_t)result;

    result = snprintf(buffer + written, buffer_size - written,
                      "Usage:\n"
                      "  cat /llm                          - Show info\n"
                      "  echo '10,20,5' | sudo tee /llm  - Set topology\n"
                      "  echo 'v1,...,v%u' | sudo tee /llm - Set input (%u values)\n"
                      "  echo 'reset' | sudo tee /llm      - Reset network state\n"
                      "  echo 'save /tmp/net.bin' | sudo tee /llm - Save network\n"
                      "  echo 'load /tmp/net.bin' | sudo tee /llm - Load network\n"
                      "(node owned by root: use '| sudo tee', or 'sudo chmod 666 /llm' once)\n",
                      (unsigned) global_network.topology.input_size,
                      (unsigned) global_network.topology.input_size);
    if (result < 0) return written;
    written += (size_t)result;

    if (written >= buffer_size) {
        written = buffer_size - 1;
    }
    buffer[written] = '\0';

    return written;
}

/* Current length of the status text */
static size_t
info_text_size(void)
{
    char buffer[INFO_BUFFER_SIZE];
    return build_info_text(buffer, sizeof buffer);
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
    (void) cred;

    st->st_mode &= ~((mode_t) S_IFMT);
    st->st_mode |= S_IFREG;
    st->st_size = (loff_t) info_text_size();
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
    char buffer[INFO_BUFFER_SIZE];
    size_t text_len;
    loff_t position;

    (void) reply;
    (void) replytype;

    if (cred == NULL)
        return EOPNOTSUPP;
    else if (!(cred->po->openmodes & O_READ))
        return EBADF;

    ensure_network();
    text_len = build_info_text(buffer, sizeof buffer);

    /* Resolve the read position */
    position = offs;
    if (position == -1) {
        struct peropen_data *pod = cred->po->hook;
        position = pod != NULL ? pod->file_pointer : 0;
    }
    if (position < 0)
        position = 0;

    /* End of file */
    if ((size_t) position >= text_len || amount == 0) {
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

    memcpy(*data, buffer + position, amount);
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

    *amount = vm_page_size;
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
    char temp[1024];
    size_t len;

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

    ensure_network();

    len = datalen;
    if (len >= sizeof temp)
        len = sizeof temp - 1;

    memcpy(temp, data, len);
    temp[len] = '\0';

    /* Remove trailing newlines */
    char *newline = strchr(temp, '\n');
    if (newline != NULL) *newline = '\0';
    newline = strchr(temp, '\r');
    if (newline != NULL) *newline = '\0';

    /* Handle commands */
    if (strncmp(temp, "reset", 5) == 0
        && (temp[5] == '\0' || temp[5] == ' ' || temp[5] == '\t')) {
        network_reset(&global_network);
        *amt = datalen;
        return 0;
    }

    if (strncmp(temp, "save ", 5) == 0) {
        char *filename = temp + 5;
        if (*filename != '\0' && network_save(&global_network, filename)) {
            *amt = datalen;
            return 0;
        }
        *amt = 0;
        return EINVAL;
    }

    if (strncmp(temp, "load ", 5) == 0) {
        char *filename = temp + 5;
        if (*filename != '\0' && network_load(&global_network, filename)) {
            *amt = datalen;
            return 0;
        }
        *amt = 0;
        return EINVAL;
    }

    /* Parse a topology specification such as "10,20,5" */
    uint16_t layers[MAX_LAYERS];
    int layer_count = parse_config_string(temp, layers, MAX_LAYERS);

    if (layer_count > 0) {
        network_free(&global_network);
        if (network_init(&global_network, (uint8_t) layer_count, layers) != 0) {
            *amt = 0;
            return EINVAL;
        }
        *amt = datalen;
        return 0;
    }

    /* Parse an input vector and run a forward pass */
    if (parse_input_string(&global_network, temp)) {
        *amt = datalen;
        return 0;
    }

    *amt = 0;
    return EINVAL;
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

    ensure_network();

    switch (whence) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        offs += pod->file_pointer;
        break;
    case SEEK_END:
        offs += (loff_t) info_text_size();
        break;
    default:
        return EINVAL;
    }

    if (offs < 0)
        return EINVAL;

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
    (void) mode;

    if (cred == NULL)
        return EOPNOTSUPP;

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
    (void) bits;

    if (cred == NULL)
        return EOPNOTSUPP;

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
    (void) bits;

    if (cred == NULL)
        return EOPNOTSUPP;

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
