<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Sigmoid Neuron Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# Sigmoid Neuron Translator for GNU Hurd

A **memory-efficient, CPU-optimized** sigmoid neuron translator for GNU Hurd, specifically designed to handle **large numbers of neurons** (100K+) with **minimal memory footprint** and **low CPU usage**. This implementation follows the **Claude Delannoy** style with extensive English comments.

## Features

### Memory Efficiency
- **float32 storage**: All numeric values use 4-byte float instead of 8-byte double
- **Contiguous allocation**: Single memory block for entire network (arena allocator)
- **Compact structures**: Minimum padding, optimal data packing
- **16-byte alignment**: SIMD-compatible memory layout
- **Scalable**: Linear memory growth with network size

### CPU Efficiency  
- **Cache-friendly**: Sequential memory access patterns
- **Minimal branching**: Predictable execution in hot loops
- **Inline functions**: No call overhead for critical functions
- **No runtime allocation**: All memory pre-allocated at initialization
- **Optimized forward pass**: Hand-tuned for performance

### Neuron Model
- **Sigmoid activation**: f(x) = 1 / (1 + exp(-x))
- **Feedforward network**: Input -> Hidden Layer(s) -> Output
- **Contiguous memory layout**: All data in single block
- **Low overhead**: Designed for thousands of neurons

## Specification Compliance

This implementation complies with:
- **C23 Standard**: Full C23 compliance
- **POSIX Standard**: POSIX.1-2008 compliant
- **GNU Hurd Documentation**: Follows Hurd translator guidelines
- **GNU AI Requirements**: https://gnu-ai.org/doku.php?id=start

## Installation on GNU/Hurd

### Prerequisites
- GNU/Hurd system (Debian GNU/Hurd recommended)
- GCC (tested with GCC 12+)
- Hurd development libraries
- Mach IPC headers

### Quick Setup

```bash
# Clone repository
git clone git@github.com:gnu-ai/neuron-translator.git
cd neuron-translator

# Build with the autotools chain shared by the whole GNU AI stack
./autogen.sh
./configure
make
make check          # CLI conformance test (--version / --help)

# Install
sudo make install

# Create mount point
sudo mkdir -p /llm

# Set translator
sudo settrans -cg /llm /hurd/sigmoid-neuron-translator

# Optional: allow writes without sudo (the node is owned by root)
sudo chmod 666 /llm
```

The translator also answers the GNU base commands like any other
GNU tool:

```bash
sigmoid-neuron-translator --version
sigmoid-neuron-translator --help
```

### Writing commands to the translator

The `/llm` node is owned by root, so plain redirection as a normal user
fails with `Permission denied` (and `sudo echo ... > /llm` does not help:
bash opens the redirection as your own user before sudo runs). Write
commands through a pipe to `tee`:

```bash
echo '3,5,2' | sudo tee /llm
```

Or run `sudo chmod 666 /llm` once, then plain redirection works:

```bash
echo '3,5,2' > /llm
```

## Scope: an inference engine, not a trainer

This translator is the **compute unit** of the GNU AI stack: it turns a
topology, a set of weights and an input vector into an output vector,
by plain POSIX reads and writes. It contains **no training step** —
nothing in this repository ever changes a weight after
initialization. What the weights *are* therefore matters as much as
what the code does with them:

- **A fresh mount carries untrained weights.** `network_init()` seeds
  every weight deterministically from its index
  (`((i * 2654435761) & 0x7FFFFF) / 0x7FFFFF * 0.4 - 0.2`, i.e. a
  fixed pseudo-random value in [-0.2, 0.2]). This is a **wiring
  placeholder**, reproducible across mounts, so that the stack can be
  exercised end to end (topology in, output out) — but the outputs are
  noise. N instances mounted by the orchestrator each vote on their
  own noise until real weights are loaded.
- **Meaningful weights come from a `.nn` file**, loaded with the
  `load <file>` command. A `.nn` file is produced either by the
  `save <file>` command of another instance, or by an **external
  trainer** that emits the documented format (see *The .nn model
  format* below). No trainer lives in this repository by design:
  training is a separate concern of the stack (the orchestrator
  archives training data through data-base-translator; the component
  that consumes it to produce `.nn` files is specified there, not
  here).

