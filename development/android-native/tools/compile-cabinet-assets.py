"""Export the NABLA guest cabinet compositions and installed song details."""
import argparse
from importlib import import_module
from pathlib import Path
import shutil
import subprocess

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT.parents[1]
SOURCE_ASSETS = PROJECT / "assets-reference"
ASSETS = PROJECT / "src/main/assets"
DATA = ROOT / "2026101300/LAZY_KFC/contents/data"
EXTRACTED = PROJECT / "build/asset-source"

def copy_art(ifstools):
    out = ASSETS / "menu/art"
    out.mkdir(parents=True, exist_ok=True)
    groups = {
        "mode_credit": ("6_pay_*.png", "6_logo_credit_s.png", "6_logo_paseli_s.png", "tex_win_line.png",
                        "bg_mat.png", "bg_mat2.png", "base_sentaku*.png"),
        "mode_credit_ja": ("6_panel_s_text*.png", "6_panel_text*.png", "6_pay_text_*.png",
                           "text_select_mode.png"),
        "ms_sel": ("panel_mainfolder.png", "panel_subfolder.png", "panel_main.png", "panel_sub.png",
                   "panel_sort.png", "panel_pcb.png",
                   "panel_music*.png", "panel_effective.png", "panel_excessive*.png",
                   "box_blc.png", "box_pc.png", "box_pcb.png", "box_level.png", "box_jk.png",
                   "jacket_*.png", "grade_*.png", "mark_*.png", "hex_*.png", "bar*.png",
                   "bg_sel_flame*.png", "bg_bpm_info_sel.png", "lamp_panel*.png",
                   "text_bestscore.png", "text_best_score_sel.png", "track_*_i_eeb.png"),
        "ms_sel_ja": ("box_category_sort.png", "text_sort.png", "text_back.png",
                      "text_level_select.png", "txt_sort*.png"),
        "psd_num": ("num_level_*.png", "num_time_*.png"),
        "psd_level": ("level_small_*.png",),
        "matching_music": ("box_*_mat.png", "skip_on.png", "hud_track_*.png", "text_track_mat.png",
                           "tex_bg_mat*.png"),
        "matching_music_ja": ("text_button_mat*.png", "skiptogo.png"),
        "play_data_small": ("matching_mine.png", "stagemain_mine_base.png", "stagemain_mine_mask.png",
                            "stagemain_crew_base.png", "hex_i_feb.png", "loading_mine_base*.png"),
    }
    EXTRACTED.mkdir(parents=True, exist_ok=True)
    archives = [DATA / "graphics" / ("ver06" if name == "play_data_small" else "ver07") / (name + ".ifs")
                for name in (*groups, "game_hud")]
    subprocess.run([ifstools, "-sy", "-o", EXTRACTED, *archives], check=True)
    for name, patterns in groups.items():
        for pattern in patterns:
            for source in (EXTRACTED / (name + "_ifs") / "tex").glob(pattern):
                if not (SOURCE_ASSETS / "menu/art" / source.name).is_file():
                    shutil.copyfile(source, out / source.name)
    shutil.copyfile(EXTRACTED / "game_hud_ifs/tex/hud_track_05.png", ASSETS / "hud/hud_track_05.png")


def export_player_art(ifstools):
    graphics = DATA / "graphics"
    out = ASSETS / "menu/art"
    # radar.ifs is a shared archive referenced by the NABLA executable.
    subprocess.run([ifstools, "-sy", "-o", EXTRACTED,
                    graphics / "ver06/radar.ifs", graphics / "ver06/psd_skill.ifs", graphics / "ver07/nameplate.ifs",
                    graphics / "ver07/psd_stamp_bg.ifs", graphics / "ver07/play_data.ifs"], check=True)
    for archive, pattern in (("radar", "radar_*.png"), ("nameplate", "base_player_info_*.png"),
                             ("psd_stamp_bg", "bg_stamp_default.png"), ("play_data", "base_small_stagemain.png"),
                             ("psd_skill", "skill_back_panel_none.png")):
        for source in (EXTRACTED / (archive + "_ifs") / "tex").glob(pattern):
            shutil.copyfile(source, out / source.name)
    for name in ("ap_0001.png", "ap_0002.png", "ap_0003.png"):
        shutil.copyfile(graphics / "ap_card" / name, out / name)


def export_clips():
    afp = import_module("prepare-cabinet-afp")
    for folder in ("track_entry", "track_header", "track_intro", "payment", "player_card", "ex_track"):
        output = ASSETS / folder
        afp.prepare(DATA / "graphics/ver07", output, *afp.SCENES[folder])
        afp.SKILL_TOTAL.copy_static(DATA / "graphics/ver07", output, folder)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ifs-tools", default="ifstools", help="Path to the existing ifstools executable")
    args = parser.parse_args()
    copy_art(args.ifs_tools)
    export_player_art(args.ifs_tools)
    export_clips()
