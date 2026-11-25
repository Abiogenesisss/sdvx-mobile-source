"""Convert locally supplied NABLA IFS, AFP, textures and cabinet audio for Android."""
import argparse
from fnmatch import fnmatchcase
from functools import lru_cache
from importlib import import_module
import json
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory


TOOLS = Path(__file__).resolve().parents[1] / "development/android-native/tools"
PARTS = ("cabinet", "stage", "backgrounds", "scenes", "audio")


def run(script, *args):
    print(f"Exporting {script}", flush=True)
    subprocess.run([sys.executable, TOOLS / (script + ".py"), *map(str, args)], check=True)


@lru_cache(maxsize=2)
def image_files(path):
    from ifstools import IFS

    archive = IFS(str(path))
    return next(folder.files for folder in archive.tree.all_folders if folder.name == "tex")


def textures(archive, output, *patterns):
    output.mkdir(parents=True, exist_ok=True)
    for name, file in image_files(archive).items():
        if any(fnmatchcase(name, pattern) for pattern in patterns):
            (output / name).write_bytes(file.load())


def copy_texture(archive, output, name, basename):
    output.mkdir(parents=True, exist_ok=True)
    (output / basename).write_bytes(image_files(archive)[name].load())


def cabinet(data, common, override):
    graphics = data / "graphics"
    ver07 = graphics / "ver07"
    run("prepare-cabinet-afp", ver07, common, "menu")
    run("prepare-cabinet-afp", ver07, override, "title_original", "cabinet", "track_entry", "track_header",
        "track_intro", "payment", "player_card", "matching", "settings", "skill_selector",
        "skill_effects", "skill_effects/windows", "ex_track")
    run("compile-entry-flow", ver07, override)
    for part in ("timer", "entry", "songs", "loading", "selection"):
        run("compile-cabinet-corrections", ver07, override, "--part", part)
    run("compile-result-original", data, override)
    run("compile-result-rewards", data, override, data)
    run("compile-generator-interaction", graphics, override / "generator_vii")
    run("compile-room-assets", graphics, override)
    cabinet_art(graphics, common, override)


