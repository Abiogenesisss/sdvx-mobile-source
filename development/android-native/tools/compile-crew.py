"""Extract original MY ROOM crew selector AFP, portraits and icon regions."""

import argparse
from importlib import import_module
from pathlib import Path
import shutil
from xml.etree import ElementTree

from bemani.format import IFS
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", type=Path, help="Existing compatibility argument for the evidence directory")
    parser.add_argument("game_data", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--skip-background", action="store_true")
    args = parser.parse_args()
    graphics = args.game_data / "graphics"
    original = import_module("prepare-cabinet-afp")
    original.prepare(graphics / "ver07", args.output, *original.SCENES["crew_selector"])
    if not args.skip_background:
        original.prepare(graphics / "ver07", args.output / "background", *original.SCENES["crew_selector/background"])

    archive = IFS((graphics / "chara/chara_icon.ifs").read_bytes(), decode_binxml=True)
    index = ElementTree.fromstring(archive.read_file("tex\\texturelist.xml"))
    regions = {entry.attrib["name"]: entry for entry in index.findall("texture/image")}
    for role, name, portrait in (
        ("rasis", "name0_rasis_i_eab", "psd_crew_0070"),
        ("grace", "name0_grace", "psd_crew_0069"),
    ):
        target = args.output / role
        target.mkdir(exist_ok=True)
        for key, texture, size in (("icon", f"icon_{role}_ver7_i_eab", (126, 126)), ("name", name, (280, 40))):
            region = regions[texture]
            img = [int(v) / 2 for v in region.findtext("imgrect").split()]
            uv = [int(v) / 2 for v in region.findtext("uvrect").split()]
            assert (img[1] - img[0], img[3] - img[2]) == size
            assert (uv[0] - img[0], uv[2] - img[2], uv[1] - uv[0], uv[3] - uv[2]) == (.5, .5, size[0] - 1, size[1] - 1)
            Image.frombytes("RGBA", size, archive.read_file("tex\\" + texture), "raw", "BGRA").save(target / f"{key}.png")
        shutil.copyfile(graphics / "crew_icon" / f"{portrait}.png", target / "portrait.png")


if __name__ == "__main__":
    main()
