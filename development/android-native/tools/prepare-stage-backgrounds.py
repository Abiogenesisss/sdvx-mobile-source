"""Prepare original AFP and side-tunnel resources for NABLA's common backgrounds."""
import argparse
from importlib import import_module
import json
from pathlib import Path
import shutil

# soundvoltex.dll 180485ea0 maps bg_no to resource IDs. 1805b742e and
# subsequent registrations map those IDs to the original IFS archives.
BACKGROUNDS = {
    0: ("0101_bridge", ()),
    2: ("0102_dome", (1, 5)),
    3: ("0103_space", (2, 6)),
    4: ("0104_pop", (3, 7)),
    8: ("0105_moon", (4,)),
    10: ("0106_domered", (9,)),
    12: ("0107_moon2", (11, 13)),
    15: ("0108_moon3", (14, 16)),
    18: ("0109_kac01", (17, 19)),
    21: ("0110_blaster", (20, 22)),
    23: ("0111_maxma", (24, 25)),
    27: ("0112_iseki", (26, 28)),
    30: ("0301_star", (29, 31)),
    32: ("2015_kac01", (33, 34)),
    36: ("0302_kac02", (35, 37)),
    38: ("2015_summer", ()),
    39: ("303boss", ()),
    40: ("nishinippori", ()),
    41: ("beach", ()),
    42: ("tenkaichi", ()),
    44: ("tenkaichi02", ()),
    45: ("edp2016", ()),
    49: ("kac5th_rasis", ()),
    50: ("jomanda", ()),
    51: ("0401_sea", ()),
    52: ("0402_sky", ()),
    58: ("0403_tunnel", (55, 56, 57)),
    60: ("0404_beam", (59,)),
    61: ("0405_sakura", ()),
    62: ("0403_city", ()),
    63: ("0408_pop2", ()),
    64: ("0406_twilight", ()),
    65: ("omega_arashi", ()),
    70: ("tenkaichi03", ()),
    71: ("omega_cyberspace", ()),
    74: ("omega18_maxma", ()),
    75: ("0501_stage", ()),
    76: ("automation_01", ()),
    79: ("omega_shinwa", ()),
    81: ("omega_capsaicin", ()),
    82: ("diver_basic01", ()),
    86: ("beach_night", ()),
    88: ("exg_1", ()),
    90: ("diver_basic02", ()),
    91: ("diver_basic02", ()),
    94: ("exg_2", ()),
    96: ("undertale", ()),
    100: ("bpl_nomal", ()),
    101: ("japan_01", ()),
    104: ("diver_basic03", ()),
    107: ("bpls3_nomal", ()),
    110: ("planet", ()),
    112: ("exgate", ()),
    114: ("diver_basic04", ()),
    117: ("bpls5_nomal", ()),
    122: ("vn2026_ehon", ()),
}

# Texture family, angular offset, scroll speed and subdivision count in
# 180485ea0. The renderer loads the normal/clear pair separately from the AFP.
TUNNEL_IDS = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 52, 55, 56, 57, 58, 59, 60, 63, 119, 121)


def gauge_labels(swf):
    labels = swf.labels
    normal = "normal" if "normal" in labels else "nomal"
    initial = next((name for name in ("normal_loop", "loop_normal") if name in labels), normal)
    if normal not in labels or "clear" not in labels:
        raise ValueError(f"{swf.exported_name} is missing original gauge labels")
    return {"initial": initial, "normal": normal, "clear": "clear"}


def prepare_backgrounds(archive_path, output, game_textures, registration, ids, publish=None):
    registrations = json.loads(registration.read_text(encoding="utf-8"))["ordinary"]
    tunnels = {}
    for number in TUNNEL_IDS:
        entry = registrations[str(number)]["tunnels"][0]
        tunnels[number] = [entry[key] for key in ("name", "angle", "density", "preset")]
    output.mkdir(parents=True, exist_ok=True)
    shared = archive_path("diver_tex") / "tex"
    palettes = {90: "blue", 91: "blue", 104: "white", 114: "purple"}
    prepare = import_module("prepare-original-afp").prepare
    read_movie = import_module("compile-stage-background").BackgroundSWF
    for number in ids:
        archive = BACKGROUNDS[number][0]
        source = archive_path(archive)
        afp = source / "afp"
        movie = "ap_bg_all" if number == 76 else "bg_all"
        swf = read_movie(movie, (afp / movie).read_bytes(), (afp / "bsi" / movie).read_bytes())
        swf.parse()
        texture_dirs = [shared]
        if number in palettes:
            texture_dirs.append(archive_path(f"diver_tex_{palettes[number]}") / "tex")
        if number == 58:
            texture_dirs.append(archive_path("0103_space") / "tex")
        if number == 76:
            texture_dirs.append(archive_path("0501_stage") / "tex")
        if number == 122:
            texture_dirs.extend(archive_path(name) / "tex" for name in ("vn2026", "vn26_final"))
        gauge = gauge_labels(swf) if "clear" in swf.labels else {}
        scene = {"movies": [{"slot": 0, "name": movie, "label": gauge.get("initial", "in")}], "events": {}}
        if number == 60:
            # Backgrounds 59 and 60 register the unlabelled beam movie at priority 62.
            scene["movies"].append({"slot": 1, "name": "beam_bg", "label": 0, "priority": 62})
        if number in (65, 79, 91):
            # 18048fec9, 180492777 and 1804946dc register bg_top at priority 9.
            scene["movies"].append({"slot": 1, "name": "bg_top", "label": 0})
        if gauge:
            scene["gauge"] = gauge
        prepare(source, output / str(number), scene, texture_dirs, publish=publish)
    aliases = {str(number): number for number in BACKGROUNDS}
    aliases.update({str(alias): number for number, (_, aliases) in BACKGROUNDS.items() for alias in aliases})
    textures = output / "tunnel"
    textures.mkdir(exist_ok=True)
    for family in sorted({row[0] for row in tunnels.values()}):
        for state in ("no", "cl"):
            name = f"tex_{family}_{state}.png"
            shutil.copyfile(game_textures / name, textures / name)
    catalog = {"backgrounds": aliases, "tunnels": tunnels}
    (output / "catalog.json").write_text(json.dumps(catalog, separators=(",", ":")), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Directory containing extracted game_bg IFS archives")
    parser.add_argument("output", type=Path)
    parser.add_argument("--source-extra", type=Path, action="append", default=[], help="Additional extracted IFS directory")
    parser.add_argument("--game-textures", type=Path, required=True, help="Original graphics/game_tex directory")
    parser.add_argument("--ids", type=int, nargs="+", choices=BACKGROUNDS, default=list(BACKGROUNDS))
    parser.add_argument("--registration", type=Path, required=True, help="Full JSON from extract-stage-registration.py")
    args = parser.parse_args()
    def archive_path(name):
        for source in (args.source, *args.source_extra):
            for suffix in (".ifs", "_ifs"):
                path = source / f"gmbg_{name}{suffix}"
                if path.is_dir():
                    return path
        raise FileNotFoundError(f"Missing extracted game_bg/gmbg_{name}.ifs")

    prepare_backgrounds(archive_path, args.output, args.game_textures, args.registration, args.ids)


if __name__ == "__main__":
    main()
