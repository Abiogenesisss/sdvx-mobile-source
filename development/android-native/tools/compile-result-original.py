"""Export original single-track result AFP and original runtime artwork."""
import argparse
from importlib import import_module
from pathlib import Path
import shutil

from bemani.format import IFS
from PIL import Image


def copy_textures(graphics, archive_name, output, names):
    archive = IFS((graphics / f"{archive_name}.ifs").read_bytes(), decode_textures=True)
    for name in names:
        file = f"tex\\{name}.png"
        if file in archive.filenames:
            (output / f"{name}.png").write_bytes(archive.read_file(file))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data", type=Path)
    parser.add_argument("assets", type=Path)
    parser.add_argument("--work", type=Path, help=argparse.SUPPRESS)
    specific = parser.add_mutually_exclusive_group()
    for option in ("root", "gauge", "details", "grade"):
        specific.add_argument(f"--{option}-only", action="store_true")
    args = parser.parse_args()
    graphics = args.data / "graphics/ver07"
    afp = import_module("prepare-cabinet-afp")
    folders = {"root": "result_original", "gauge": "result_gauge", "details": "result_details",
               "grade": "result_grade"}
    selected = next((folder for name, folder in folders.items() if getattr(args, f"{name}_only")), None)
    for folder in (selected,) if selected else (*folders.values(), "result_score", "result_profile"):
        output = args.assets / folder
        afp.prepare(graphics, output, *afp.SCENES[folder])
        if folder == "result_original":
            copy_textures(graphics, "psd_num", output, (f"num_score_{i}" for i in range(10)))
            copy_textures(graphics, "result", output, (
                "txt_mirror", "txt_effective_rate", "txt_excessive_rate", "txt_ultra_rate",
                "grade_s", "grade_aaa_plus", "grade_aaa", "grade_aa_plus", "grade_aa", "grade_a_plus",
                "grade_a", "grade_b", "grade_c", "grade_d", "num_score_plus", "num_score_minus",
                "bar_white", "bar_yellow", "icon_best"))
        elif folder == "result_details":
            copy_textures(graphics, "result_ja", output,
                          ("box_update_world", "box_update_store", "text_update_rival", "pt_box"))
            copy_textures(graphics, "result", output, ("bpass_r_icon",))
            copy_textures(graphics, "game_hud", output, (f"pt_result_icon_{name}" for name in (
                "none", "s", "aaa_plus", "aaa", "uc", "puc", "best")))
        elif folder == "result_profile":
            copy_textures(graphics, "psd_stamp_bg", output, ("bg_stamp_default",))
            shutil.copyfile(graphics.parent / "board_base/base_0001.png", output / "local_board_base.png")
            Image.new("RGBA", (600, 300)).save(output / "local_empty_stamp_canvas.png")
            Image.new("RGBA", (960, 360)).save(output / "local_empty_nameplate_frame.png")


if __name__ == "__main__":
    main()
