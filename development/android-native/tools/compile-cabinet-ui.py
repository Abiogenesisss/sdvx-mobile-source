"""Export additional original menu and track-end artwork."""
import argparse
from importlib import import_module
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", type=Path)
    parser.add_argument("assets", type=Path)
    parser.add_argument("part", choices=("menu", "end"))
    args = parser.parse_args()
    play = args.case / "playthrough"
    scenes = play / "scenes"
    afp = import_module("prepare-cabinet-afp")
    graphics = Path(__file__).resolve().parents[3] / "2026101300/LAZY_KFC/contents/data/graphics/ver07"
    if args.part == "menu":
        output = args.assets / "cabinet"
        afp.prepare(graphics, output, *afp.SCENES["cabinet"])
        for archive, files in {
            "cmn_sys": ["cmn_timerbase", "txt_time"],
            "ms_sel": ["panel_mainfolder", "panel_subfolder"],
            "option": ["mainop_tex_panel", "mainop_tex_select", "mainop_title_s_critical"],
            "option_ja": ["mainop_title_lane_speed", "mainop_title_score_style",
                          "mainop_title_early_late_style", "mainop_title_early_late_position",
                          "mainop_title_random_mirror", "mainop_bg"],
        }.items():
            for name in files:
                shutil.copyfile(scenes / (archive + "_ifs") / "tex" / (name + ".png"),
                                output / (name + ".png"))
        for jacket in (scenes / "psd_level_ifs/tex").glob("level_panel_*.png"):
            shutil.copyfile(jacket, args.assets / "menu/art" / jacket.name)
        for name in ("font_early", "font_late", "pt_option_win"):
            shutil.copyfile(args.case / "original-layout/game_hud_ifs/tex" / (name + ".png"),
                            output / (name + ".png"))
        shutil.copyfile(args.case / "original-layout/game_hud_ifs/tex/gauge_excessive.png",
                        output / "gauge_excessive.png")
        shutil.copyfile(args.case / "rendering/gauge/gauge_ifs/tex/gauge_excessive_naka.png",
                        output / "gauge_excessive_naka.png")
    else:
        output = args.assets / "stage_end"
        afp.prepare(graphics, output, *afp.SCENES["stage_end"])
        for path in (play / "audio/se_ver7_07_comp").glob("*.wav"):
            shutil.copyfile(path, output / path.name)


if __name__ == "__main__":
    main()
