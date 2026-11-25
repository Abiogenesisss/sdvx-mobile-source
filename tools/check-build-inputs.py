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

required = ["font/0000.ttc", "crew/grace/model/grace_ver7.moc3", "crew/rasis/model/rasis_ver7.moc3",
            "crew/shaders/FragShaderSrc.frag", "title_original/afp-library.json", "stage_bg/catalog.json"]
if catalog.is_file() and catalog.stat().st_size:
    songs = json.loads(catalog.read_text(encoding="utf-8"))
    bundled = next(((song, chart) for song in songs for chart in song["charts"] if chart.get("bundled")), None)
    if bundled:
        song, chart = bundled
        required.extend(f"songs/{song['folder']}/{chart[key]}" for key in ("chart", "jacket"))
    else:
        missing.append("songs/library.json: no bundled chart")
for name in required:
    path = assets / name
    if not path.exists():
        path = reference / name
    if not path.is_file() or not path.stat().st_size:
        missing.append(name)
if missing:
    print("Missing local build inputs (development/android-native or merged assets):")
    print("\n".join("  " + item for item in missing))
    raise SystemExit(1)
print("Key local inputs found, including one bundled chart. No APK was built.")
