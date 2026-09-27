"""Write the build secret both projects compile in (not in git).

The DSi console key that signs unlock records and sets is SHA-256 of this
secret, the console's eMMC CID and a label. nds-bootstrap-ra (which signs)
and RA Sync (which checks before sending) must be built with the same file.

    python tools/make_secret.py <path to nds-bootstrap-ra>

writes include/ra_secret.h here and retail/common/include/ra_secret.h there,
unless they exist (a new secret invalidates every signature: all sets are
fetched again and unsent unlocks can no longer be sent).
"""
import os
import pathlib
import sys

HEADER = """// Build secret for the RetroAchievements console key (tools/make_secret.py).
// Not in git: nds-bootstrap-ra and ra-direct must be built with the same file.
#ifndef RA_SECRET_H
#define RA_SECRET_H
static const {type} raBuildSecret[32] = {{ {bytes} }};
#endif
"""


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    here = pathlib.Path(__file__).resolve().parent.parent
    targets = [
        (here / "include" / "ra_secret.h", "unsigned char"),
        (pathlib.Path(sys.argv[1]) / "retail" / "common" / "include" / "ra_secret.h", "u8"),
    ]
    existing = [p for p, _ in targets if p.exists()]
    if existing:
        sys.exit(f"Already there, not replaced: {', '.join(map(str, existing))}")
    secret = ", ".join(f"0x{b:02x}" for b in os.urandom(32))
    for path, type_ in targets:
        path.write_text(HEADER.format(type=type_, bytes=secret), newline="\n")
        print("wrote", path)


if __name__ == "__main__":
    main()
