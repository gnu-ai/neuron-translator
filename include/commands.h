/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NEURON_COMMANDS_H
#define NEURON_COMMANDS_H
#include "neuron.h"

/* One complete command per write. Large vectors fit; oversized messages
 * fail explicitly. These helpers have no Hurd dependency. */
#define NETWORK_COMMAND_MAX (1024u * 1024u)
int network_command(CompactNeuralNetwork *net, const char *data, size_t length);
int network_status(const CompactNeuralNetwork *net, char **text, size_t *length);
#endif
