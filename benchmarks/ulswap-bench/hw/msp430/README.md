# MSP430 Build Setup

This guide explains how to set up the MSP430 cross-compilation environment for UlSWaP-Bench.

## Prerequisites

- CMake 3.16+
- Meson build system
- Ninja build system
- Python 3.x

Install on Ubuntu/Debian:
```bash
sudo apt install cmake meson ninja-build python3
```

Install on macOS:
```bash
brew install cmake meson ninja python3
```

## Required Environment Variables

| Variable | Description | Example |
|----------|-------------|---------|
| `MSP430_GCC_ROOT` | Path to MSP430 GCC toolchain | `$HOME/ti/msp430-gcc` |

Set these in your shell profile or use [direnv](https://direnv.net/) with the provided `.envrc.example`.

## Step 1: Install MSP430 GCC Toolchain

Download the MSP430 GCC toolchain from Texas Instruments:
- https://www.ti.com/tool/MSP430-GCC-OPENSOURCE

Extract to your preferred location (e.g., `$HOME/ti/msp430-gcc`):
```bash
mkdir -p ~/ti
tar -xf msp430-gcc-*.tar.bz2 -C ~/ti
mv ~/ti/msp430-gcc-*_linux64 ~/ti/msp430-gcc
```

## Step 2: Build and Install picolibc 1.7.9

> **Why version 1.7.9?** Newer picolibc versions use linker script features (like `ALIGN_WITH_INPUT`) that are not supported by the linker (ld 2.34) bundled with the MSP430 GCC toolchain. Version 1.7.9 is the last version compatible with ld 2.34.

### Download picolibc 1.7.9

```bash
cd ~/ti
wget https://keithp.com/picolibc/dist/picolibc-1.7.9.tar.xz
tar -xf picolibc-1.7.9.tar.xz
cd picolibc-1.7.9
```

### Create Cross-Compilation Configuration

Create a file named `cross-msp430.txt`:
```ini
[binaries]
c = 'msp430-elf-gcc'
ar = 'msp430-elf-ar'
as = 'msp430-elf-as'
strip = 'msp430-elf-strip'

[host_machine]
system = 'none'
cpu_family = 'msp430'
cpu = 'msp430'
endian = 'little'

[properties]
skip_sanity_check = true
```

### Configure and Build

```bash
# Ensure MSP430 GCC is in PATH
export PATH="$HOME/ti/msp430-gcc/bin:$PATH"

# Configure with meson
meson setup build-msp430 \
    --cross-file cross-msp430.txt \
    -Dmultilib=false \
    -Dpicocrt=false \
    -Dpicolib=false \
    -Dsemihost=false \
    -Dspecsdir=none \
    -Dtests=false \
    -Dtinystdio=true \
    -Dio-long-long=true \
    -Dformat-default=integer \
    -Dnewlib-nano-malloc=true \
    -Dlite-exit=true \
    -Dprefix="$HOME/ti/msp430-gcc"

# Build
ninja -C build-msp430

# Install (installs into MSP430 GCC directory)
ninja -C build-msp430 install
```

After installation, verify that `picolibc.specs` exists:
```bash
ls $HOME/ti/msp430-gcc/lib/gcc/msp430-elf/*/picolibc.specs
```

## Step 3: Set Environment Variables

### Option A: Shell Profile

Add to your `~/.bashrc` or `~/.zshrc`:
```bash
export MSP430_GCC_ROOT="$HOME/ti/msp430-gcc"
export PATH="$MSP430_GCC_ROOT/bin:$PATH"
```

### Option B: Using direnv

Copy the example file and customize:
```bash
cp .envrc.example .envrc
# Edit .envrc to match your paths
direnv allow
```

## Step 4: Build UlSWaP-Bench

```bash
# From the repository root
cmake -B build-msp430 \
    -DARCH=msp430 \
    -DCMAKE_TOOLCHAIN_FILE=hw/msp430/toolchain.cmake

# Build a specific benchmark
cmake --build build-msp430 --target activity_rec

# Check the binary size
msp430-elf-size build-msp430/bin/activity_rec.elf
```

## Troubleshooting

### "MSP430_GCC_ROOT environment variable is not set"

Set the environment variable:
```bash
export MSP430_GCC_ROOT="$HOME/ti/msp430-gcc"
```

### "picolibc.specs not found"

Ensure picolibc is installed into the MSP430 GCC directory. Re-run the picolibc build with the correct `--prefix`.

### "ALIGN_WITH_INPUT not supported" or similar linker errors

You're using a picolibc version newer than 1.7.9. Downgrade to version 1.7.9 as described above.

### Build errors with missing headers

Verify the include path:
```bash
ls $MSP430_GCC_ROOT/include/msp430fr5994.h
```
