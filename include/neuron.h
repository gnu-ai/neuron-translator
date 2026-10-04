/*
 * neuron.h - Neural Network Definitions for LLM Translator
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

/** @file neuron.h
 *  @brief Neural network data structures and function declarations
 *
 *  This header defines the compact neural network structure used by the
 *  sigmoid neuron translator for GNU Hurd. The design follows Claude
 *  Delannoy's style with clear, educational, and maintainable code.
 */

#ifndef NEURON_H
#define NEURON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>       /* For expf() */


/*****************************************************************************
 *                                                                           *
 *                         CONSTANT DEFINITIONS                             *
 *                                                                           *
 *  Neural network parameters and limits for memory-efficient implementation  *
 *                                                                           *
 *****************************************************************************/
/* --- Model file format --- */
/* Every .nn file starts with a magic number and a format version, so
 * network_load() can reject files it does not know how to read.
 *
 * Version 2 removed the unused spiking-model fields (threshold, leak
 * rate, refractory length) from NetworkTopology: the model is a pure
 * sigmoid feedforward pass and these parameters were never read by
 * network_forward().  Files written by version 1 are rejected. */
#define NET_FILE_MAGIC     0x4E455552u   /* 'N' 'E' 'U' 'R' */
#define NET_FILE_VERSION   2u

/* --- Safe upper limits (protection against fuzzed/malicious files) ---
 * A model file is untrusted input: these ceilings bound how much memory
 * a single load is allowed to request, whatever the file claims. */
#define NET_MAX_TOTAL_NEURONS  (16u  * 1024u * 1024u)   /* 16M  */
#define NET_MAX_TOTAL_WEIGHTS  (256u * 1024u * 1024u)   /* 256M */
#define NET_MAX_TOTAL_BIASES   NET_MAX_TOTAL_NEURONS

/** Maximum number of layers in the neural network */
#define MAX_LAYERS 8

/** Maximum number of neurons per layer */
#define MAX_NEURONS_PER_LAYER 8192

/** SIMD alignment for memory allocation */
#define SIMD_ALIGNMENT 16

/** Initial neuron voltage (millivolts).
 * The voltages array is scratch state between passes; this constant
 * fills it at init and reset time.  It is NOT a spiking mechanism:
 * the model is a pure sigmoid feedforward pass (see network_forward
 * in src/neuron.c), so no threshold/leak/refractory parameters
 * exist. */
#define RESET_POTENTIAL (-80.0f)

/** Default network topology: input=10, hidden=20, output=5 */
#define DEFAULT_LAYER_SIZES {10, 20, 5}
#define DEFAULT_LAYER_COUNT 3


/*****************************************************************************
 *                                                                           *
 *                          DATA STRUCTURES                                 *
 *                                                                           *
 *  Memory layout: [voltages][weights][biases][input_buffer][output_buffer] *
 *                                                                           *
 *****************************************************************************/

/** Network topology configuration.
 * This structure is serialized as-is into .nn files (see
 * network_save/network_load), so its layout is part of the file
 * format: NET_FILE_VERSION is bumped whenever it changes. */
typedef struct NetworkTopology {
    uint8_t layer_count;            /**< Number of layers (2-8) */
    uint16_t layer_sizes[MAX_LAYERS]; /**< Neurons per layer */
    uint16_t input_size;             /**< Input layer size */
    uint16_t output_size;            /**< Output layer size */
    float reset_potential;          /**< Initial voltage of the scratch
                                         array (mV); see RESET_POTENTIAL */
    uint8_t padding[3];              /**< Keep the struct 4-byte aligned
                                         and the layout explicit */
} NetworkTopology;

/** Compact neural network with contiguous memory allocation */
typedef struct CompactNeuralNetwork {
    NetworkTopology topology;       /**< Network architecture */
    size_t total_neurons;           /**< Total neurons across all layers */
    size_t total_weights;           /**< Total weights */
    size_t total_biases;            /**< Total biases */
    size_t layer_offsets[MAX_LAYERS]; /**< Starting index of each layer */
    size_t weight_offsets[MAX_LAYERS - 1]; /**< Weight matrix offsets */
    size_t bias_offsets[MAX_LAYERS]; /**< Bias vector offsets */
    
    /* Contiguous memory block pointers */
    float *voltages;                 /**< Neuron membrane potentials */
    float *weights;                 /**< Synaptic weights */
    float *biases;                  /**< Neuron biases */
    float *input_buffer;            /**< Input data buffer */
    float *output_buffer;           /**< Output data buffer */
    
    void *memory_block;             /**< Single allocated memory block */
    size_t memory_block_size;        /**< Total memory allocated */
    
    /* Runtime state */
    bool initialized;               /**< Network initialization flag */
    bool needs_reset;               /**< Reset required flag */
    size_t forward_pass_count;      /**< Number of forward passes */
    size_t neuron_activations;       /**< Total neuron activations */
} CompactNeuralNetwork;


/*****************************************************************************
 *                                                                           *
 *                      FUNCTION DECLARATIONS                              *
 *                                                                           *
 *****************************************************************************/

/**
 * @brief Initialize the neural network with specified topology
 * 
 * @param net         Pointer to network structure
 * @param layer_count Number of layers (2-8)
 * @param layer_sizes Array of neuron counts per layer
 * @return            0 on success, -1 on error (errno set)
 */
int network_init(CompactNeuralNetwork *net,
                uint8_t layer_count,
                const uint16_t *layer_sizes);

/**
 * @brief Free all network resources
 * 
 * @param net Pointer to network structure
 */
void network_free(CompactNeuralNetwork *net);

/**
 * @brief Reset network state (voltages to reset potential)
 * 
 * @param net Pointer to network structure
 */
void network_reset(CompactNeuralNetwork *net);

/**
 * @brief Perform forward pass through the network
 * 
 * @param net Pointer to initialized network
 */
void network_forward(CompactNeuralNetwork *net);

/**
 * @brief Sigmoid activation function (inline for performance)
 * 
 * @param x Input value
 * @return   sigmoid(x) = 1 / (1 + exp(-x))
 */
static inline float sigmoidf(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

/**
 * @brief Parse configuration string (e.g., "10,20,5") into layer sizes
 * 
 * @param config_str Configuration string
 * @param layer_sizes Output array for layer sizes
 * @param max_layers  Maximum number of layers to parse
 * @return            Number of layers parsed, or -1 on error
 */
int parse_config_string(const char *config_str,
                       uint16_t *layer_sizes,
                       int max_layers);

/**
 * @brief Parse input string and run forward pass
 * 
 * @param net       Pointer to network
 * @param input_str Comma/space-separated input values
 * @return          true if input was valid and forward pass executed
 */
bool parse_input_string(CompactNeuralNetwork *net,
                       const char *input_str);

/**
 * @brief Save network state to file
 * 
 * @param net      Pointer to network
 * @param filename Path to save file
 * @return          true on success, false on failure
 */
bool network_save(const CompactNeuralNetwork *net,
                 const char *filename);

/**
 * @brief Load network state from file
 * 
 * @param net      Pointer to network
 * @param filename Path to load file
 * @return          true on success, false on failure
 */
bool network_load(CompactNeuralNetwork *net,
                 const char *filename);


#endif /* NEURON_H */
