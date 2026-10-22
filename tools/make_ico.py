#!/usr/bin/env python3
"""Packs icon_<size>.png files into a Windows .ico (PNG-compressed entries).

The PNGs come from the GUI itself, so the icon has one source (AppIcon.cc):
    andor_emccd_gui --export-icon DIR
    python3 tools/make_ico.py DIR src/exec/gui/resources/andor_emccd_gui.ico
Standard library only.
"""

import pathlib
import struct
import sys

SIZES = [16, 24, 32, 48, 64, 128, 256]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src = pathlib.Path(sys.argv[1])
    images = [(s, (src / f"icon_{s}.png").read_bytes()) for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries, data = b"", b""
    for size, png in images:
        dim = 0 if size >= 256 else size  # 0 means 256 in the ICO format.
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png),
                               offset + len(data))
        data += png
    pathlib.Path(sys.argv[2]).write_bytes(header + entries + data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