def cabinet_art(graphics, common, override):
    ver07 = graphics / "ver07"
    menu = common / "menu/art"
    for archive, patterns in {
        "mode_credit": ("6_panel*title*.png",),
        "psd_level": ("level_panel_*.png",),
        "my_room": ("box_bg_gene.png", "box_bg_start.png", "box_cursor1.png",
                    "text_box_gene_val.png", "text_box_gene.png", "text_box_start_sel.png"),
        "cmn_bg": ("bg_iseki.png",),
        "title": ("title_chara_grace.png",),
    }.items():
        textures(ver07 / f"{archive}.ifs", menu, *patterns)
    for archive, patterns in {
        "ver07/mode_credit": ("6_pay_*.png", "6_logo_credit_s.png", "6_logo_paseli_s.png", "tex_win_line.png",
                              "bg_mat.png", "bg_mat2.png", "base_sentaku*.png"),
        "ver07/mode_credit_ja": ("6_panel_s_text*.png", "6_panel_text*.png", "6_pay_text_*.png",
                                 "text_select_mode.png"),
        "ver07/ms_sel": ("panel_mainfolder.png", "panel_subfolder.png", "panel_main.png", "panel_sub.png",
                         "panel_sort.png", "panel_pcb.png", "panel_music*.png", "panel_effective.png",
                         "panel_excessive*.png", "box_blc.png", "box_pc.png", "box_pcb.png", "box_level.png",
                         "box_jk.png", "jacket_*.png", "grade_*.png", "mark_*.png", "hex_*.png", "bar*.png",
                         "bg_sel_flame*.png", "bg_bpm_info_sel.png", "lamp_panel*.png", "text_bestscore.png",
                         "text_best_score_sel.png", "track_*_i_eeb.png"),
        "ver07/ms_sel_ja": ("box_category_sort.png", "text_sort.png", "text_back.png",
                            "text_level_select.png", "txt_sort*.png"),
        "ver07/psd_num": ("num_level_*.png", "num_time_*.png"),
        "ver07/psd_level": ("level_small_*.png",),
        "ver07/matching_music": ("box_*_mat.png", "skip_on.png", "hud_track_*.png", "text_track_mat.png",
                                  "tex_bg_mat*.png"),
        "ver07/matching_music_ja": ("text_button_mat*.png", "skiptogo.png"),
        "ver06/play_data_small": ("matching_mine.png", "stagemain_mine_base.png", "stagemain_mine_mask.png",
                                   "stagemain_crew_base.png", "hex_i_feb.png", "loading_mine_base*.png"),
        "ver06/radar": ("radar_*.png",),
        "ver07/nameplate": ("base_player_info_*.png",),
        "ver07/psd_stamp_bg": ("bg_stamp_default.png",),
        "ver07/play_data": ("base_small_stagemain.png",),
        "ver06/psd_skill": ("skill_back_panel_none.png",),
    }.items():
        textures(graphics / f"{archive}.ifs", override / "menu/art", *patterns)
    for name in ("ap_0001.png", "ap_0002.png", "ap_0003.png"):
        shutil.copyfile(graphics / "ap_card" / name, override / "menu/art" / name)
    for number, suffix in enumerate("abcdefghji", 1):
        copy_texture(ver07 / "mode_credit_ja.ifs", override / "mode_flow",
                     f"6_panel_text02{suffix}.png", f"description_{number}.png")
    copy_texture(ver07 / "ms_sel.ifs", override / "selection_ui", "panel_mainfolder.png",
                 "panel_mainfolder_71a29b0776.png")
    corrections = import_module("compile-cabinet-corrections")
    corrections.textures(ver07, "psd_level", override / "song_row",
                         ("exgate_level_", "h_base_diff_", "h_diff_num_", "barrier_orange",
                          "base_level_glow", "gauge_excessive_ars"))
    result = import_module("compile-result-original")
    result.copy_textures(ver07, "result", override / "result_original", ("txt_permissive_rate",))
    result.copy_textures(ver07, "psd_num", override / "result_original", (
        f"num_score_{style}_{digit}" for style in ("gold", "silver", "small", "small_yellow") for digit in range(10)
    ))

    for archive, patterns in {
        "cmn_sys": ("cmn_timerbase.png", "txt_time.png"),
        "ms_sel": ("panel_mainfolder.png", "panel_subfolder.png"),
        "option": ("mainop_tex_panel.png", "mainop_tex_select.png", "mainop_title_s_critical.png"),
        "option_ja": ("mainop_title_lane_speed.png", "mainop_title_score_style.png",
                      "mainop_title_early_late_style.png", "mainop_title_early_late_position.png",
                      "mainop_title_random_mirror.png", "mainop_bg.png"),
        "game_hud": ("font_early.png", "font_late.png", "pt_option_win.png", "gauge_excessive.png"),
        "gauge": ("gauge_excessive_naka.png",),
    }.items():
        version = "ver06" if archive in ("option", "option_ja") else "ver07"
        textures(graphics / version / f"{archive}.ifs", common / "cabinet", *patterns)


def stage(data, common, override):
    graphics = data / "graphics"
    ver07 = graphics / "ver07"
    ver06 = graphics / "ver06"
    run("compile-hud", ver07, override)
    run("compile-laser-assets", graphics, override)
    run("compile-button-hits", ver06 / "game_effect.ifs", override / "button_hit")
    run("compile-stage-buttons", ver07, override / "stage_buttons", "--game-data", data)
    run("prepare-cabinet-afp", ver07, override, "beam", "stage_ready", "stage_end")
    stage_art(graphics, common, override)


def stage_art(graphics, common, override):
    ver06 = graphics / "ver06"
    run("compile-stage-cursors", ver06 / "game_effect.ifs", common / "stage")
    copy_texture(ver06 / "game_lane_test.ifs", common / "stage", "lane_test_a.png", "lane.png")
    textures(ver06 / "game_notes.ifs", common / "stage", "notes_BT_*.png", "notes_FX_*.png", "notes_AD_*.png")
    textures(ver06 / "game_lane.ifs", override / "stage", "line_01_shou.png")
    textures(graphics / "ver07/game_nemsys.ifs", common / "stage_buttons", "io_eff_*.png")


