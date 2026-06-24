# YACRig

YACRig is a CPU and NVIDIA GPU miner for [Yacoin](https://github.com/yacoin/yacoin) (YAC), implementing the scrypt-chacha proof-of-work and `getwork` JSON-RPC against a local `yacoind`.

YACRig is a fork of [XMRig](https://github.com/xmrig/xmrig) v6.25.0. The mining engine, autotuner, OpenCL/CUDA backends, configuration system, and CLI conventions all come from XMRig. The scrypt-chacha kernel (vendored from [`scrypt-jane`](https://github.com/floodyberry/scrypt-jane)), the `YacGetworkClient` that speaks to yacoind, the YAC `Coin` / `Algorithm` registrations, and the YAC-aware autotuner are the additions made here.

## Table of contents

- [1. Status](#1-status)
- [2. Prerequisites](#2-prerequisites)
- [3. Build](#3-build)
- [4. Quick start](#4-quick-start)
  - [4.1 Basic commands](#41-basic-commands)
  - [4.2 Basic commandline options](#42-basic-commandline-options)
  - [4.3 Concrete example](#43-concrete-example)
  - [4.4 Mining on an NVIDIA GPU](#44-mining-on-an-nvidia-gpu)
- [5. What YACRig changes from XMRig](#5-what-yacrig-changes-from-xmrig)
- [6. License](#6-license)
- [7. Acknowledgements](#7-acknowledgements)

## 1. Status

Alpha. CPU mining and NVIDIA GPU mining (via the [yacrig-cuda](https://github.com/chautnguyen4392/yacrig-cuda) plugin) both work end-to-end against `yacoind`. AMD GPU support and Stratum pool support are not yet implemented for YAC.

## 2. Prerequisites

YACRig is tested on Ubuntu 20.04 / 22.04. Install the build dependencies with:

```bash
sudo apt-get install -y \
    build-essential cmake git automake libtool autoconf pkg-config \
    libuv1-dev libssl-dev libhwloc-dev
```

A C++17-capable compiler is required (gcc 9+ or clang 10+). Other Linux distributions work with equivalent packages.

## 3. Build

The default build covers CPU mining, NVIDIA GPU mining, or both. From the repository root:

```bash
cmake -S . -B build
cd build && make -j$(nproc)
```

The scrypt-chacha algorithm, the CUDA backend, and HTTP support are all enabled by default, so no extra flags are needed.

The CUDA backend compiles only a runtime loader and does not pull in the CUDA Toolkit, so the same binary supports NVIDIA GPU mining. To actually mine on an NVIDIA GPU you also need the separate [yacrig-cuda](https://github.com/chautnguyen4392/yacrig-cuda) plugin (`libyacrig-cuda.so`), which holds the CUDA kernels. See the [yacrig-cuda README](https://github.com/chautnguyen4392/yacrig-cuda) for its driver/toolkit requirements and build steps, and [`doc/YAC_CUDA_MINING.md`](doc/YAC_CUDA_MINING.md) for the full GPU walkthrough (building, tuning, measured hashrate). The individual CMake flags are documented in the build sections of [`doc/YAC_CPU_MINING.md`](doc/YAC_CPU_MINING.md) and [`doc/YAC_CUDA_MINING.md`](doc/YAC_CUDA_MINING.md).

A successful build leaves a `yacrig` binary inside `build/`. Verify it with the built-in self-test:

```bash
./yacrig --scrypt-chacha-test
# vector 1: OK
# vector 2: OK
# scrypt-chacha-test: all 2 vector(s) passed
```

This runs the same scrypt-chacha kernel YACRig uses at mining time against two known block headers and exits 0 on success. Useful as a sanity check on any new machine.

## 4. Quick start

### 4.1 Basic commands

With a `yacoind` instance already running (`server=1`, `rpcuser` / `rpcpassword` set, and at least one P2P peer), point YACRig at it. The four invocations below build up from minimal to fully-configured. Pick the one that fits your needs:

```bash
# 1. The basic command to run
./yacrig --coin=yac --daemon -o <host>:<rpcport> -u <rpcuser> -p <rpcpassword>

# 2. Specify the interval for polling yacoind for new mining jobs
./yacrig --coin=yac --daemon -o <host>:<rpcport> -u <rpcuser> -p <rpcpassword> \
         --daemon-poll-interval=<interval>

# 3. Display more helpful logs
./yacrig --coin=yac --daemon -o <host>:<rpcport> -u <rpcuser> -p <rpcpassword> \
         --daemon-poll-interval=<interval> --print-time=<seconds> --verbose

# 4. Specify the number of CPU threads
./yacrig --coin=yac --daemon -o <host>:<rpcport> -u <rpcuser> -p <rpcpassword> \
         --threads=<number_threads> --daemon-poll-interval=<interval> \
         --print-time=<seconds> --verbose
```

**Commands 1-3 do not set `--threads` and therefore let YACRig's autotuner pick the worker count automatically**, based on the number of physical CPU cores available and the amount of free RAM on the machine (each scrypt-chacha worker reserves a 512 MiB scratchpad). Only command 4 overrides that with an explicit thread count.

### 4.2 Basic commandline options

| Option | Default | Description |
|--------|---------|-------------|
| `--coin=yac` | - | Selects the YAC preset (algorithm = scrypt-chacha). **Required** for YAC mining. |
| `--daemon` | off | Talks to a daemon over HTTP JSON-RPC instead of a Stratum pool. **Required** for solo YAC mining. |
| `-o <host>:<rpcport>` | - | yacoind's RPC endpoint. The default RPC port is `7687`. **Required.** |
| `-u <rpcuser>` | - | Matches yacoind's `rpcuser`. **Required.** |
| `-p <rpcpassword>` | - | Matches yacoind's `rpcpassword`. **Required.** |
| `--daemon-poll-interval=<ms>` | `10000` (ms) | How often YACRig polls yacoind for a new chain tip. The default of 10 s keeps log noise down with no measurable hashrate impact. Lower it (e.g. `1000`) if you need faster chain-tip detection. |
| `--print-time=<seconds>` | `60` (s) | How often the rolling-average hashrate line is printed. |
| `--verbose` | off | Print one line per RPC call and one line per share found. Recommended while you're learning the tool or diagnosing problems. |
| `--threads=<number_threads>` | autotuned | Force exactly this many CPU worker threads. When omitted, the autotuner picks `min(cpu_budget, mem_budget)`. |
| `--reserve-ram=<MiB>` | `2048` (MiB) | RAM in megabytes the autotuner leaves untouched for the OS and other processes. Lower this to fit more workers on a memory-tight machine. Raise it when YACRig competes with other workloads. **Has no effect when `--threads` is also set**. Setting `--threads` bypasses the autotuner entirely, and `--reserve-ram` is silently ignored. |

### 4.3 Concrete example

Local `yacoind` on the default RPC port `7687`, polling every 30 s, printing the hashrate every 30 s, and running 4 worker threads:

```bash
./yacrig --coin=yac --daemon \
         -o 127.0.0.1:7687 \
         -u yacuser -p yacpass \
         --threads=4 \
         --daemon-poll-interval=30000 \
         --print-time=30 \
         --verbose
```

`--reserve-ram` is intentionally omitted: when `--threads` is set the autotuner doesn't run, so `--reserve-ram` would be silently ignored. If you'd rather let the autotuner pick the thread count and tune the OS reserve, drop `--threads=4` and add `--reserve-ram=<MiB>` instead.

The full setup walkthrough, including yacoind configuration, autotuner behaviour, log interpretation, and troubleshooting, is documented in [`doc/YAC_CPU_MINING.md`](doc/YAC_CPU_MINING.md).

### 4.4 Mining on an NVIDIA GPU

With the CUDA backend built (see [3. Build](#3-build) above) and the `libyacrig-cuda.so` plugin next to the `yacrig` binary, add `--cuda` to any of the commands above:

```bash
# GPU + CPU together (the autotuner sizes both backends)
./yacrig --coin=yac --daemon -o <host>:<rpcport> -u <rpcuser> -p <rpcpassword> --cuda

# GPU only, with a tuned scratchpad/compute trade-off
./yacrig --coin=yac --daemon -o <host>:<rpcport> -u <rpcuser> -p <rpcpassword> \
         --cuda --no-cpu --cuda-lookup-gap=32 --verbose
```

The plugin autotunes each card to fill its VRAM, so `--cuda` alone is enough to start. The most common GPU options:

| Option | Default | Description |
|--------|---------|-------------|
| `--cuda` | off | Enable the CUDA backend. **Required** to mine on the NVIDIA GPU. |
| `--cuda-loader=PATH` | next to binary | Path to `libyacrig-cuda.so`. Omit when it sits beside the `yacrig` binary. |
| `--no-cpu` | off | Mine on the GPU only. Drop it to run GPU and CPU together. |
| `--cuda-lookup-gap=N` | `64` | Scratchpad/compute trade-off. Lower it (e.g. `32`) so each launch finishes faster between blocks. |
| `--cuda-use-system-ram` | off | Spill scratchpads into host RAM past the VRAM ceiling (helps Turing/Ampere, not Pascal). |

The full NVIDIA GPU walkthrough, including the driver/toolkit setup, the per-device tuning knobs, and measured hashrate on tested cards, is in [`doc/YAC_CUDA_MINING.md`](doc/YAC_CUDA_MINING.md).

## 5. What YACRig changes from XMRig

User-visible:

- New `--coin=yac` selector and `--scrypt-chacha-test` commandline option.
- Binary is named `yacrig`, banner says `YACRig 0.1.0`, default data directory is `~/.yacrig/`.
- Donation strategy is forced off whenever a YAC pool is configured (the upstream dev pool can't accept YAC `getwork` shares).
- URL references to `xmrig.com` in user-visible text have been stripped.

Internal (kept under upstream naming for merge compatibility with XMRig):

- The CMake target is still named `xmrig`, the `xmrig::` C++ namespace is preserved, the `XMRIG_*` preprocessor macros are preserved, and source files under `src/` keep their existing paths. Only `OUTPUT_NAME` is overridden to emit a `yacrig` binary.
- All upstream copyright notices and the GPLv3 license terms are preserved unchanged.

## 6. License

YACRig is licensed under [GPL-3.0-or-later](LICENSE), the same as upstream XMRig. Per-file copyright notices for upstream XMRig contributors are preserved.

## 7. Acknowledgements

YACRig stands on the shoulders of:

- [XMRig](https://github.com/xmrig/xmrig): the CPU/GPU mining engine and most of the surrounding infrastructure.
- [scrypt-jane](https://github.com/floodyberry/scrypt-jane): the scrypt-chacha kernel.
- [Yacoin](https://github.com/yacoin/yacoin): the coin itself, its scrypt-chacha parameters, and the `getwork` RPC YACRig consumes.

Thanks to the authors of all three.

The original XMRig README is preserved as [`UPSTREAM_README.md`](UPSTREAM_README.md) for reference.