If the stack is mounted and driven without loading weights, the
compute claim is limited to "the plumbing works": every translator
keeps its structural guarantees (fault isolation, auditability,
composition, replayability of the *same* numbers), and the numbers
themselves remain pseudo-random until a trained file is loaded.

## Usage

### Reading Translator Status

```bash
# View network information and current output
cat /llm
```

Output includes:
- Network topology (layer sizes)
- Initial voltage parameter (reset potential)
- Memory usage
- Statistics (forward passes, neuron activations)
- Current output values

### Configuring the Network

```bash
# Set topology (comma or space separated)
echo '10,20,5' | sudo tee /llm

# Or with spaces
echo '10 20 5' | sudo tee /llm
```

This creates a network with:
- Input layer: 10 neurons
- Hidden layer: 20 neurons  
- Output layer: 5 neurons

### Providing Input

```bash
# Set input values (comma or space separated)
# The number of values must exactly match the input layer size
echo '0.5,0.3,0.8,0.1,0.9,0.2,0.4,0.6,0.0,0.7' | sudo tee /llm
```

The input must match the number of neurons in the input layer. When input is provided, the forward pass is automatically executed, and the output is available for reading.

### Saving and Loading Weights

```bash
# Write the current network (topology + weights) to a .nn file
echo 'save /tmp/model.nn' | sudo tee /llm

# Load a trained (or saved) network from a .nn file
echo 'load /tmp/model.nn' | sudo tee /llm

# Re-seed the scratch state (voltages, counters) without touching
# the weights
echo 'reset' | sudo tee /llm
```

`load` is the only way a weight ever changes after `network_init()`.
A failed or inconsistent load leaves the previously mounted network
untouched (the load is transactional: everything is validated into
local state before being committed).

### The .nn Model Format (v2)

The format is the contract between this translator and any external
trainer: anything that writes this layout produces a loadable
network. The file is the in-memory layout of the running program
written out field by field, so on a given platform it is fixed and
exact; the canonical platform is 64-bit little-endian Hurd (x86_64).

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x4E455552` (`'NEUR'`, little-endian) |
| 4 | 2 | format version, `2` (uint16; version 1 files are rejected) |
| 6 | 1 | `layer_count` (2..8) |
| 7 | 1 | (alignment padding) |
| 8 | 16 | `layer_sizes[8]` (uint16 each, first `layer_count` used, none 0 or > 8192) |
| 24 | 2 | `input_size` (must equal `layer_sizes[0]`) |
| 26 | 2 | `output_size` (must equal `layer_sizes[layer_count-1]`) |
| 28 | 2 | (alignment padding) |
| 30 | 4 | `reset_potential` (float32; initial scratch voltage) |
| 34 | 4 | (struct padding, 3 bytes + alignment) |
| 38 | 8 | `total_neurons` (size_t; must match the recomputed value) |
| 46 | 8 | `total_weights` (size_t; recomputed and checked) |
| 54 | 8 | `total_biases` (size_t; recomputed and checked) |
| 62 | 8*layer_count | `layer_offsets` (size_t each) |
| .. | 8*(layer_count-1) | `weight_offsets` (size_t each) |
| .. | 8*layer_count | `bias_offsets` (size_t each) |
| .. | 4*total_neurons | `voltages` (float32 each; scratch state) |
| .. | 4*total_weights | `weights` (float32 each) |
| .. | 4*total_biases | `biases` (float32 each) |

(The `NetworkTopology` struct occupies bytes 6..37 — its measured
layout on 64-bit little-endian is `sizeof == 32` with `layer_count`
at 0, `layer_sizes` at 2, `input_size` at 18, `output_size` at 20,
`reset_potential` at 24.)

Key rules, enforced by `network_load()`:

- the totals, offsets and sizes in the file are **never trusted**:
  they are recomputed from `layer_sizes` and must match exactly;
- upper limits bound the damage a hostile file can request
  (16M neurons, 256M weights);
- the weight matrix of layer `i` (1-based, hidden/output layers) is
  a row-major `layer_sizes[i] * layer_sizes[i-1]` block, immediately
  followed by the next layer's block; the bias vector of layer `i`
  has `layer_sizes[i]` entries.

### Example Session

```bash
# Configure network
echo '3,5,2' | sudo tee /llm