def archive_path(data, name):
    graphics = data / "graphics"
    preferred = graphics / "game_bg" / f"{name}.ifs"
    if preferred.is_file():
        return preferred
    candidates = sorted(p for p in graphics.glob(f"*/{name}.ifs") if p.is_file())
    if len(candidates) > 1:
        raise ValueError(f"Ambiguous source archive for {name}.ifs: {candidates}")
    return next(iter(candidates), None)


def backgrounds(data, common, index, registration):
    spec = import_module("prepare-stage-backgrounds")
    names = {f"gmbg_{entry[0]}" for entry in spec.BACKGROUNDS.values()}
    names.update(f"gmbg_{name}" for name in ("diver_tex", "diver_tex_blue", "diver_tex_white",
                                           "diver_tex_purple", "vn2026", "vn26_final"))
    archives = {}
    for name in sorted(names):
        path = archive_path(data, name)
        if path is None:
            raise FileNotFoundError(f"Missing original archive {name}.ifs for common backgrounds")
        archives[name] = path
    spec.prepare_backgrounds(lambda name: index.tree(archives[f"gmbg_{name}"]),
                             common / "stage_bg", data / "graphics/game_tex",
                             registration, spec.BACKGROUNDS, index.publish)


def scenes(data, override, index):
    spec = import_module("prepare-stage-scenes")
    songs = override / "songs"
    library = json.loads((songs / "library.json").read_text(encoding="utf-8"))
    installed = {row["id"] for row in library}
    live2d = data / "graphics/live2d"
    selected = []
    archives = {}
    for song, row in spec.SCENES.items():
        if song not in installed:
            continue
        names = {"gmbg_diver_tex", row["archive"], *row.get("texture_packs", ()),
                 *row.get("extra_afp", {}).values()}
        sources = {name: archive_path(data, name) for name in sorted(names)}
        missing = [f"{name}.ifs" for name, path in sources.items() if path is None]
        for model in row.get("models", ()):
            model_path = live2d / model["asset"]
            if not model_path.is_file():
                missing.append(str(model_path.relative_to(data)))
                continue
            refs = json.loads(model_path.read_text(encoding="utf-8-sig"))["FileReferences"]
            for name in (refs["Moc"], *refs["Textures"]):
                path = model_path.parent / name
                if not path.is_file():
                    missing.append(str(path.relative_to(data)))
        if missing:
            manifest = songs / row.get("folder", str(song)) / "background/afp-scene.json"
            manifest.unlink(missing_ok=True)
            print(f"Song {song}: skipping special background, missing {', '.join(missing)}", flush=True)
            continue
        selected.append(song)
        archives.update(sources)
    if selected:
        def package(name):
            return index.tree(archives[name])

        spec.prepare_scenes(package, package("gmbg_diver_tex") / "tex", songs, selected, live2d, index.publish)


