#!/bin/sh
# Host run of step 2 against the real RA server (read-only calls).  Needs
# RA_USER and RA_TOKEN in the environment and the Pi's set files as
# arguments, e.g. from the repo root in WSL:
#   docker run --rm -e RA_USER -e RA_TOKEN -v $PWD:/src -w /src debian:bookworm-slim \
#       sh tests/run_host_step2.sh build/fixtures/*.txt
set -e
apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev >/dev/null 2>&1
M=external/mbedtls
R=external/rcheevos
mkdir -p /tmp/cfg
# The DSi configuration minus the ARM assembly
sed '/MBEDTLS_HAVE_ASM/d' include/mbedtls_config_ds.h > /tmp/cfg/mbedtls_config_host.h
gcc -O1 -Wall -ffunction-sections -fdata-sections -Wl,--gc-sections -Iinclude -I/tmp/cfg -I$M/include -I$R/include -DRC_NO_THREADS \
	'-DMBEDTLS_CONFIG_FILE="mbedtls_config_host.h"' \
	tests/host_step2.c source/https.c source/raset.c source/certs.c $M/library/*.c \
	$R/src/rapi/rc_api_common.c $R/src/rapi/rc_api_runtime.c $R/src/rapi/rc_api_user.c \
	$R/src/rc_compat.c $R/src/rc_util.c $R/src/rc_version.c $R/src/rhash/md5.c \
	$R/src/rcheevos/format.c -o /tmp/host_step2 2>&1 | grep -E "error|undefined|(host_step2|https|raset).c:" || true
/tmp/host_step2 "$@"
