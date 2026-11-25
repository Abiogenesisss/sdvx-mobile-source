"""Pack local song directories in HDD layout and optionally package exported crew assets."""
import argparse
from collections import Counter
import json
from pathlib import Path, PurePosixPath
import shutil
import sys
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


def load(path):
    return json.loads(path.read_bytes())


def encoded(data):
    return json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode()


def add_file(archive, path, name):
    compression = ZIP_DEFLATED if path.suffix in {".json", ".dat"} else ZIP_STORED
    archive.write(path, name, compress_type=compression, compresslevel=3 if compression == ZIP_DEFLATED else None)


def build_crew(destination, external, builtin, reference):
    relative = Path("crew")
    source = external / relative
    base_catalog = load(builtin / relative / "catalog.json")
    catalog = load(source / "catalog.json")
    included = {(item["id"], item["style"]) for item in base_catalog["crews"] if item.get("bundled", True)}
    # Icon-only entries have no navigator presentation. Silent portraits are
    # valid crews and must not require an animation or a voice bank.
    rows = [item for item in catalog["crews"] if (item["id"], item["style"]) not in included
            and (item["actors"] or item.get("room_afp") or item.get("portrait"))]
    used_actors = sorted({i for item in rows for i in item["actors"]})
    offsets = {old: new for new, old in enumerate(used_actors)}
    for item in rows:
        item["actors"] = [offsets[i] for i in item["actors"]]
    actors = [catalog["actors"][i] for i in used_actors]
    own_model_dirs = {PurePosixPath(actor["model"]).parent for actor in actors}
    needed_art = {item[field] for item in rows for field in ("icon", "name_image", "preview", "portrait")
                  if item.get(field)}
    # The runtime checks the portrait in external storage before enabling the
    # crew, even when the selector preview already exists in the APK.
    external_portraits = {item["portrait"] for item in rows if not item["actors"]
                         and not item.get("room_afp") and item.get("portrait")}

    old_voices = load(builtin / relative / "voice/index.json")
    new_voices = load(source / "voice/index.json")
    used_banks = {item["voice"] for item in rows}
    new_voices = {bank: {slot: data for slot, data in slots.items() if slot not in old_voices.get(bank, {})}
                  for bank, slots in new_voices.items() if bank in used_banks}
    new_voices = {bank: slots for bank, slots in new_voices.items() if slots}
    needed_voice_files = {f"crew/voice/{data[0]}" for slots in new_voices.values() for data in slots.values()}
    full_catalog = {"actors": actors, "crews": rows}
    replacements = {
        "crew/catalog.json": full_catalog,
        "crew/voice/index.json": new_voices,
    }

    name = "SDVX-extra-crew.zip"
    output = destination / name
    partial = output.with_suffix(".zip.partial")
    with ZipFile(partial, "w") as archive:
        archive.writestr(".nomedia", b"", ZIP_STORED)
        for key, value in replacements.items():
            archive.writestr(key, encoded(value), ZIP_DEFLATED, 3)
        for key in sorted(external_portraits):
            if not (external / key).is_file():
                path = builtin / key if (builtin / key).is_file() else reference / key
                add_file(archive, path, key)
        for path in sorted(source.rglob("*")):
            if not path.is_file():
                continue
            key = path.relative_to(external).as_posix()
            if key in replacements:
                continue
            if key not in external_portraits and ((builtin / key).is_file() or (reference / key).is_file()):
                continue
            if key.startswith("crew/voice/") and key not in needed_voice_files:
                continue
            if key.startswith("crew/art/") and key not in needed_art:
                continue
            if key.startswith("crew/live2d/") and not any(directory in PurePosixPath(key).parents for directory in own_model_dirs):
                continue
            add_file(archive, path, key)
        count = len(archive.filelist)
    partial.replace(output)
    print(f"{name}: {len(rows)} crew styles, {count:,} entries, {output.stat().st_size / 2**30:.2f} GiB", flush=True)
    return name, count