# View configuration
cat /llm
# Output shows: 3 layers, sizes 3-5-2, etc.

# Set input (3 values for a 3-neuron input layer)
echo '0.5,0.3,0.8' | sudo tee /llm

# View output (automatically updated)
cat /llm
# Output shows new output values

# Change topology
echo '5,10,3' | sudo tee /llm

# Set new input (now 5 values)
echo '0.1,0.2,0.3,0.4,0.5' | sudo tee /llm

# View results
cat /llm
```

## Implementation Details

### Memory Layout

All network data is stored in a single contiguous memory block with the following layout:

```
[voltages][weights][biases][input_buffer][output_buffer]
```

This provides:
- **Single allocation**: Only one malloc call
- **Better cache locality**: All data is close together
- **Easier management**: Simpler to allocate and free
- **Memory-mapped file support**: Could be extended to use mmap()

### Forward Pass Algorithm

The forward pass uses the following optimized algorithm:

```c
for each layer (starting from first hidden layer):
    for each neuron in layer:
        sum = bias + Σ(input_neuron × weight)
        output = sigmoid(sum)
```

Key optimizations:
- Pre-calculated memory offsets
- Sequential memory access
- Inline sigmoid function
- No dynamic allocation

### Neuron Parameters

The model has a single parameter:

- **Reset Potential**: -80.0 mV — the value that fills the scratch
  voltage array at init and reset time.

Earlier revisions documented a spiking model (threshold -55 mV, leak
0.1, refractory 5).  Those parameters were never read by the forward
pass, which is and always was a plain sigmoid feedforward; they were
removed in `.nn` format version 2 (see *The .nn Model Format*).

## Memory Usage Examples

| Network Topology | Neurons | Weights | Memory Usage |
|------------------|---------|--------|--------------|
| 10-20-5 | 35 | 250 | ~2 KB |
| 784-256-128-10 | 1,178 | 230,400 | ~930 KB |
| 1000-500-100 | 1,600 | 600,000 | ~2.3 MB |
| 10000-1000-100 | 11,100 | 10,100,000 | ~80 MB |

## Files

- `src/main-hurd.c` — entry point on GNU/Hurd (trivfs startup, argp)
- `src/main.c` — verification entry point for non-Hurd POSIX systems
- `src/neuron.c` — the compute core: init, forward pass, save/load
- `src/trivfs-hooks.c` — the trivfs server (reads, writes, status)
- `include/neuron.h` — the data structures and the `.nn` format constants
- `configure.ac`, `Makefile.am`, `src/Makefile.am`, `tests/Makefile.am` — the autotools build
- `README.md` — this documentation
- `LICENSE` — GNU GPLv3 license

## Compilation

The code uses:
- **C23 standard** (detected by configure: `-std=c23` or `-std=c2x`)
- **POSIX compliance** for portability
- **Hurd trivfs** for the translator interface
- **GNU Mach IPC** for inter-process communication

### Build Commands

```bash
./autogen.sh   # autoreconf, run once after cloning
./configure    # picks the right main for the host (Hurd or not)
make           # builds sigmoid-neuron-translator
make check     # runs the CLI conformance test (--version/--help)
make install   # installs to /hurd/
```

On GNU/Hurd the binary is the real translator (main-hurd.c linked
against `-ltrivfs -lfshelp -lports -lshouldbeinlibc`); on any other
POSIX system the same name builds the verification binary driven by
main.c, so the code compiles and the tests run everywhere.

## Development

### Running GNU/Hurd VM

For testing and development:

```bash
# Download pre-built Hurd image (32-bit)
wget https://cdimage.debian.org/cdimage/ports/stable/hurd-i386/debian-hurd.img.tar.gz
tar xzf debian-hurd.img.tar.gz

# Start VM (requires KVM)
kvm -m 2G -drive file=$(echo debian-hurd*.img),cache=writeback

