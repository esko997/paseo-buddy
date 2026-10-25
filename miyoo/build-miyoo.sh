#!/usr/bin/env bash
# Cross-compiles the Onion app folder (build/PaseoPager) inside a cached Debian buster (gcc 8.3, glibc 2.28) image matching the device.
set -euo pipefail
cd "$(dirname "$0")"
engine=$(command -v podman || command -v docker)
"$engine" build -q -t paseo-pager-toolchain -f Containerfile . >/dev/null
"$engine" run --rm -v "$PWD":/src:Z -w /src paseo-pager-toolchain make dist "$@"
echo "Built $(pwd)/build/PaseoPager -> copy it to /mnt/SDCARD/App/ on the Miyoo's SD card"
