#!/bin/sh
# From the repo root: MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash -lc 'cd /mnt/e/projects/ra-nds && docker run --rm -v /mnt/e/projects/ra-nds:/src -w /src debian:bookworm-slim sh tests/run_host_tls.sh'
set -e
apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev ca-certificates >/dev/null 2>&1
M=external/mbedtls
gcc -O1 -Iinclude -I$M/include '-DMBEDTLS_CONFIG_FILE="mbedtls_config_host.h"' -Itests \
	tests/host_tls.c source/certs.c $M/library/*.c -o /tmp/host_tls
/tmp/host_tls