# 64-bit pre-release (experimental)
wget https://cdimage.debian.org/cdimage/ports/latest/hurd-amd64/debian-hurd.img.tar.gz
tar xzf debian-hurd.img.tar.gz
kvm -m 2G -drive file=$(echo debian-hurd*.img),cache=writeback
```

Inside Hurd VM:
```bash
# Set up network (required for apt)
echo "nameserver 8.8.8.8" > /etc/resolv.conf

# Install build tools
apt update
apt install build-essential gcc hurd-dev git make

# Clone and build
cd /tmp
git clone https://github.com/gnu-ai/neuron-translator.git
cd neuron-translator
make
sudo make install
```

### Cross-Compilation (Advanced)

The build system is autotools, so cross-building from Linux is a
cross toolchain plus the usual triple:

```bash
./autogen.sh
./configure --host=x86_64-gnu   # with the matching cross toolchain
make
```

Cross-compiling to GNU/Hurd also requires MIG and the Hurd/glibc
headers for the target; building natively on a Hurd system (as in
the VM instructions above) is the supported path.

## Code Style

This implementation follows the **Claude Delannoy** style:
- **Extensive comments**: Every function and major code block is documented
- **Educational**: Explains the "why" not just the "what"
- **Clear structure**: Well-organized with logical sections
- **Consistent formatting**: Proper indentation and spacing
- **Descriptive names**: Self-documenting variable and function names

## Compliance with GNU Hurd Documentation

This translator implements the following from the [GNU Hurd documentation](https://www.gnu.org/software/hurd/hurd/documentation.html):

1. **Trivial Filesystem (trivfs)**: Uses the standard trivfs interface
2. **Mach IPC**: Proper message passing with Mach ports
3. **Translator Protocol**: Follows the Hurd translator conventions
4. **Error Handling**: Returns proper error_t codes
5. **Port Management**: Correctly manages Mach ports

## Compliance with GNU AI Requirements

This implementation satisfies the requirements from [https://gnu-ai.org/doku.php?id=start](https://gnu-ai.org/doku.php?id=start):

- **Memory efficiency**: Single contiguous allocation, float32 storage
- **CPU efficiency**: Optimized forward pass, cache-friendly access
- **Scalability**: Supports thousands of neurons
- **Multi-task and multi-user operation**: the translator must serve several
  users **simultaneously** — concurrent `write` commands and `read` requests
  on `/llm` are serialized by a lock, each `read` returns one consistent
  snapshot (topology and outputs never mixed mid-pass), and no unprotected
  global state may serialize users with each other
- **POSIX compliance**: Follows POSIX standards
- **C standard compliance**: Uses C23 standard
- **Extensive documentation**: Claude Delannoy style comments
- **Hurd integration**: Full trivfs translator implementation

## Cluster Operation

As of orchestrator phase 3, GNU AI runs on multi-node Hurd clusters:
instances of this translator are launched on remote nodes and driven
by the orchestrator over SSH. This translator is deliberately
unchanged by that — each node runs its own instance, driven by local
POSIX `write`/`read`, and the contract (write topology, write input,
read status/output) is identical whether the instance is local or
remote. No network capability is added to this translator:
distribution is the orchestrator's responsibility.

## Troubleshooting

### Compilation Errors

If you encounter compilation errors:

1. **Missing headers**: Ensure you have Hurd development packages installed:
   ```bash
   apt install hurd-dev libhurdfs-dev
   ```

2. **Type errors**: Check that all types match the Hurd headers. The code uses compatible POSIX types.

3. **Linker errors**: Ensure all required libraries are linked. The Makefile includes all necessary libraries.

### Runtime Issues

1. **settrans fails**: Make sure the translator binary is in `/hurd/` or specify the full path.

2. **Permission denied on writes**: The node enforces the underlying file's permissions. Write via `echo '...' | sudo tee /llm`, or run `sudo chmod 666 /llm` once. Note that `sudo echo ... > /llm` does not work: bash performs the redirection as your own user before sudo runs.

3. **Translator not responding**: Check that the translator is running with `ps aux | grep sigmoid-neuron-translator`.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Commit your changes
4. Push to the branch
5. Open a pull request

## License

GNU General Public License version 3 or later. See [LICENSE](LICENSE) for details.

```
Copyright (C) 2026 GNU AI Project
Author: Claire <claire@gnu-ai.org>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
```
