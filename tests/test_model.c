/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_model.c — conformance tests of the compute core (neuron.c).
 *
 * The .nn format is the contract between this translator and the
 * external trainer (see README, "The .nn Model Format"): these tests
 * pin down the promises the rest of the stack relies on:
 *
 *   - the pseudo-random initialization is deterministic and bounded,
 *     exactly as documented (a wiring placeholder, not a trainer);
 *   - save / load round-trips weights, biases and outputs untouched;
 *   - the file really carries the magic and the format version, and
 *     a file of another version is rejected, leaving errno EINVAL.
 *
 * Pure C23/POSIX: no Hurd, no trivfs, no network — the suite runs
 * on any system, like the parser tests of httpfs-translator.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neuron.h"

static int failures = 0;

/* check — report one named assertion; the harness stays quiet about
 * what passes and names what does not. */
static void check (const char *name, int ok)
{
    printf ("%s: %s\n", name, ok ? "OK" : "FAIL");
    if (!ok)
        failures++;
}

int main (void)
{
    /* --- Two fresh networks must be seeded identically -------- */
    uint16_t layers[MAX_LAYERS] = DEFAULT_LAYER_SIZES;
    CompactNeuralNetwork a, b;

    if (network_init (&a, DEFAULT_LAYER_COUNT, layers) != 0)
        {
            perror ("network_init");
            return 2;
        }
    if (network_init (&b, DEFAULT_LAYER_COUNT, layers) != 0)
        {
            perror ("network_init");
            return 2;
        }
    check ("deterministic init",
           memcmp (a.weights, b.weights,
                   a.total_weights * sizeof (float)) == 0);

    /* --- The documented seed range is [-0.2, 0.2] -------------- */
    int in_range = 1;
    for (size_t i = 0; i < a.total_weights; i++)
        if (a.weights[i] < -0.2f - 1e-6f || a.weights[i] > 0.2f + 1e-6f)
            in_range = 0;
    check ("weight range [-0.2, 0.2]", in_range);

    /* --- Forward pass, then save / load round-trip ------------ */
    for (uint16_t i = 0; i < a.topology.input_size; i++)
        a.input_buffer[i] =
            (float) (i + 1) / (float) (a.topology.input_size + 1);
    network_forward (&a);

    float out1[64];
    memcpy (out1, a.output_buffer,
            a.topology.output_size * sizeof (float));

    const char *path = "test-model.nn";
    check ("save v2", network_save (&a, path));

    memset (&b, 0, sizeof (b));
    check ("load v2", network_load (&b, path));
    check ("same topology after load",
           b.topology.layer_count == a.topology.layer_count
               && b.topology.output_size == a.topology.output_size);
    check ("same weights after load",
           memcmp (a.weights, b.weights,
                   a.total_weights * sizeof (float)) == 0);
    check ("same biases after load",
           memcmp (a.biases, b.biases,
                   a.total_biases * sizeof (float)) == 0);

    memcpy (b.input_buffer, a.input_buffer,
            a.topology.input_size * sizeof (float));
    network_forward (&b);
    check ("same outputs after round-trip",
           memcmp (out1, b.output_buffer,
                   a.topology.output_size * sizeof (float)) == 0);

    /* --- The header really is magic + version ----------------- */
    FILE *fp = fopen (path, "rb");
    unsigned char hdr[8] = { 0 };
    if (fp == nullptr || fread (hdr, 1, sizeof (hdr), fp) != sizeof (hdr))
        {
            perror ("read header");
            return 2;
        }
    fclose (fp);

    unsigned magic = (unsigned) hdr[0] | ((unsigned) hdr[1] << 8)
                   | ((unsigned) hdr[2] << 16) | ((unsigned) hdr[3] << 24);
    unsigned version = (unsigned) hdr[4] | ((unsigned) hdr[5] << 8);
    check ("magic NEUR", magic == NET_FILE_MAGIC);
    check ("format version 2", version == NET_FILE_VERSION);

    /* --- A version-1 file must be rejected -------------------- */
    fp = fopen (path, "r+b");
    if (fp == nullptr)
        {
            perror ("reopen");
            return 2;
        }
    unsigned forged = 1;
    fseek (fp, 4, SEEK_SET);
    fwrite (&forged, sizeof (uint16_t), 1, fp);
    fclose (fp);

    CompactNeuralNetwork c;
    errno = 0;
    int rejected = !network_load (&c, path) && errno == EINVAL;
    check ("version 1 rejected", rejected);

    network_free (&a);
    network_free (&b);
    remove (path);

    printf ("%s\n", failures ? "SUITE FAIL" : "SUITE OK");
    return failures;
}
