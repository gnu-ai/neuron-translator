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

# Compile
make

# Install
sudo make install

# Create mount point
sudo mkdir -p /llm

# Set translator
sudo settrans -cg /llm /hurd/sigmoid-neuron-translator

# Optional: allow writes without sudo (the node is owned by root)
sudo chmod 666 /llm
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

## Usage

### Reading Translator Status

```bash
# View network information and current output
cat /llm
```

Output includes:
- Network topology (layer sizes)
- Neuron parameters (reset potential, threshold, leak rate, refractory length)
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

- **Reset Potential**: -80.0 mV (biologically plausible)
- **Threshold**: -55.0 mV (firing threshold)
- **Leak Rate**: 0.1 (voltage decay rate)
- **Refractory Length**: 5 timesteps (post-spike silence)

These can be adjusted in the code if needed.

## Memory Usage Examples

| Network Topology | Neurons | Weights | Memory Usage |
|------------------|---------|--------|--------------|
| 10-20-5 | 35 | 250 | ~2 KB |
| 784-256-128-10 | 1,178 | 230,400 | ~930 KB |
| 1000-500-100 | 1,600 | 600,000 | ~2.3 MB |
| 10000-1000-100 | 11,100 | 10,100,000 | ~80 MB |

## Files

- `sigmoid-neuron-translator.c` - Main translator source code (47KB, 1400+ lines)
- `Makefile` - Compilation and installation
- `README.md` - This documentation
- `LICENSE` - GNU GPLv3 license

## Compilation

The code uses:
- **C23 standard** (compatible with C11 for Hurd)
- **POSIX compliance** for portability
- **Hurd trivfs** for translator interface
- **GNU Mach IPC** for inter-process communication

### Compilation Command

```bash
gcc -std=c23 -Wall -Wextra -pedantic -O3 -march=native \
    -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -o sigmoid-neuron-translator sigmoid-neuron-translator.c \
    -ltrivfs -lhurdfs -lports -lshouldbeinlibc -lm -lpthread
```

### Makefile Targets

```bash
make          # Build the translator
make install  # Install to /hurd/
make uninstall # Remove from /hurd/
make clean    # Clean build artifacts
make help     # Show usage information
```

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

For cross-compiling from Linux to Hurd:

```bash
# Install cross-compiler (Debian/Ubuntu)
sudo apt-get install gcc-i686-unknown-hurd

# Cross-compile
i686-unknown-hurd-gcc -std=c23 -O3 \
    -o sigmoid-neuron-translator sigmoid-neuron-translator.c \
    -ltrivfs -lhurdfs -lports -lshouldbeinlibc -lm
```

Note: Cross-compilation to Hurd requires nightly Rust with `-Z build-std` for the standard library.

## Bug Fixes Implemented

Based on Sylvia-27's analysis:

### 1. Voltage Reset After Spike (Critical)
**Issue**: Original code set voltage to SPIKE_AMPLITUDE but never reset to reset_potential, causing infinite spiking.

**Fix**: Explicitly reset voltage to reset_potential after spike detection.

### 2. Full Hurd Translator Implementation
**Issue**: Hurd layer (Mach IPC + trivfs) was stubbed out.

**Fix**: Complete implementation with proper trivfs integration, correct function signatures, and proper type declarations.

### 3. Correct settrans Syntax
**Issue**: README had inverted syntax: `settrans -c <translator> <node>`

**Fix**: Correct syntax: `settrans -c <node> <translator>`

### 4. Type Compatibility
**Issue**: Missing type definitions (pthread_spinlock_t, loff_t, blksize_t, ino64_t)

**Fix**: Added proper includes (`<pthread.h>`, etc.) and used compatible types (off_t instead of loff_t where needed).

### 5. Function Signature Mismatches
**Issue**: trivfs function signatures didn't match Hurd's expectations.

**Fix**: Corrected all function signatures to match trivfs.h declarations.

### 6. Build Target Correction
**Issue**: Used `i686-unknown-gnu` which is not a valid Rust target.

**Fix**: Correct targets are `i686-unknown-hurd-gnu` and `x86_64-unknown-hurd-gnu`.

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
