/*
 * neuron.c - Neural Network Implementation for LLM Translator
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

/** @file neuron.c
 *  @brief Neural network implementation with memory-efficient design
 *
 *  This file implements a compact sigmoid neural network for the GNU Hurd
 *  translator. It follows Claude Delannoy's educational style with extensive
 *  comments explaining each algorithmic step.
 */

/*****************************************************************************
 *                                                                           *
 *                      MODEL PERSISTENCE — LOAD                             *
 *                                                                           *
 *  Reconstructs a network from a .nn file written by network_save().        *
 *                                                                           *
 *  Every field read from disk is treated as UNTRUSTED input. The load is    *
 *  performed as a transaction: nothing is written into @p net until the     *
 *  whole file has been validated and the arena allocated. On any error      *
 *  @p net is left in its previous state.                                    *
 *                                                                           *
 *  Security rationale (Hurd translator context):                            *
 *  ------------------------------------------------                         *
 *  A translator may be started by any user via settrans(8). If the model    *
 *  file is attacker-controlled, a naive fread()-into-struct loader can      *
 *  cause:                                                                   *
 *    - stack/heap buffer overflow via oversized layer_count,                *
 *    - integer overflow in the arena size computation,                      *
 *    - heap overflow when fread() writes past a too-small allocation.       *
 *                                                                           *
 *  This implementation closes all three classes by:                         *
 *    (a) validating layer_count against MAX_LAYERS before any array write,  *
 *    (b) recomputing totals from the topology and cross-checking them,      *
 *    (c) using overflow-safe size arithmetic and NET_MAX_TOTAL_* guards.    *
 *                                                                           *
 *  Caller contract:                                                         *
 *  ----------------                                                         *
 *    CompactNeuralNetwork net = {0};   (zero-init on first use)             *
 *    if (!network_load(&net, path))    (check errno)                         *
 *                                                                           *
 *                                                                           *
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "neuron.h"


/*****************************************************************************
 *                                                                           *
 *                        MEMORY MANAGEMENT                                *
 *                                                                           *
 *  Uses posix_memalign for SIMD-compatible allocations                    *
 *                                                                           *
 *****************************************************************************/

/**
 * @brief Aligned memory allocation
 * 
 * Allocates memory with specified alignment for SIMD operations.
 * 
 * @param size      Size of memory to allocate
 * @param alignment Alignment requirement (e.g., 16 for SIMD)
 * @return          Pointer to allocated memory, or NULL on failure
 */
static void *aligned_malloc(size_t size, size_t alignment)
{
    void *ptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return NULL;
    }
    return ptr;
}

/**
 * @brief Aligned memory deallocation
 * 
 * @param ptr Pointer to memory to free
 */
static void aligned_free(void *ptr)
{
    free(ptr);
}


/*****************************************************************************
 *                                                                           *
 *                      NETWORK INITIALIZATION                              *
 *                                                                           *
 *  Initializes network topology and allocates contiguous memory block        *
 *                                                                           *
 *****************************************************************************/

