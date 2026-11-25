"""Build locally installable music and crew ZIPs for a particular Android APK."""
import argparse
from importlib import import_module
import json
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
from zipfile import Path as ZipPath, ZipFile

from hdd_inputs import locate_contents


ROOT = Path(__file__).resolve().parents[1]
NATIVE_TOOLS = ROOT / "development/android-native/tools"


def write_index(output, apk, category, names):
    archives = []
    for name in names:
        path = output / name
        item = {"file": name, "bytes": path.stat().st_size}
        with ZipFile(path) as archive:
            if ".nomedia" not in archive.namelist() or archive.getinfo(".nomedia").file_size != 0:
                raise ValueError(f"{name} needs an empty .nomedia file at the ZIP root")
            if category == "music":
                folders = {key.split("/", 2)[1] for key in archive.namelist() if key.startswith("music/")}
                item["song_ids"] = sorted(int(folder.partition("_")[0]) for folder in folders)
            else:
                catalog = json.loads(archive.read("crew/catalog.json"))
                item["crew_styles"] = [[row["id"], row["style"]] for row in catalog["crews"]]
        archives.append(item)
    index = {"format": "sdvx-mobile-incremental/1", "category": category,
             "target_apk": apk.name, "archives": archives}
    (output / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hdd", required=True, type=Path, help="Local HDD, contents or data directory")
    parser.add_argument("--apk", required=True, type=Path, help="The installed APK to supplement")
    parser.add_argument("--output", required=True, type=Path, help="Release directory for music/ and crew/ packages")
    parser.add_argument("--part", choices=("music", "crew"), help="Build only this group (default: both)")
    parser.add_argument("--songs", nargs="+", type=int, help="Build only the listed additional song IDs")
    args = parser.parse_args()
    if args.songs and args.part == "crew":
        parser.error("--songs is only applicable to music")
    parts = [args.part] if args.part else (["music"] if args.songs else ["music", "crew"])
    contents = locate_contents(args.hdd)
    apk = args.apk.resolve(strict=True)
    output = args.output.resolve()
    if output.is_relative_to(contents):
        parser.error("Use an output directory outside the HDD contents")
    output.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(NATIVE_TOOLS))
    packer = import_module("pack-download-content")
    for category in parts:
        dest = output / category
        dest.mkdir(exist_ok=True)
        # Finish all ZIPs before replacing deliverables, with the index last.
        with TemporaryDirectory(prefix=f".{category}-", dir=output) as scratch:
            stage = Path(scratch)
            if category == "music":
                requested = set(args.songs or ())
                dirs, found = packer.incremental_songs(contents / "data/music", apk, requested)
                if missing := requested - found:
                    print("No playable addition for IDs:", *sorted(missing), flush=True)
                names, _ = packer.build_songs(stage, dirs, False)
            else:
                from imageio_ffmpeg import get_ffmpeg_exe

                with TemporaryDirectory(prefix="sdvx-crew-") as tmp:
                    work = Path(tmp)
                    registry = work / "registry.json"
                    crew = work / "crew"
                    for argv in (
                        (NATIVE_TOOLS / "extract-crew-registry.py", contents / "modules/soundvoltex.dll", registry),
                        (NATIVE_TOOLS / "prepare-crew-library.py", contents / "data", get_ffmpeg_exe(), crew),
                        (NATIVE_TOOLS / "compile-crew-catalog.py", contents / "data", registry, work / "extracted", crew),
                    ):
                        subprocess.run([sys.executable, *map(str, argv)], check=True)
                    with ZipFile(apk) as base:
                        name, _ = packer.build_crew(stage, work, ZipPath(base, "assets/"))
                names = [name]
            write_index(stage, apk, category, names)
            for name in [*names, "index.json"]:
                (stage / name).replace(dest / name)
            print(f"Ready: {dest / 'index.json'}", flush=True)
    shutil.copyfile(ROOT / "docs/INCREMENTAL.md", output / "INCREMENTAL.md")


if __name__ == "__main__":
    main()
