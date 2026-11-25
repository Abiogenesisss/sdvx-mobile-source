"""Extract original laser AFP effects and note materials."""

import argparse
from importlib import import_module
from pathlib import Path

from bemani.format import IFS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graphics", type=Path)
    parser.add_argument("assets", type=Path)
    args = parser.parse_args()
    stage = args.assets / "stage"
    stage.mkdir(parents=True, exist_ok=True)
    notes = IFS((args.graphics / "ver06/game_notes.ifs").read_bytes(), decode_textures=True)
    for name in notes.filenames:
        if name.startswith("tex\\notes_AD_") and "_w_" not in name:
            (stage / name.split("\\")[-1]).write_bytes(notes.read_file(name))
    original = import_module("prepare-cabinet-afp")
    for folder in ("laser_effect", "laser_notice"):
        original.prepare(args.graphics / "ver07", args.assets / folder, *original.SCENES[folder])


if __name__ == "__main__":
    main()