int network_init(CompactNeuralNetwork *net,
                uint8_t layer_count,
                const uint16_t *layer_sizes)
{
    /* Input validation */
    if (!net || !layer_sizes) {
        errno = EINVAL;
        return -1;
    }
    
    /* Validate layer count */
    if (layer_count < 2 || layer_count > MAX_LAYERS) {
        errno = EINVAL;
        return -1;
    }
    
    /* Validate each layer size */
    for (uint8_t i = 0; i < layer_count; i++) {
        if (layer_sizes[i] == 0 || layer_sizes[i] > MAX_NEURONS_PER_LAYER) {
            errno = EINVAL;
            return -1;
        }
    }
    
    /* Store topology */
    net->topology.layer_count = layer_count;
    net->topology.input_size = layer_sizes[0];
    net->topology.output_size = layer_sizes[layer_count - 1];
    
    for (uint8_t i = 0; i < layer_count; i++) {
        net->topology.layer_sizes[i] = layer_sizes[i];
    }
    
    /* Set the initial scratch voltage (no other neuron parameters
     * exist: the model is a pure sigmoid feedforward pass). */
    net->topology.reset_potential = RESET_POTENTIAL;
    
    /* Calculate total neuron count */
    net->total_neurons = 0;
    for (uint8_t i = 0; i < layer_count; i++) {
        net->total_neurons += layer_sizes[i];
    }
    
    /* Calculate total weights (sum of layer_i * layer_{i-1}) */
    net->total_weights = 0;
    net->total_biases = 0;
    for (uint8_t i = 1; i < layer_count; i++) {
        net->total_weights += (size_t)layer_sizes[i] * (size_t)layer_sizes[i - 1];
        net->total_biases += layer_sizes[i];
    }
    
    /* Calculate layer offsets */
    net->layer_offsets[0] = 0;
    for (uint8_t i = 1; i < layer_count; i++) {
        net->layer_offsets[i] = net->layer_offsets[i - 1] + layer_sizes[i - 1];
    }
    
    /* Calculate weight offsets */
    if (layer_count > 1) {
        net->weight_offsets[0] = 0;
        for (uint8_t i = 1; i < layer_count - 1; i++) {
            net->weight_offsets[i] = net->weight_offsets[i - 1] +
                                      (size_t)layer_sizes[i] * (size_t)layer_sizes[i - 1];
        }
    }
    
    /* Calculate bias offsets */
    net->bias_offsets[0] = 0;
    for (uint8_t i = 1; i < layer_count; i++) {
        net->bias_offsets[i] = net->bias_offsets[i - 1] + layer_sizes[i];
    }
    
    /* Calculate memory requirements */
    size_t voltages_size = net->total_neurons * sizeof(float);
    size_t weights_size = net->total_weights * sizeof(float);
    size_t biases_size = net->total_biases * sizeof(float);
    size_t input_size = layer_sizes[0] * sizeof(float);
    size_t output_size = layer_sizes[layer_count - 1] * sizeof(float);
    
    net->memory_block_size = voltages_size + weights_size + biases_size +
                             input_size + output_size;
    
    /* Allocate contiguous memory block */
    net->memory_block = aligned_malloc(net->memory_block_size, SIMD_ALIGNMENT);
    if (!net->memory_block) {
        errno = ENOMEM;
        return -1;
    }
    
    /* Set up pointers into the memory block */
    char *ptr = net->memory_block;
    net->voltages = (float *)ptr;
    ptr += voltages_size;
    net->weights = (float *)ptr;
    ptr += weights_size;
    net->biases = (float *)ptr;
    ptr += biases_size;
    net->input_buffer = (float *)ptr;
    ptr += input_size;
    net->output_buffer = (float *)ptr;
    
    /* Initialize voltages to reset potential */
    for (size_t i = 0; i < net->total_neurons; i++) {
        net->voltages[i] = net->topology.reset_potential;
    }
    
    /* Initialize weights with small random values */
    for (size_t i = 0; i < net->total_weights; i++) {
        /* Simple deterministic pseudo-random initialization */
        uint32_t seed = (uint32_t)i * 2654435761U; /* Knuth's multiplicative constant */
        float random = (float)(seed & 0x007FFFFF) / (float)0x007FFFFF;
        net->weights[i] = random * 0.4f - 0.2f; /* Range: [-0.2, 0.2] */
    }
    
    /* Initialize biases to zero */
    for (size_t i = 0; i < net->total_biases; i++) {
        net->biases[i] = 0.0f;
    }
    
    /* Set initialization flag */
    net->initialized = true;
    net->needs_reset = false;
    net->forward_pass_count = 0;
    net->neuron_activations = 0;
    
    return 0;
}


/*****************************************************************************
 *                                                                           *
 *                        NETWORK OPERATIONS                               *
 *                                                                           *
 *****************************************************************************/

void network_free(CompactNeuralNetwork *net)
{
    if (!net) return;
    
    if (net->memory_block) {
        aligned_free(net->memory_block);
        net->memory_block = NULL;
    }
    
    /* Clear all pointers */
    net->voltages = NULL;
    net->weights = NULL;
    net->biases = NULL;
    net->input_buffer = NULL;
    net->output_buffer = NULL;
    
    net->initialized = false;
    net->needs_reset = false;
}


void network_reset(CompactNeuralNetwork *net)
{
    if (!net || !net->initialized) return;
    
    /* Reset all neuron voltages to resting potential */
    for (size_t i = 0; i < net->total_neurons; i++) {
        net->voltages[i] = net->topology.reset_potential;
    }
    
    /* Reset counters */
    net->forward_pass_count = 0;
    net->neuron_activations = 0;
    net->needs_reset = false;
}