def build_songs(output, directories, unpacked):
    if unpacked:
        (output / ".nomedia").touch()
        music = output / "music"
        music.mkdir(exist_ok=True)
        for directory in directories:
            shutil.copytree(directory, music / directory.name, dirs_exist_ok=True)
        print(f"Copied {len(directories):,} song directories to {music}", flush=True)
        return [], Counter()

    total = Counter()
    names = []
    batch_size = 500
    for start in range(0, len(directories), batch_size):
        batch = directories[start:start + batch_size]
        name = f"SDVX-s3v-songs-{start // batch_size + 1:02d}.zip"
        destination = output / name
        partial = destination.with_suffix(".zip.partial")
        print(f"{name}: packing {start+1:,}–{start+len(batch):,}/{len(directories):,}", flush=True)
        try:
            with ZipFile(partial, "w") as archive:
                archive.writestr(".nomedia", b"", ZIP_STORED)
                for directory in batch:
                    for file in sorted(directory.rglob("*")):
                        if file.is_file():
                            key = PurePosixPath("music", directory.name, *file.relative_to(directory).parts)
                            add_file(archive, file, key.as_posix())
                            total[file.suffix] += 1
                count = len(archive.filelist)
            partial.replace(destination)
        finally:
            partial.unlink(missing_ok=True)
        names.append(name)
        print(f"{name}: {count:,} entries, {destination.stat().st_size / 2**30:.2f} GiB", flush=True)
    return names, total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--hdd", type=Path, help="Local HDD root, contents, or data directory")
    parser.add_argument("--songs", nargs="+", type=int, help="Export only these song IDs and skip the crew pack")
    parser.add_argument("--directory", action="store_true", help="Copy songs to an unpacked music directory")
    args = parser.parse_args()
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
    from hdd_inputs import locate_contents

    contents = locate_contents(args.hdd or args.project / "2026101300")
    music = contents / "data/music"
    output = args.output.resolve()
    original_music = music.resolve()
    if output.is_relative_to(original_music) or (args.directory and (output / "music").resolve() == original_music):
        parser.error("Choose an output directory outside the original HDD music directory.")
    requested = set(args.songs or ())
    found = set()
    directories = []
    for directory in sorted(music.iterdir()):
        prefix, sep, _ = directory.name.partition("_")
        if not sep or not prefix.isascii() or not prefix.isdecimal() or not directory.is_dir():
            continue
        song_id = int(prefix)
        if requested and song_id not in requested:
            continue
        if any(directory.glob("*.vox")):
            directories.append(directory)
            found.add(song_id)
    if missing := requested - found:
        print(f"Skipped song IDs without a local VOX directory: {sorted(missing)}", flush=True)

    output.mkdir(parents=True, exist_ok=True)
    names, totals = build_songs(output, directories, args.directory)
    if args.songs or args.directory:
        return

    runtime = args.project / "development/android-native"
    builtin = runtime / "src/main/assets"
    reference = runtime / "assets-reference"
    external = args.project / "development/download-content"
    crew_name, crew_count = None, 0
    crew_output = output / "SDVX-extra-crew.zip"
    if crew_output.is_file():
        crew_name = crew_output.name
        with ZipFile(crew_output) as archive:
            crew_count = len(archive.infolist())
    elif all((root / file).is_file() for root in (external, builtin)
             for file in ("crew/catalog.json", "crew/voice/index.json")):
        crew_name, crew_count = build_crew(output, external, builtin, reference)
    else:
        print("Skipped crew ZIP since the local crew inputs are unavailable.", flush=True)

    report = {"songs": len(directories), "song_file_counts": dict(totals), "song_archives": names,
              "crew_archive": crew_name, "crew_files": crew_count}
    (output / "s3v-report.json").write_bytes(encoded(report))
    print(json.dumps(report, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
