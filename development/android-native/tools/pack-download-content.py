"""Pack local song directories in HDD layout and optionally package exported crew assets."""
import argparse
from collections import Counter
import json
from pathlib import Path, PurePosixPath
import shutil
import sys
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


def load(path):
    return json.loads(path.read_bytes())


def encoded(data):
    return json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode()


def add_file(archive, path, name):
    compression = ZIP_DEFLATED if path.suffix in {".json", ".dat"} else ZIP_STORED
    archive.write(path, name, compress_type=compression, compresslevel=3 if compression == ZIP_DEFLATED else None)


def build_crew(destination, external, builtin, reference=None):
    relative = Path("crew")
    source = external / relative
    base_catalog = load(builtin / "crew/catalog.json")
    catalog = load(source / "catalog.json")
    locked = {(item["id"], item["style"]) for item in base_catalog["crews"] if not item.get("bundled", True)}
    rows = [item for item in catalog["crews"] if (item["id"], item["style"]) in locked
            and (item["actors"] or item.get("room_afp") or item.get("portrait"))]
    used_actors = sorted({i for item in rows for i in item["actors"]})
    offsets = {old: new for new, old in enumerate(used_actors)}
    for item in rows:
        item["actors"] = [offsets[i] for i in item["actors"]]
    actors = [catalog["actors"][i] for i in used_actors]
    files = {item[field] for item in rows for field in ("icon", "name_image", "preview", "portrait")
             if item.get(field)}
    # The runtime checks the portrait in external storage before enabling the
    # crew, even when the selector preview already exists in the APK.
    required_external = {item["portrait"] for item in rows if not item["actors"]
                         and not item.get("room_afp") and item.get("portrait")}
    for actor in actors:
        model = actor["model"]
        required_external.add(model)
        path = external / model
        refs = json.loads(path.read_text(encoding="utf-8-sig"))["FileReferences"]
        folder = PurePosixPath(model).parent
        files.add(model)
        files.update((folder / name).as_posix() for name in (refs["Moc"], *refs["Textures"]))
        if refs.get("Physics"):
            files.add((folder / refs["Physics"]).as_posix())
        for motions in refs["Motions"].values():
            files.add((folder / motions[0]["File"]).as_posix())
            files.update((folder / motion["File"]).as_posix() for motion in motions[1:]
                         if (path.parent / motion["File"]).is_file())
        for name in [refs.get(key) for key in ("Pose", "DisplayInfo", "UserData")] + [
                expression["File"] for expression in refs.get("Expressions", ())]:
            if name and (path.parent / name).is_file():
                files.add((folder / name).as_posix())
    for folder in {item[field] for item in rows for field in ("room_afp", "matching_afp") if item.get(field)}:
        path = external / folder
        manifest = next((file for file in ("afp-library.json", "timeline.dat") if (path / file).is_file()), None)
        if manifest is None:
            raise FileNotFoundError(f"Missing crew animation manifest: {path}")
        files.update(file.relative_to(external).as_posix() for file in path.rglob("*") if file.is_file())
        if any(not item["actors"] and item.get("room_afp") == folder for item in rows):
            required_external.add(f"{folder}/{manifest}")

    old_voices = load(builtin / "crew/voice/index.json")
    new_voices = load(source / "voice/index.json")
    used_banks = {item["voice"] for item in rows if item["voice"]}
    if missing_banks := used_banks - old_voices.keys() - new_voices.keys():
        raise ValueError(f"Missing crew voice banks: {', '.join(sorted(missing_banks))}")
    new_voices = {bank: {slot: data for slot, data in slots.items() if slot not in old_voices.get(bank, {})}
                  for bank, slots in new_voices.items() if bank in used_banks}
    new_voices = {bank: slots for bank, slots in new_voices.items() if slots}
    files.update(f"crew/voice/{data[0]}" for slots in new_voices.values() for data in slots.values())
    full_catalog = {"actors": actors, "crews": rows}
    replacements = {
        "crew/catalog.json": full_catalog,
        "crew/voice/index.json": new_voices,
    }

    name = "SDVX-extra-crew.zip"
    output = destination / name
    partial = output.with_suffix(".zip.partial")
    try:
        with ZipFile(partial, "w") as archive:
            archive.writestr(".nomedia", b"", ZIP_STORED)
            for key, value in replacements.items():
                archive.writestr(key, encoded(value), ZIP_DEFLATED, 3)
            for key in sorted(files):
                packaged = builtin / key
                if not packaged.is_file() and reference is not None:
                    packaged = reference / key
                in_apk = packaged.is_file()
                if in_apk and key not in required_external:
                    continue
                path = external / key
                if path.is_file():
                    add_file(archive, path, key)
                elif in_apk:
                    archive.writestr(key, packaged.read_bytes(), ZIP_STORED)
                else:
                    raise FileNotFoundError(f"Missing crew dependency: {path}")
            count = len(archive.filelist)
        partial.replace(output)
    finally:
        partial.unlink(missing_ok=True)
    print(f"{name}: {len(rows)} crew styles, {count:,} entries, {output.stat().st_size / 2**30:.2f} GiB", flush=True)
    return name, count