void network_forward(CompactNeuralNetwork *net)
{
    if (!net || !net->initialized || net->topology.layer_count < 2) {
        return;
    }
    
    /* Copy input buffer to first layer voltages */
    if (net->input_buffer) {
        memcpy(net->voltages, net->input_buffer,
               net->topology.input_size * sizeof(float));
    }
    
    /* Process each layer (starting from first hidden layer) */
    for (int layer = 1; layer < net->topology.layer_count; layer++) {
        size_t prev_size = net->topology.layer_sizes[layer - 1];
        size_t curr_size = net->topology.layer_sizes[layer];
        size_t prev_offset = net->layer_offsets[layer - 1];
        size_t curr_offset = net->layer_offsets[layer];
        
        /* Walk the layer table to find where this layer's weight matrix
         * starts.  The running sum adds up the sizes of all the weight
         * matrices that precede layer. */
        size_t weight_offset = 0;
        for (int l = 1; l < layer; l++) {
            weight_offset += (size_t)net->topology.layer_sizes[l] *
                            (size_t)net->topology.layer_sizes[l - 1];
        }
        
        size_t bias_offset = net->bias_offsets[layer - 1];
        
        /* Process each neuron in current layer */
        for (size_t n = 0; n < curr_size; n++) {
            /* Start with bias */
            float sum = net->biases[bias_offset + n];
            
            /* Dot product of the previous layer's activations (v) with
             * this neuron's row of the weight matrix (w): the heart of
             * the forward pass. */
            float *w = net->weights + weight_offset + n * prev_size;
            float *v = net->voltages + prev_offset;
            
            for (size_t p = 0; p < prev_size; p++) {
                sum += v[p] * w[p];
            }
            
            /* Apply sigmoid activation */
            net->voltages[curr_offset + n] = sigmoidf(sum);
            net->neuron_activations++;
        }
    }
    
    /* Copy output layer to output buffer */
    if (net->output_buffer) {
        size_t out_offset = net->layer_offsets[net->topology.layer_count - 1];
        memcpy(net->output_buffer, net->voltages + out_offset,
               net->topology.output_size * sizeof(float));
    }
    
    net->forward_pass_count++;
}


/*****************************************************************************
 *                                                                           *
 *                     CONFIGURATION PARSING                                *
 *                                                                           *
 *****************************************************************************/

int parse_config_string(const char *config_str,
                       uint16_t *layer_sizes,
                       int max_layers)
{
    if (!config_str || !layer_sizes || max_layers < 2) {
        errno = EINVAL;
        return -1;
    }
    
    const char *ptr = config_str;
    int count = 0;
    
    /* Parse comma or space separated values */
    while (*ptr != '\0' && count < max_layers) {
        /* Skip separators */
        while (*ptr == ' ' || *ptr == '\t' || *ptr == ',') ptr++;
        if (*ptr == '\0') break;
        
        /* Parse integer */
        errno = 0;
        char *endptr;
        long value = strtol(ptr, &endptr, 10);
        
        if (ptr == endptr || errno == ERANGE) {
            /* No digit found or out of range */
            errno = EINVAL;
            return -1;
        }
        
        /* Validate range */
        if (value < 1 || value > MAX_NEURONS_PER_LAYER) {
            errno = EINVAL;
            return -1;
        }
        
        layer_sizes[count++] = (uint16_t)value;
        ptr = endptr;
    }
    
    /* Must have at least 2 layers */
    if (count < 2) {
        errno = EINVAL;
        return -1;
    }
    
    return count;
}


bool parse_input_string(CompactNeuralNetwork *net,
                       const char *input_str)
{
    if (!net || !net->initialized || !input_str) {
        return false;
    }
    
    const char *ptr = input_str;
    size_t idx = 0;
    
    /* Parse comma or space separated values */
    while (*ptr != '\0' && idx < net->topology.input_size) {
        /* Skip separators */
        while (*ptr == ',' || *ptr == ' ' || *ptr == '\t' || *ptr == '\n') ptr++;
        if (*ptr == '\0') break;
        
        /* Parse float */
        errno = 0;
        char *endptr;
        float val = strtof(ptr, &endptr);
        
        if (ptr == endptr || errno == ERANGE) {
            /* Invalid number - fail parsing */
            return false;
        }
        
        net->input_buffer[idx++] = val;
        ptr = endptr;
    }
    
    /* Only execute forward pass if we got all required inputs */
    if (idx == net->topology.input_size) {
        network_forward(net);
        return true;
    }
    
    return false;
}


/*****************************************************************************
 *                                                                           *
 *                      FILE PERSISTENCE                                    *
 *                                                                           *
 *****************************************************************************/

