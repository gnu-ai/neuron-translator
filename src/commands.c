/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "commands.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int network_command(CompactNeuralNetwork *net, const char *data, size_t length)
{
    if (!net || !data) return EINVAL;
    if (!length) return 0;
    if (length > NETWORK_COMMAND_MAX) return EMSGSIZE;
    if (memchr(data, '\0', length)) return EINVAL;
    char *command = malloc(length + 1);
    if (!command) return ENOMEM;
    memcpy(command, data, length);
    command[length] = '\0';
    char *tail = command;
    for (char *scan = command; *scan; scan++)
        if (!isspace((unsigned char)*scan)) tail = scan + 1;
    *tail = '\0';
    char *p = command;
    while (*p == ' ' || *p == '\t') p++;
    int error = EINVAL;
    if (!*p || strchr(p, '\n') || strchr(p, '\r')) goto done;

    if (!strcmp(p, "reset")) {
        if (!net->initialized) goto done;
        network_reset(net);
        error = 0;
        goto done;
    }
    /* Explicit verbs disambiguate integer inputs from layer sizes. The
     * legacy numeric form keeps its historical topology-first meaning. */
    char *argument = p;
    while (*argument && !isspace((unsigned char)*argument)) argument++;
    if (*argument) {
        char saved = *argument;
        *argument++ = '\0';
        while (isspace((unsigned char)*argument)) argument++;
        if (!strcmp(p, "save") || !strcmp(p, "load")) {
            errno = 0;
            bool ok = *argument && (!strcmp(p, "save")
                ? network_save(net, argument) : network_load(net, argument));
            error = ok ? 0 : (errno ? errno : EINVAL);
            goto done;
        }
        if (!strcmp(p, "input")) {
            error = parse_input_string(net, argument) ? 0 : EINVAL;
            goto done;
        }
        if (!strcmp(p, "topology")) {
            uint16_t layers[MAX_LAYERS];
            int count = parse_config_string(argument, layers, MAX_LAYERS);
            if (count > 0)
                error = network_init(net, (uint8_t)count, layers) == 0 ? 0 : errno;
            goto done;
        }
        /* Restore the original string for the legacy parser. */
        char *end = p + strlen(p);
        *end = saved;
    }
    uint16_t layers[MAX_LAYERS];
    int count = parse_config_string(p, layers, MAX_LAYERS);
    if (count > 0)
        error = network_init(net, (uint8_t)count, layers) == 0 ? 0 : errno;
    else
        error = parse_input_string(net, p) ? 0 : EINVAL;
done:
    free(command);
    return error;
}

static int append(char *text, size_t capacity, size_t *length,
                  const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vsnprintf(text + *length, capacity - *length, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= capacity - *length) return EOVERFLOW;
    *length += (size_t)count;
    return 0;
}

int network_status(const CompactNeuralNetwork *net, char **text, size_t *length)
{
    if (!net || !net->initialized || !text || !length) return EINVAL;
    size_t capacity = 2048 + (size_t)net->topology.output_size * 80;
    char *buffer = malloc(capacity);
    if (!buffer) return ENOMEM;
    size_t used = 0;
    int error;
#define ADD(...) do { error = append(buffer, capacity, &used, __VA_ARGS__); \
                     if (error) goto fail; } while (0)
    ADD("LLM Sigmoid Neuron Translator - GNU Hurd\n"
        "===========================================\n\nNetwork: %u layers",
        (unsigned)net->topology.layer_count);
    for (unsigned i = 0; i < net->topology.layer_count; i++)
        ADD(", %u", (unsigned)net->topology.layer_sizes[i]);
    ADD("\n\nMemory: %.2f KB, Neurons: %zu, Weights: %zu\n\n"
        "Parameters:\n  Reset Potential: %.2f mV\n\nOutput:\n",
        (double)net->memory_block_size / 1024, net->total_neurons,
        net->total_weights, (double)net->topology.reset_potential);
    for (size_t i = 0; i < net->topology.output_size; i++)
        ADD("  [%zu]: %.6f\n", i, (double)net->output_buffer[i]);
    ADD("\nStatistics:\n  Forward Passes: %zu\n  Neuron Activations: %zu\n\n"
        "Commands (one complete command per write):\n"
        "  topology 10,20,5\n  input v1,...,v%u\n"
        "  reset\n  save /path/model.nn\n  load /path/model.nn\n",
        net->forward_pass_count, net->neuron_activations,
        (unsigned)net->topology.input_size);
#undef ADD
    *text = buffer;
    *length = used;
    return 0;
fail:
    free(buffer);
    return error;
}
