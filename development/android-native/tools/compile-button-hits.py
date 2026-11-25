"""Extract original button-effects AFP and released hold note artwork."""

import argparse
from importlib import import_module
from pathlib import Path

from bemani.format import IFS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="Original graphics/ver06/game_effect.ifs")
    parser.add_argument("output", type=Path, help="button_hit asset directory")
    args = parser.parse_args()
    original = import_module("prepare-cabinet-afp")
    original.prepare(args.archive.parent.parent / "ver07", args.output, *original.SCENES["button_hit"])
    notes = IFS(args.archive.with_stem("game_notes").read_bytes(), decode_textures=True)
    stage = args.output.parent / "stage"
    stage.mkdir(parents=True, exist_ok=True)
    for name in ("notes_BT_long_offlp.png", "notes_FX_long_offlp.png"):
        (stage / name).write_bytes(notes.read_file("tex\\" + name))


if __name__ == "__main__":
    main()