bool network_save(const CompactNeuralNetwork *net, const char *filename)
{
    if (!net || !net->initialized || !filename)
        return false;

    FILE *fp = fopen(filename, "wb");
    if (!fp)
        return false;

    /* --- File header: magic number and format version --- */
    uint32_t magic   = NET_FILE_MAGIC;
    uint16_t version = NET_FILE_VERSION;
    if (fwrite(&magic,   sizeof(magic),   1, fp) != 1) { fclose(fp); return false; }
    if (fwrite(&version, sizeof(version), 1, fp) != 1) { fclose(fp); return false; }
    
    /* Write topology */
    if (fwrite(&net->topology, sizeof(NetworkTopology), 1, fp) != 1) {
        fclose(fp); return false;
    }
    
    /* Write counts */
    if (fwrite(&net->total_neurons, sizeof(size_t), 1, fp) != 1) {
        fclose(fp); return false;
    }
    if (fwrite(&net->total_weights, sizeof(size_t), 1, fp) != 1) {
        fclose(fp); return false;
    }
    if (fwrite(&net->total_biases, sizeof(size_t), 1, fp) != 1) {
        fclose(fp); return false;
    }
    
    /* Write offsets */
    if (fwrite(net->layer_offsets, sizeof(size_t),
               net->topology.layer_count, fp) != net->topology.layer_count) {
        fclose(fp); return false;
    }
    
    if (net->topology.layer_count > 1) {
        if (fwrite(net->weight_offsets, sizeof(size_t),
                   net->topology.layer_count - 1, fp) !=
            (size_t)(net->topology.layer_count - 1)) {
            fclose(fp); return false;
        }
    }
    
    if (fwrite(net->bias_offsets, sizeof(size_t),
               net->topology.layer_count, fp) != net->topology.layer_count) {
        fclose(fp); return false;
    }
    
    /* Write data arrays */
    if (fwrite(net->voltages, sizeof(float), net->total_neurons, fp) !=
        net->total_neurons) {
        fclose(fp); return false;
    }
    
    if (fwrite(net->weights, sizeof(float), net->total_weights, fp) !=
        net->total_weights) {
        fclose(fp); return false;
    }
    
    if (fwrite(net->biases, sizeof(float), net->total_biases, fp) !=
        net->total_biases) {
        fclose(fp); return false;
    }
    
    fclose(fp);
    return true;
}


