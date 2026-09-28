# RA-NDS: RetroAchievements on a real Nintendo DSi

Earn [RetroAchievements](https://retroachievements.org) in Nintendo DS games on
a real DSi or DSi XL, with no PC or server in between. The achievements are
checked while you play, an unlock pops up with a chime, and when you quit the
game the DSi sends your unlocks to your RetroAchievements profile itself, over
HTTPS.

It is two pieces that work together:

- **[nds-bootstrap-ra](https://github.com/pepasjc/nds-bootstrap-ra)**, a fork of
  nds-bootstrap (the loader TWiLight Menu++ uses to run DS games) with a
  RetroAchievements engine (rcheevos) on the ARM7, the unlock popup, and an
  achievements list on **Select + Down**.
- **RA Sync** (this repository, `rasync.nds`), which the loader starts before a
  game to fetch its achievement set and after it to send the unlocks.

> **Unofficial client.** This isn't a RetroAchievements-approved emulator.
> Unlocks are sent as **softcore** only. Hardcore is built in but switched off
> until RetroAchievements reviews the client.

## What you need

- A **Nintendo DSi or DSi XL** with **[Unlaunch](https://dsi.cfw.guide/)**
  installed (RA Prep and RA Sync start through Unlaunch). A 3DS, a DS Lite or a
  flashcard won't work.
- **[TWiLight Menu++](https://github.com/DS-Homebrew/TWiLightMenu) v27.24.1**
  (it comes with nds-bootstrap v2.16.0, which nds-bootstrap-ra is based on).
- **WiFi saved in the DSi's own settings**. For WPA/WPA2 use connections 4-6:
  *System Settings > Internet > Connection Settings > Advanced Setup*.
- The DSi's **clock set correctly** (certificates are checked against it).
- A **RetroAchievements account**.
- ROMs whose dumps RetroAchievements recognises (the game's page on
  retroachievements.org lists the supported hashes). Games RA has no set for
  just play as usual.

## Install

1. Download `ra-nds-<version>.zip` from
   [Releases](https://github.com/pepasjc/ra-nds/releases) and unzip it.
2. Copy its `_nds` folder to the root of the DSi's SD card, merging with the
   `_nds` folder there. This replaces TWiLight Menu++'s
   `nds-bootstrap-nightly.nds` and `nds-bootstrap-hb-nightly.nds` with
   nds-bootstrap-ra and adds `_nds/ra/` (keep a copy of the originals if you
   want to go back).
3. Make TWiLight Menu++ use the nightly nds-bootstrap: *TWiLight Menu++
   Settings > Games and Apps settings > nds-bootstrap: Nightly*.
4. Rename `_nds/ra/account.txt.example` to `_nds/ra/account.txt` and put in
   your RetroAchievements user name and password:

       user=YourUserName
       password=YourPassword
       submit=1

   The first time RA Sync runs, it logs in, saves the connect token RA gives
   it (`token=...`) and **removes the password from the file**. `submit=0`
   makes it a dry run: unlocks are recorded but not sent.

That's it. Start a game from TWiLight Menu++.

## How it plays

- **First start of a game**: the console restarts into *RA Prep*. It hashes
  the ROM, looks it up on RetroAchievements over WiFi, saves the achievement
  set (and which achievements your account already has), then restarts into
  the game. About 10-15 seconds, once per game. Games RA doesn't know are
  noted and start straight away after that. A replaced or changed ROM goes
  through RA Prep again.
- **Playing**: unlocks show a popup with a chime (the game pauses for about
  3 seconds). **Select + Down** shows the game's achievements, locked and
  unlocked.
- **Quitting** through the nds-bootstrap in-game menu: the console restarts
  into *RA Sync*, which sends the new unlocks to your profile and goes back to
  TWiLight Menu++. Without WiFi they stay on the SD card and go next time (RA
  Prep also sends any that are waiting when it's online for a new game).
- **WiFi**: RA Prep and RA Sync give the connection 30 seconds (**B** skips).
  If the DSi's WiFi hangs while starting up, which can happen after a game
  that used DS wireless, the console restarts once and tries again; if it
  still won't connect, the game starts without achievements (RA Prep) or the
  unlocks wait for next time (RA Sync).
- The in-game menu hides nothing in softcore; cheats work as usual.

## Files on the SD card

| File | What it is |
|---|---|
| `_nds/nds-bootstrap-nightly.nds`, `_nds/nds-bootstrap-hb-nightly.nds` | nds-bootstrap-ra |
| `_nds/ra/raprep.nds`, `_nds/ra/rasync.nds` | RA Sync (the same program; it knows its job from the files below) |
| `_nds/ra/account.txt` | User name and RA connect token (keep it private) |
| `_nds/ra/config.txt` | Engine settings (see the comments in it) |
| `_nds/ra/sets/<rom>.txt`, `.sig` | A game's achievement set and its signature |
| `_nds/ra/sets/<rom>.unl` | Achievements your account already had |
| `_nds/ra/sets/<rom>.id`, `.none` | ROM fingerprint; "RA has no set for this ROM" |
| `_nds/ra/unlocks.bin` | Signed unlock records |
| `_nds/ra/unlocks_history.txt` | The same, readable (nothing reads it back) |
| `_nds/ra/ra_submitted.txt` | How many unlocks have been sent |
| `_nds/ra/rasync_log.txt` | What RA Prep and RA Sync did, for troubleshooting |

Unlock records and sets are signed with a key derived from the console's own
eMMC ID, so records edited on the SD card or copied from another console are
never sent, and edited sets aren't loaded.

## Troubleshooting

- **"No connection" / "WiFi didn't come up"**: check the connection in the
  DSi's System Settings (WPA2 needs connections 4-6). If it keeps failing
  right after a game, turning the DSi off and on again resets the WiFi chip.
- **A game never gets achievements**: its dump probably isn't one RA
  recognises. Delete `_nds/ra/sets/<rom>.none` after replacing the ROM (or just
  replace it: a changed ROM is checked again automatically).
- **Something went wrong**: `_nds/ra/rasync_log.txt` says what RA Prep and RA
  Sync saw.
- **Going back**: restore TWiLight Menu++'s own nds-bootstrap files, or set
  nds-bootstrap back to *Release*. Delete `_nds/ra/raprep.nds` to stop the
  pre-game fetch.

## Building

You need devkitPro: devkitARM with **libnds 2.x/calico** for this repository,
and Docker for nds-bootstrap-ra (it builds in the `devkitpro/devkitarm:20241104`
image, libnds 1.x).

    git clone --recursive https://github.com/pepasjc/ra-nds
    git clone --recursive -b retroachievements https://github.com/pepasjc/nds-bootstrap-ra
    python ra-nds/tools/make_secret.py nds-bootstrap-ra
    (cd ra-nds && make)                              # rasync.nds, ratest.nds
    (cd nds-bootstrap-ra && ./docker-build.sh)          # bin/nds-bootstrap-*nightly.nds

`make_secret.py` writes the same random `ra_secret.h` into both trees (it isn't
in git). Both must be built with the same secret, and a new one invalidates
every signature on an SD card that used the old one.

The pieces:

- `app/rasync.c`: RA Prep and RA Sync. `app/ratest.c`: a test that fetches
  every set on the card straight from RA.
- `source/https.c`: HTTPS client (mbedTLS 2.28 in `external/mbedtls`, built with
  `include/mbedtls_config_ds.h`; TLS 1.2, certificates checked against
  `certs/`), with keep-alive and timeouts that work around dswifi's sgIP stack.
- `source/ra_client.c`: RetroAchievements calls through rcheevos' `rc_api`
  (`external/rcheevos`).
- `source/ra_key.c`: the console key; `source/raset.c`: the set file format;
  `source/nds_hash.c`: RA's DS ROM hash.
- `tests/run_host_step2.sh`: the same HTTPS and rc_api code on a PC against the
  real server.

## Credits

[RetroAchievements](https://retroachievements.org) and
[rcheevos](https://github.com/RetroAchievements/rcheevos),
[nds-bootstrap](https://github.com/DS-Homebrew/nds-bootstrap) and
[TWiLight Menu++](https://github.com/DS-Homebrew/TWiLightMenu) (DS-Homebrew),
[Mbed TLS](https://github.com/Mbed-TLS/mbedtls), devkitPro.