def audio(data, common, override, ffmpeg, work):
    from bemani.format import TwoDX

    sound = data / "sound"

    def copy(source, root, name):
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)

    def export_bank(bank, destinations):
        decoded = work / bank.stem
        run("extract-audio-bank", bank, ffmpeg, decoded)
        for name, root, target in destinations:
            copy(decoded / f"{name}.wav", root, target)
        return decoded

    ver7 = sound / "ver7"
    export_bank(ver7 / "bgm_ver7.s3p", [
        ("bgm_ver7_01_title", common, "menu/audio/bgm_ver7_01_title.wav"),
        ("bgm_ver7_02_modeselect", common, "menu/audio/bgm_ver7_02_modeselect.wav"),
        ("bgm_ver7_03_musicselect", override, "menu/audio/bgm_ver7_03_musicselect.wav"),
        ("bgm_ver7_04_matching", common, "flow/matching.wav"),
        ("bgm_ver7_05_result", common, "result/bgm.wav"),
        ("bgm_ver7_06_totalresult", override, "entry_flow/total.wav"),
    ])
    for bank, groups in {
        "01_short": [("matching_ok", common, "flow"), ("achievement", override, "menu/audio")],
        "02_in": [("entry", override, "entry_original/audio"), ("difficulty", override, "selection_ui/audio"),
                  ("shutter", common, "flow")],
        "03_out": [("entry", override, "entry_original/audio"), ("difficulty", override, "selection_ui/audio"),
                   ("shutter", common, "flow")],
        "04_cursor": [("cursor", override, "menu/audio/cursor")],
        "05_decide": [("medium", common, "menu/audio"), ("high", override, "room_flow")],
        "07_comp": [("comp", common, "stage_end")],
        "08_big": [("matching", common, "flow"), ("nextarea", override, "flow")],
    }.items():
        path = ver7 / f"se_ver7_{bank}.s3p"
        names = (entry["name"] for entry, _ in import_module("extract-audio-bank").read_bank(path))
        export_bank(path, [(name, root, f"{folder}/{name}.wav")
                           for name in names for match, root, folder in groups if match in name])
    export_bank(ver7 / "se_ver7_06_rank.s3p", [
        (f"se_06_rank00{index}_{source}", override, f"result_original/audio/se_06_rank00{index}_{target}.wav")
        for index, (source, target) in enumerate((("s", "s"), ("aaa+", "aaa_plus"), ("aaa", "aaa")), 1)
    ])
    copy(work / "se_ver7_01_short/se_01_short001_modeselect.wav", override, "entry_flow/mode.wav")
    copy(work / "se_ver7_08_big/se_08_big01_main_ready.wav", override, "stage_ready/ready.wav")
    copy(work / "se_ver7_08_big/se_08_big04_endscene.wav", override, "entry_flow/ending.wav")
    copy(work / "se_ver7_04_cursor/se_04_cursor001_modeselect.wav", common, "crew/cursor.wav")
    for index, suffix in ((3, "achievement"), (4, "achievement_all")):
        copy(work / f"se_ver7_01_short/se_01_short00{index}_{suffix}.wav",
             override, f"entry_original/audio/se_01_short00{index}.wav")

    sampler = export_bank(sound / "ver5/general_sampler.s3p", [])
    bank = sampler / "bank.json"
    for entry in json.loads(bank.read_text(encoding="utf-8")):
        if entry["index"] >= 2:
            copy(sampler / entry["file"], common, f"stage/sound/{entry['index']}.wav")
    copy(bank, common, "stage/sound/general_sampler.json")
    export_bank(sound / "ver5/virtical_shot.s3p", [
        ("fs00_virtical_se01", override, "stage/sound/virtical_shot.wav"),
    ])
    export_bank(sound / "ver6/se_08_big.s3p", [
        ("se_08_big008", common, "stage/sound/se_08_big008.wav"),
        ("se_08_big009", override, "generator_vii/roll.wav"),
        ("se_08_big010", override, "generator_vii/get.wav"),
    ])
    export_bank(sound / "ver6/se_11_skiana.s3p", [
        (f"se_11_skiana{index:03}", override, f"skill_effects/audio/{name}.wav")
        for index, name in enumerate(("select-in", "analysis-success", "analysis-fail", "skill-change", "bonus-in"), 1)
    ])

    for bank, destinations in {
        "sys_sd": {
            4: (override, "entry_original/audio/s12_card_entry_num_input.wav"),
            10: (override, "entry_original/audio/s21_name_entry_decide.wav"),
            11: (override, "entry_original/audio/s22_name_entry_cursor.wav"),
            12: (override, "entry_original/audio/s23_name_entry_cancel.wav"),
            13: (override, "entry_original/audio/s24_name_entry_end.wav"),
            22: (common, "stage/sound/s50_tutorial_flame_in.wav"),
            28: (override, "skill_effects/audio/s103_music_select_level_cursor.wav"),
            52: (override, "result_force/audio/s151_result_score_move.wav"),
            53: (override, "result_force/audio/s152_result_score_decide.wav"),
            55: (override, "result_force/audio/total_build.wav"),
            59: (override, "result_force/audio/total_info.wav"),
            85: (override, "entry_original/audio/s205_timer_countdown.wav"),
            87: (override, "caution_original/audio/s207_caution.wav"),
            97: (override, "entry_original/audio/s221_general_waiting_loop_02.wav"),
            183: (override, "result_force/audio/class.wav"),
            184: (override, "result_force/audio/plus.wav"),
            185: (override, "result_force/audio/level.wav"),
        },
        "sys_sd_credit": {0: (common, "flow/coin.wav"), 1: (override, "payment/audio/paseli-ok.wav")},
    }.items():
        archive = TwoDX((sound / (bank + ".2dx")).read_bytes())
        for index, (root, name) in destinations.items():
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-y", "-i", "pipe:0",
                            "-ac", "2", "-ar", "44100", "-c:a", "pcm_s16le", target],
                           input=archive.read_file(archive.filenames[index]), check=True)
    archive = TwoDX((sound / "sys_sd.2dx").read_bytes())
    for index, name in ((93, "bonus-out"), (124, "skill-change-end"), (71, "title-cursor"), (62, "title-decide")):
        (override / f"skill_effects/audio/{name}.wav").write_bytes(archive.read_file(archive.filenames[index]))
    lane_preview(sound / "ver6/lanespeed.s3v", override / "selection_ui/audio/lanespeed.flac", ffmpeg)


