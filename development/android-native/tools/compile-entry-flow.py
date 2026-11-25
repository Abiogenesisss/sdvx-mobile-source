"""Export original entry, room, shutter and end-of-session AFP."""
import argparse
from importlib import import_module
from io import BytesIO
from pathlib import Path

from bemani.format import IFS
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graphics", type=Path)
    parser.add_argument("assets", type=Path)
    folders = ("shutter_nabla", "session_dialog", "entry_flow", "room_flow", "entry_wait", "mode_flow",
               "entry_profile", "entry_checkin")
    parser.add_argument("--only", choices=folders)
    args = parser.parse_args()
    afp = import_module("prepare-cabinet-afp")
    for folder in (args.only,) if args.only else folders:
        output = args.assets / folder
        afp.prepare(args.graphics, output, *afp.SCENES[folder])
        if folder == "entry_wait":
            entry = IFS((args.graphics / "card_entry_ja.ifs").read_bytes(), decode_textures=True)
            with Image.open(BytesIO(entry.read_file("tex\\txt_wait.png"))) as glyph:
                caption = Image.new("RGBA", glyph.size, "white")
                caption.putalpha(glyph.convert("RGBA").getchannel("A"))
                caption.save(output / "txt_wait.png")
        elif folder == "entry_checkin":
            play_data = IFS((args.graphics / "play_data.ifs").read_bytes(), decode_textures=True)
            for name in ("box_pc.png", "box_bloc.png"):
                (output / name).write_bytes(play_data.read_file(f"tex\\{name}"))
        elif folder == "entry_flow":
            system = IFS((args.graphics / "cmn_sys.ifs").read_bytes(), decode_textures=True)
            (output / "txt_generator.png").write_bytes(system.read_file("tex\\txt_generator.png"))


if __name__ == "__main__":
    main()