def reuse_crew_archive(path):
    with ZipFile(path) as archive:
        if ".nomedia" in archive.namelist():
            return len(archive.filelist)
    # Earlier crew ZIPs omitted the marker. Preserve the existing compressed entries.
    with TemporaryDirectory(prefix=".crew-", dir=path.parent) as scratch:
        partial = Path(scratch) / path.name
        shutil.copyfile(path, partial)
        with ZipFile(partial, "a") as archive:
            archive.writestr(".nomedia", b"", ZIP_STORED)
            count = len(archive.filelist)
        partial.replace(path)
    return count


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


def incremental_songs(music, baseline, requested):
    if baseline.suffix.lower() == ".apk":
        with ZipFile(baseline) as apk:
            catalog = json.loads(apk.read("assets/songs/library.json"))
    else:
        catalog = load(baseline)
    found = set()
    directories = []
    for song in catalog:
        song_id = song["id"]
        if requested and song_id not in requested:
            continue
        folder = song["folder"]
        prefix, separator, _ = folder.partition("_")
        if (Path(folder).name != folder or not separator or not prefix.isascii() or
                not prefix.isdecimal() or int(prefix) != song_id):
            raise ValueError(f"Invalid baseline song folder for ID {song_id}: {folder!r}")
        directory = music / folder
        charts = song["charts"]
        # The April Fools chart is generated from separate assets, not a HDD music folder.
        if song_id == 1438 or all(row["bundled"] for row in charts) or not directory.is_dir():
            continue
        cover = f"jk_{prefix}_"
        shared = f"{cover}1"
        for row in charts:
            if row["bundled"] or not (directory / row["chart"]).is_file():
                continue
            arrangement = row.get("music", Path(row["chart"]).with_suffix(".s3v"))
            if not (directory / arrangement).is_file() and (
                "music" in row or not (directory / song["music"]).is_file()
            ):
                continue
            slot = f"{cover}{row['slot']}"
            jackets = (row["jacket"], f"{slot}.png", f"{shared}_b.png",
                       f"{shared}.png", f"{slot}_s.png", f"{shared}_s.png")
            if any((directory / jacket).is_file() for jacket in jackets):
                directories.append(directory)
                found.add(song_id)
                break
    print(f"Found {len(directories):,} incremental song directories", flush=True)
    return sorted(directories), found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--hdd", type=Path, help="Local HDD root, contents, or data directory")
    parser.add_argument("--songs", nargs="+", type=int, help="Export only these song IDs and skip the crew pack")
    parser.add_argument("--song-catalog", type=Path, metavar="APK_OR_JSON",
                        help="Export only playable song additions to this APK or library.json")
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
    if args.song_catalog:
        directories, found = incremental_songs(music, args.song_catalog, requested)
    else:
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
        reason = "without a playable addition in the baseline catalog" if args.song_catalog else "without a local VOX directory"
        print(f"Skipped song IDs {reason}: {sorted(missing)}", flush=True)

    output.mkdir(parents=True, exist_ok=True)
    names, totals = build_songs(output, directories, args.directory)
    if args.song_catalog or args.songs or args.directory:
        return

    runtime = args.project / "development/android-native"
    builtin = runtime / "src/main/assets"
    reference = runtime / "assets-reference"
    external = args.project / "development/download-content"
    crew_name, crew_count = None, 0
    crew_output = output / "SDVX-extra-crew.zip"
    if crew_output.is_file():
        crew_name = crew_output.name
        crew_count = reuse_crew_archive(crew_output)
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
