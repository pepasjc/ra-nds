# RA Direct (proof of concept)

HTTPS from a Nintendo DSi straight to RetroAchievements, with no server in
between: step 1 of letting the DSi talk to RA itself instead of through the
GameSync server.

`radirect.nds` connects with the console's saved WiFi settings (DSi mode, so the
WPA2 connections 4-6 work), opens TLS 1.2 to retroachievements.org with mbedTLS,
verifies the certificate chain against the roots in `certs/` (GTS Root R4 and
GlobalSign Root CA, via Cloudflare's WE1) and the DSi's clock, then asks RA for
the game id of Tetris DS (USA) by its hash, an unauthenticated read. It prints
how long WiFi, DNS/TCP, the TLS handshake and the request took.

Separate from nds-bootstrap-ra and GameSync on purpose: nothing here changes them.

## Build

devkitARM with libnds 2.x/calico (DSi-mode WiFi):

    git submodule update --init
    make

mbedTLS 2.28.8 (`external/mbedtls`) is compiled with `include/mbedtls_config_ds.h`:
TLS 1.2 client only; ECDHE-ECDSA/ECDHE-RSA; P-256, P-384, X25519; AES-GCM and
ChaCha20-Poly1305; certificate validity from the RTC; entropy from timer jitter
(`source/entropy.c`, proof-of-concept grade).

`tools/embed_certs.py` regenerates `source/certs.c` from `certs/*.pem`.

## Host check

`tests/run_host_tls.sh` builds the same configuration, roots and calls for the PC
(POSIX sockets, /dev/urandom) and talks to retroachievements.org, so the cipher
list, chain and request are proven before the DSi runs them.