bool network_load(CompactNeuralNetwork *net, const char *filename)
{
    if (!net || !filename) {
        errno = EINVAL;
        return false;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp)
        return false;

    /* Load as a transaction: read everything into LOCAL variables, validate
     * each field, and only commit the result to *net as the very last step.
     * A half-loaded or inconsistent network must never become visible. */
    uint32_t         magic   = 0;
    uint16_t         version = 0;
    NetworkTopology  topo;
    void            *block   = NULL;

    /* ---- 1. File header ---- */
    if (fread(&magic,   sizeof(magic),   1, fp) != 1) goto fail;
    if (fread(&version, sizeof(version), 1, fp) != 1) goto fail;
    if (magic != NET_FILE_MAGIC || version != NET_FILE_VERSION) {
        errno = EINVAL;
        goto fail;
    }

    if (fread(&topo, sizeof(topo), 1, fp) != 1) goto fail;

    /* ---- 2. Validate the topology BEFORE using any of it ---- */
    if (topo.layer_count < 2 || topo.layer_count > MAX_LAYERS) {
        errno = EINVAL;
        goto fail;
    }

    size_t total_neurons = 0;
    for (uint8_t i = 0; i < topo.layer_count; i++) {
        uint16_t s = topo.layer_sizes[i];
        if (s == 0 || s > MAX_NEURONS_PER_LAYER) {
            errno = EINVAL;
            goto fail;
        }
        total_neurons += s;
    }

    if (topo.input_size  != topo.layer_sizes[0] ||
        topo.output_size != topo.layer_sizes[topo.layer_count - 1]) {
        errno = EINVAL;
        goto fail;
    }

    /* Recompute the weight and bias totals from the layer table: a file
     * is never allowed to tell us how large it is, we always derive the
     * sizes ourselves from the topology. */
    size_t total_weights = 0;
    size_t total_biases  = 0;
    for (uint8_t i = 1; i < topo.layer_count; i++) {
        size_t w = (size_t)topo.layer_sizes[i] * topo.layer_sizes[i - 1];
        if (total_weights + w < total_weights) {  /* overflow check */
            errno = EOVERFLOW;
            goto fail;
        }
        total_weights += w;
        total_biases  += topo.layer_sizes[i];
    }

    /* Enforce the global upper limits */
    if (total_neurons > NET_MAX_TOTAL_NEURONS ||
        total_weights > NET_MAX_TOTAL_WEIGHTS ||
        total_biases  > NET_MAX_TOTAL_BIASES) {
        errno = EFBIG;
        goto fail;
    }

    /* ---- 3. Do the counts stored in the file match our own? ---- */
    size_t f_neurons = 0, f_weights = 0, f_biases = 0;
    if (fread(&f_neurons, sizeof(size_t), 1, fp) != 1) goto fail;
    if (fread(&f_weights, sizeof(size_t), 1, fp) != 1) goto fail;
    if (fread(&f_biases,  sizeof(size_t), 1, fp) != 1) goto fail;

    if (f_neurons != total_neurons ||
        f_weights != total_weights ||
        f_biases  != total_biases) {
        errno = EINVAL;
        goto fail;
    }

    /* ---- 4. Read the offset tables into TEMPORARY arrays ---- */
    size_t layer_offsets [MAX_LAYERS];
    size_t weight_offsets[MAX_LAYERS];
    size_t bias_offsets  [MAX_LAYERS];

    if (fread(layer_offsets, sizeof(size_t),
              topo.layer_count, fp) != topo.layer_count) goto fail;

    if (topo.layer_count > 1) {
        if (fread(weight_offsets, sizeof(size_t),
                  topo.layer_count - 1, fp) !=
            (size_t)(topo.layer_count - 1)) goto fail;
    }

    if (fread(bias_offsets, sizeof(size_t),
              topo.layer_count, fp) != topo.layer_count) goto fail;

    /* Defensive check: each layer must start strictly after the previous
     * one, i.e. the offsets must grow monotonically. */
    for (uint8_t i = 1; i < topo.layer_count; i++) {
        if (layer_offsets[i] <= layer_offsets[i - 1] ||
            bias_offsets[i]  <= bias_offsets[i - 1]) {
            errno = EINVAL;
            goto fail;
        }
    }
    if (layer_offsets[topo.layer_count - 1] + topo.layer_sizes[topo.layer_count - 1]
            != total_neurons) {
        errno = EINVAL;
        goto fail;
    }

    /* ---- 5. Compute the arena size, guarding against overflow ---- */
    const size_t elem = sizeof(float);
    if (total_neurons > SIZE_MAX / elem ||
        total_weights > SIZE_MAX / elem ||
        total_biases  > SIZE_MAX / elem) {
        errno = EOVERFLOW;
        goto fail;
    }

    size_t mem_size =
        total_neurons * elem +
        total_weights * elem +
        total_biases  * elem +
        (size_t)topo.input_size  * elem +
        (size_t)topo.output_size * elem;

    block = aligned_malloc(mem_size, SIMD_ALIGNMENT);
    if (!block) {
        errno = ENOMEM;
        goto fail;
    }

    /* Carve the freshly allocated arena into its five data areas, in
     * exactly the layout documented in neuron.h. */
    char  *p     = (char *)block;
    float *volt  = (float *)p; p += total_neurons * elem;
    float *wts   = (float *)p; p += total_weights * elem;
    float *bs    = (float *)p; p += total_biases  * elem;
    float *inb   = (float *)p; p += (size_t)topo.input_size  * elem;
    float *outb  = (float *)p;

    /* ---- 6. Read the data arrays ---- */
    if (fread(volt, elem, total_neurons, fp) != total_neurons) goto fail_block;
    if (fread(wts,  elem, total_weights, fp) != total_weights) goto fail_block;
    if (fread(bs,   elem, total_biases,  fp) != total_biases ) goto fail_block;

    /* ---- 7. Reject trailing garbage after the declared data ---- */
    if (fgetc(fp) != EOF) {
        errno = EINVAL;
        goto fail_block;
    }

    fclose(fp);
    fp = NULL;

    if (net->initialized && net->memory_block) {
        aligned_free(net->memory_block);
    }

    net->topology       = topo;
    net->total_neurons  = total_neurons;
    net->total_weights  = total_weights;
    net->total_biases   = total_biases;

    memcpy(net->layer_offsets,  layer_offsets,  sizeof(layer_offsets));
    memcpy(net->weight_offsets, weight_offsets, sizeof(weight_offsets));
    memcpy(net->bias_offsets,   bias_offsets,   sizeof(bias_offsets));

    net->memory_block      = block;
    net->memory_block_size = mem_size;
    net->voltages          = volt;
    net->weights           = wts;
    net->biases            = bs;
    net->input_buffer      = inb;
    net->output_buffer     = outb;

    net->initialized        = true;
    net->needs_reset        = false;
    net->forward_pass_count = 0;
    net->neuron_activations = 0;

    return true;

fail_block:
    aligned_free(block);
fail:
    if (fp) fclose(fp);
    return false;
}
