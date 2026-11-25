"""Check representative private build inputs without compiling or copying them."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("root", type=Path, nargs="?", default=Path(__file__).resolve().parents[1])
app = parser.parse_args().root / "development/android-native"
assets = app / "src/main/assets"
reference = app / "assets-reference"
catalog = assets / "songs/library.json"
missing = []
if not reference.is_dir():
    missing.append("assets-reference/")
for path in (catalog, app / "src/main/cpp/stage_registration_2026101300.inc",
             app / "src/main/cpp/entry_notice_2026101300.inc",
             *(app / "src/main/res/drawable-xxxhdpi" / name for name in
               ("ic_launcher_background.png", "ic_launcher_foreground.png"))):
    if not path.is_file() or not path.stat().st_size:
        missing.append(str(path.relative_to(app)))

required = ["songs/judgment.json", "font/0000.ttc", "crew/grace/model/grace_ver7.moc3", "crew/rasis/model/rasis_ver7.moc3",
            "crew/shaders/FragShaderSrc.frag", "title_original/afp-library.json", "stage_bg/catalog.json",
            "crew/catalog.json","crew/voice/index.json","game_nemsys/catalog.json","skill_selector/courses.json",
            "songs/details.json","songs/selection.json","stage/sound/general_sampler.json","subscreen/achievements.json",
            "subscreen/catalog.json","result/bg_grid_result.png","crew_selector/afp-library.json",
            "stage/touch/note.png","stage/touch/fx.png","startup/warning_j.png"]
if catalog.is_file() and catalog.stat().st_size:
    songs = json.loads(catalog.read_text(encoding="utf-8"))
    count = 0
    for song in songs:
        folder = "music/" + song["folder"] + "/"
        for chart in song["charts"]:
            if not chart.get("bundled"):
                continue
            count += 1
            slot = str(chart["slot"])
            prefix = "jk_" + song["folder"].split("_")[0] + "_"
            jackets = [chart["jacket"], prefix+slot+".png", prefix+"1_b.png", prefix+"1.png",prefix+slot+"_s.png",prefix+"1_s.png"]
            if not any((assets / folder / name).is_file() for name in jackets):
                missing.append(folder + chart["jacket"])
            if song["id"] == 1438:
                required.append("songs/1438/arrangement.json")
            else:
                required.extend((folder + chart["chart"],folder + chart.get("music",song["music"])))
    if not any(song["charts"] for song in songs):
        missing.append("songs/library.json: empty selection catalog")
    elif not count:
        print("No playable chart from the preferred bundle is present; the available catalog is retained.")
    print(f"Catalog: {len(songs)} songs, {count} bundled charts. Song count is not a version check.")
required.append("graphics/stage-ifs.json")
ifs_index = reference / "graphics/stage-ifs.json"
if ifs_index.is_file():
    archive_ends = {}
    for entry in json.loads(ifs_index.read_bytes())["entries"].values():
        name = entry["archive"]
        archive_ends[name] = max(archive_ends.get(name, 0), entry["offset"] + entry["size"])
    for name, end in archive_ends.items():
        file = reference / name
        if not file.is_file() or file.stat().st_size < end:
            missing.append(name)
    print(f"Backgrounds: {len(archive_ends)} IFS archives referenced by the index.")
for name in sorted(set(required)):
    path = assets / name
    if not path.exists():
        path = reference / name
    if not path.is_file() or not path.stat().st_size:
        missing.append(name)
if missing:
    print("Missing local build inputs (development/android-native or merged assets):")
    print("\n".join("  " + item for item in missing))
    raise SystemExit(1)
print("Required catalogs, installed-song jackets and original VOX/audio inputs found.")
