# Lichii DLSS module

A small shared library that lets [Lichii](https://github.com/tasarren/Lichii)
use NVIDIA DLSS Super Resolution on Linux through the NGX Vulkan API. Lichii
loads it at runtime from its user data directory (`ngx/`); it is never bundled
with Lichii itself.

Lichii's settings dialog can download the prebuilt module and NVIDIA's DLSS
runtime files for you. This repository is for building that module by hand.

## Licences

- The module's own source code is MIT licensed; see `LICENSE`.
- The release also contains object code from NVIDIA's NGX SDK
  (`libnvsdk_ngx.a`, linked into `lichii-dlss-module.so`) and NVIDIA's DLSS
  runtime files. NVIDIA's licence terms cover that object code; they ship
  alongside the release as `NVIDIA-LICENSE.txt`.
- The NGX SDK itself is proprietary and is never distributed with this
  repository. The build fetches it directly from NVIDIA's public DLSS-tos
  repository and verifies every file against pinned SHA-256 sums.

This project is not affiliated with, endorsed by, or sponsored by NVIDIA
Corporation. NVIDIA and DLSS are trademarks of NVIDIA Corporation.

## Building with Docker

```sh
docker compose -f externals/lichii-dlss/compose.yaml run --rm build
```

The result lands in `externals/lichii-dlss/out/`:

- `lichii-dlss-module.so` — the module
- `LICENSE` — this project's MIT licence
- `NVIDIA-LICENSE.txt` — NVIDIA's licence terms for the SDK object code

Copy all three files into Lichii's `ngx` directory (the dialog's folder
button opens it), or point `EDEN_NX2_NGX_DIR` at a directory holding them.

## Building by hand

Requires a C++17 compiler, CMake, and the Vulkan headers.

1. Fetch the NGX SDK (headers and `lib/Linux_x86_64/libnvsdk_ngx.a`) from
   NVIDIA's DLSS-tos repository, DLSS v310.9.16, and accept NVIDIA's licence
   terms for the SDK.
2. Build:

```sh
cmake -B build -DNGX_SDK_DIR=/path/to/ngx-sdk
cmake --build build
```

## Requirements

- An NVIDIA GPU and the NVIDIA display driver with NGX support
  (`libnvidia-ngx.so.1`), 535.29.05 or newer.
- Vulkan.
- glibc 2.35 or newer (Ubuntu 22.04 era). The Docker build pins that base on
  purpose: a module built on a newer distro silently fails to load on older
  ones.
