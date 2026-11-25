"""Extract original gameplay HUD AFP and directly bound texture assets."""

import argparse
from importlib import import_module
from pathlib import Path

from bemani.format import IFS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graphics", type=Path, help="Original graphics/ver07 directory")
    parser.add_argument("output", type=Path, help="Android assets directory")
    args = parser.parse_args()
    original = import_module("prepare-cabinet-afp")
    for folder in ("hud", "hud_chain"):
        original.prepare(args.graphics, args.output / folder, *original.SCENES[folder])

    hud = args.output / "hud"
    archive = IFS((args.graphics / "game_hud.ifs").read_bytes(), decode_textures=True)
    for name in ("gauge_permissiv", "gauge_ultra", "gauge_ultra_ars",
                 *(f"hud_track_{track:02d}" for track in range(1, 7))):
        file = name + ".png"
        (hud / file).write_bytes(archive.read_file("tex\\" + file))
    digits = IFS((args.graphics / "psd_num.ifs").read_bytes(), decode_textures=True)
    for family in ("num_score", "num_score_small", "num_chain_a", "num_chain_b"):
        for digit in range(10):
            file = f"{family}_{digit}.png"
            (hud / file).write_bytes(digits.read_file("tex\\" + file))


if __name__ == "__main__":
    main()
