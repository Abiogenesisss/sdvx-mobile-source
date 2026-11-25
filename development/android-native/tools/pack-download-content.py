"""Build optional song and crew ZIPs from the original S3V and exported assets."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from importlib import import_module
import json
from pathlib import Path, PurePosixPath
import sys
from tempfile import TemporaryDirectory
from xml.etree import ElementTree
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--jobs", type=int, default=12)
    parser.add_argument("--songs", nargs="+", type=int)
    args = parser.parse_args()
    runtime = args.project / "development/android-native"
    builtin = runtime / "src/main/assets"
    reference = runtime / "assets-reference"
    external = args.project / "development/download-content"
    original = args.project / "2026101300/LAZY_KFC/contents/data"
    sys.path.insert(0, str(runtime / "tools"))
    compile_chart = import_module("compile-vox").compile_chart

    song_catalog = load(builtin / "songs/library.json")
    judgment = load(reference / "songs/666/advanced.json")
    database = ElementTree.fromstring((original / "others/music_db.xml").read_bytes().decode("cp932"))
    db_songs = {int(node.attrib["id"]): node for node in database}
    original_dirs = {int(path.name.split("_", 1)[0]): path for path in (original / "music").iterdir() if path.is_dir()}
    slots = {"NOV": "1", "ADV": "2", "EXH": "3", "MXM": "5"}
    missing = []
    plans = []
    bundled_charts = 0
    for song in song_catalog:
        song_id = song["id"]
        folder = song["folder"]
        rows = song["charts"]
        directory = original_dirs[song_id]
        vox = {path.stem.rsplit("_", 1)[1][0]: path for path in directory.glob("*.vox")}
        charts = []
        for row in rows:
            if row.get("bundled", True):
                bundled_charts += 1
                continue
            slot = "6" if row["label"] == "ULT" and "6" in vox else slots.get(row["label"], "4")
            if slot not in vox:
                missing.append((song_id, song["title"], row["label"], "VOX absent from original directory"))
                continue
            charts.append((row, slot, vox[slot]))
        if charts:
            plans.append((song, directory, charts, any(row.get("bundled", True) for row in rows)))

    if args.songs:
        plans = [plan for plan in plans if plan[0]["id"] in args.songs]
        if len(plans) != len(set(args.songs)):
            raise ValueError("Selected song IDs must all have downloadable audio and charts")
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"{len(plans):,} songs, {sum(len(plan[2]) for plan in plans):,} charts to export, "
          f"{bundled_charts} built-in charts skipped, {len(missing)} unavailable charts", flush=True)

    with TemporaryDirectory(prefix="song-work-", dir=args.output) as temp:
        work = Path(temp)

        def convert(plan):
            song, source, charts, shared_builtin = plan
            song_id = song["id"]
            folder = song["folder"]
            target = work / str(song_id)
            target.mkdir()
            assets = []
            for row, slot, vox in charts:
                name = row["chart"]
                cached = external / "songs" / folder / name
                if cached.is_file() and load(cached).get("song_id") == song_id:
                    assets.append((cached, f"songs/{folder}/{name}", False))
                    continue
                difficulty = "infinite" if slot == "4" else row["name"].lower()
                stats = db_songs[song_id].find(f"difficulty/{difficulty}")
                metadata = {
                    "song_id": song_id, "title": song["title"], "artist": song["artist"],
                    "difficulty": row["name"], "source_vox": vox.name, "level_tenths": row["level"],
                    "max_exscore": int(stats.findtext("max_exscore")),
                }
                chart_path = target / name
                chart_path.write_bytes(encoded(compile_chart(vox.read_bytes(), judgment, metadata)))
                assets.append((chart_path, f"songs/{folder}/{name}", True))

            if not shared_builtin:
                shared = source / f"{source.name}.s3v"
                if shared.is_file():
                    assets.append((shared, f"songs/{folder}/music.s3v", False))
                else:
                    for row, _, vox in charts:
                        audio = vox.with_suffix(".s3v")
                        if not audio.is_file():
                            raise FileNotFoundError(audio)
                        name = Path(row["chart"]).stem + ".s3v"
                        assets.append((audio, f"songs/{folder}/{name}", False))

            preview = source / f"{source.name}_pre.s3v"
            if preview.is_file() and not shared_builtin:
                assets.append((preview, f"songs/{folder}/preview.s3v", False))
            return assets, target

        total = Counter()
        errors = []
        archive_names = []
        batch_size = 500
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for start in range(0, len(plans), batch_size):
                batch = plans[start:start + batch_size]
                name = f"SDVX-s3v-songs-{start // batch_size + 1:02d}.zip"
                output = args.output / name
                partial = output.with_suffix(".zip.partial")
                if output.is_file():
                    with ZipFile(output) as archive:
                        for item in archive.infolist():
                            total[Path(item.filename).suffix] += 1
                    archive_names.append(name)
                    continue
                print(f"{name}: exporting {start+1:,}–{start+len(batch):,}/{len(plans):,}", flush=True)
                with ZipFile(partial, "w") as archive:
                    archive.writestr(".nomedia", b"", ZIP_STORED)
                    jobs = [pool.submit(convert, plan) for plan in batch]
                    for plan, job in zip(batch, jobs):
                        try:
                            assets, target = job.result()
                        except Exception as error:
                            errors.append((plan[0]["id"], plan[0]["title"], repr(error)))
                            print(f"Failed song {plan[0]['id']}: {error}", flush=True)
                            continue
                        for path, key, temporary in assets:
                            add_file(archive, path, key)
                            total[path.suffix] += 1
                            if temporary:
                                path.unlink()
                        target.rmdir()
                    count = len(archive.filelist)
                partial.replace(output)
                archive_names.append(name)
                print(f"{name}: {count:,} entries, {output.stat().st_size / 2**30:.2f} GiB", flush=True)

    if args.songs:
        if errors:
            sys.exit(1)
        return
    crew_name = "SDVX-extra-crew.zip"
    crew_output = args.output / crew_name
    if crew_output.is_file():
        with ZipFile(crew_output) as archive:
            crew_count = len(archive.infolist())
    else:
        crew_name, crew_count = build_crew(args.output, external, builtin, reference)
    report = {"songs": len(plans) - len(errors), "bundled_charts_skipped": bundled_charts,
              "missing_vox": missing, "song_errors": errors, "song_file_counts": dict(total),
              "song_archives": archive_names, "crew_archive": crew_name, "crew_files": crew_count}
    (args.output / "s3v-report.json").write_bytes(encoded(report))
    print(json.dumps(report, ensure_ascii=False), flush=True)
    if errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
