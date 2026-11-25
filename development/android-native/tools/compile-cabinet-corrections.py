"""Extract original cabinet AFP and source glyphs bound by the Android UI."""

import argparse
from importlib import import_module
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

from bemani.format import IFS
from PIL import Image


def textures(graphics, archive, destination, prefixes):
    source = IFS((graphics / f"{archive}.ifs").read_bytes(), decode_textures=True)
    for entry in source.filenames:
        name = entry.split("\\")[-1]
        if entry.startswith("tex\\") and name.endswith(".png") and name.startswith(prefixes):
            (destination / name).write_bytes(source.read_file(entry))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graphics", type=Path)
    parser.add_argument("assets", type=Path)
    parser.add_argument("--ffmpeg", type=Path, help="Extract entry and difficulty audio from original S3P")
    parser.add_argument("--part", choices=("timer", "entry", "songs", "loading", "selection"), required=True)
    args = parser.parse_args()
    original = import_module("prepare-cabinet-afp")
    folder = {"timer": "cabinet_timer", "entry": "entry_original", "songs": "song_row",
              "loading": "next_area", "selection": "selection_ui"}[args.part]
    output = args.assets / folder
    original.prepare(args.graphics, output, *original.SCENES[folder])
    if args.part == "timer":
        textures(args.graphics, "psd_num", output, ("num_time_",))
    elif args.part == "entry":
        original.prepare(args.graphics, args.assets / "caution_original", *original.SCENES["caution_original"])
        for archive in ("card_entry", "card_entry_ja", "caution", "caution_ja"):
            textures(args.graphics, archive, output, ("txt_", "num_", "name_moji_"))
    elif args.part == "songs":
        textures(args.graphics, "psd_level", output, ("level_", "num_"))
    elif args.part == "loading":
        font = args.graphics.parent / "font/DF-HeiSeiMaruGothic-W4.ttc"
        (args.assets / "font").mkdir(exist_ok=True)
        shutil.copyfile(font, args.assets / "font" / font.name)
        textures(args.graphics, "loading", output, ("track_",))
        textures(args.graphics, "psd_num", output, ("num_level_",))
    else:
        for archive in ("ms_sel", "ms_sel_ja"):
            textures(args.graphics, archive, output, (
                "txt_", "text_", "panel_sort", "cursor_sort", "lamp_panel_sort",
                "box_ratestyle", "panel_effective", "panel_excessive", "panel_ultra",
                "icon_favorite", "sel_dmjk", "panel_folder_hole", "bar2", "bar_lamp2",
            ))
        # 1802893b3 supplies the caption quad's black .4/.8 corner alpha.
        caption = Image.new("RGBA", (32, 1))
        caption.putdata([(0, 0, 0, round(102 + 102 * x / 31)) for x in range(32)])
        caption.save(output / "scroll_label_bg.png")

    if args.part in ("entry", "selection") and args.ffmpeg:
        audio = output / "audio"
        audio.mkdir(exist_ok=True)
        with TemporaryDirectory(prefix="nabla-entry-audio-") as temp:
            for bank in ("se_ver7_02_in", "se_ver7_03_out"):
                decoded = Path(temp) / bank
                subprocess.run([sys.executable, str(Path(__file__).with_name("extract-audio-bank.py")),
                                str(args.graphics.parents[1] / "sound/ver7" / f"{bank}.s3p"),
                                str(args.ffmpeg), str(decoded)], check=True)
                kind = "entry" if args.part == "entry" else "difficulty"
                for file in decoded.glob(f"*{kind}*.wav"):
                    shutil.copyfile(file, audio / file.name)


if __name__ == "__main__":
    main()