def lane_preview(source, output, ffmpeg):
    # 16 beats at 130 BPM, padded to the original loop duration at 48 kHz.
    subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-y", "-f", "asf", "-i", source,
                    "-ac", "2", "-ar", "48000", "-af", "aresample=48000,apad=whole_len=354462,atrim=end_sample=354462",
                    output], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data", type=Path, help="Locally supplied 2026101300/LAZY_KFC/contents/data")
    parser.add_argument("common", type=Path, help="Common assets output directory")
    parser.add_argument("override", type=Path, help="Version-specific assets output directory")
    parser.add_argument("--part", nargs="+", choices=PARTS)
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg"), help="FFmpeg CLI for cabinet audio")
    parser.add_argument("--registration", type=Path, default=TOOLS.parents[2] / "local-inputs/stage-registration.json",
                        help="Full local stage registration JSON from extract-stage-registration.py")
    args = parser.parse_args()
    data, common, override = (path.resolve() for path in (args.data, args.common, args.override))
    if not (data / "graphics/ver07/title.ifs").is_file():
        parser.error("Expected NABLA graphics/ver07/title.ifs under the supplied data directory")
    if common == override or common.is_relative_to(data) or override.is_relative_to(data):
        parser.error("Use distinct output directories outside the original data tree")
    parts = args.part or PARTS
    ffmpeg = shutil.which(args.ffmpeg) if args.ffmpeg else None
    if "audio" in parts and not ffmpeg:
        parser.error("Audio export requires --ffmpeg or an FFmpeg executable on PATH")
    if "backgrounds" in parts and not args.registration.is_file():
        parser.error("Background export requires --registration with the full local stage registration JSON")
    if "scenes" in parts and not (override / "songs/library.json").is_file():
        parser.error("Special scene export requires songs/library.json in the override tree. Prepare songs first.")
    sys.path.insert(0, str(TOOLS))
    for root in (common, override):
        root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="nabla-graphics-") as temp:
        work = Path(temp)
        if "cabinet" in parts:
            cabinet(data, common, override)
        if "stage" in parts:
            stage(data, common, override)
        if "backgrounds" in parts or "scenes" in parts:
            with import_module("stage-ifs-index").StageIfsIndex(data, common, override, parts) as index:
                if "backgrounds" in parts:
                    backgrounds(data, common, index, args.registration)
                if "scenes" in parts:
                    scenes(data, override, index)
                index.write()
        if "audio" in parts:
            audio(data, common, override, ffmpeg, work)


if __name__ == "__main__":
    main()
