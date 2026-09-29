#!/bin/sh
# Build with the legacy-libnds devkitARM image (the ARM7 side needs its own main).
# The repo root is mounted: ranet uses include/ra_netprofile.h
set -e
cd "$(dirname "$0")/.."
docker run --rm -v "$(pwd)":/src -w /src/ranet devkitpro/devkitarm:20241104 make "$@"
