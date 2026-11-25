"""Extract original NABLA NEMSYS animations, console catalogue and body art."""

import argparse
from importlib import import_module
import json
from pathlib import Path
import shutil
from xml.etree import ElementTree

from bemani.format import IFS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graphics", type=Path, help="Original graphics/ver07 directory")
    parser.add_argument("output", type=Path, help="Android stage_buttons asset directory")
    parser.add_argument("--game-data", type=Path, help="Original contents/data for the NEMSYS catalogue and body art")
    args = parser.parse_args()
    original = import_module("prepare-cabinet-afp")
    original.prepare(args.graphics, args.output, *original.SCENES["stage_buttons"])
    if not args.game_data:
        return
    output = args.output.parent / "game_nemsys"
    original.prepare(args.graphics, output, *original.SCENES["game_nemsys"])

    data = ElementTree.fromstring((args.game_data / "others/custom_nemsys.xml").read_bytes().decode("cp932"))
    catalog = []
    for row in data.findall("info"):
        number = int(row.findtext("id"))
        name = row.findtext("texture_name") + ".png"
        catalog.append({"id": number, "image": name, "io": row.findtext("io_type_suffix")})
        shutil.copyfile(args.game_data / "graphics/game_nemsys" / name, output / name)
    assert [row["id"] for row in catalog] == list(range(60))
    archive = IFS((args.graphics / "game_nemsys.ifs").read_bytes(), decode_textures=True)
    for suffix in ("i", "ii", "iii", "iv"):
        names = [f"io_front_{suffix}"]
        if suffix != "i":
            names.extend((f"hantei_line_{suffix}", f"hantei_kikai_ef_{suffix}"))
        for name in names:
            file = name + ".png"
            (output / file).write_bytes(archive.read_file("tex\\" + file))
    (output / "catalog.json").write_text(json.dumps(catalog, separators=(",", ":")), encoding="utf-8")


if __name__ == "__main__":
    main()
