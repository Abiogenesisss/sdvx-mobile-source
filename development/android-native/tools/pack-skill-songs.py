"""Build the song and chart assets required by the bundled SKILL ANALYZER courses."""
import argparse
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from importlib import import_module
import json
from pathlib import Path
import shutil
from xml.etree import ElementTree


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("game", "catalog", "courses", "judgment", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    catalog = json.loads(args.catalog.read_bytes())
    songs = {song["id"]: song for song in catalog}
    course_sets = json.loads(args.courses.read_bytes())
    tracks = defaultdict(set)
    for course_set in course_sets:
        for course in course_set["courses"]:
            for track in course["tracks"]:
                tracks[track["mid"]].add(track["mty"])
    database = ElementTree.fromstring((args.game / "others/music_db.xml").read_bytes().decode("cp932"))
    judgment = json.loads(args.judgment.read_bytes())
    compile_chart = import_module("compile-vox").compile_chart

    def pack(entry):
        song_id, types = entry
        song = songs[song_id]
        source, = (args.game / "music").glob(f"{song_id:04d}_*")
        target = args.output / song["folder"]
        target.mkdir(parents=True, exist_ok=True)
        already_bundled = any(row.get("bundled", True) for row in song["charts"])
        shared_music = source / f"{source.name}.s3v"
        for kind in sorted(types):
            chart, = (row for index, row in enumerate(song["charts"])
                      if (index == kind if kind < 3 else index == 3 and row["label"] != "MXM"
                          if kind == 3 else row["label"] == "MXM"))
            if chart.get("bundled", True):
                continue
            slot = kind + 1
            vox, = source.glob(f"*_{slot}*.vox")
            difficulty = chart["name"].lower()
            original_slot = "infinite" if kind == 3 else difficulty
            stats = database.find(f"music[@id='{song_id}']/difficulty/{original_slot}")
            metadata = {
                "song_id": song_id, "title": song["title"], "artist": song["artist"],
                "difficulty": chart["name"], "source_vox": vox.name,
                "level_tenths": chart["level"], "max_exscore": int(stats.findtext("max_exscore")),
            }
            data = compile_chart(vox.read_bytes(), judgment, metadata)
            (target / chart["chart"]).write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":")),
                                                encoding="utf-8")
            if not shared_music.is_file():
                audio = Path(chart["chart"]).with_suffix(".s3v").name
                shutil.copyfile(vox.with_suffix(".s3v"), target / audio)
                chart["music"] = audio
            chart["bundled"] = True
        if not already_bundled:
            if shared_music.is_file():
                shutil.copyfile(shared_music, target / "music.s3v")
                song["music"] = "music.s3v"
            shutil.copyfile(source / f"{source.name}_pre.s3v", target / "preview.s3v")
        return song_id

    with ThreadPoolExecutor(max_workers=4) as pool:
        for song_id in pool.map(pack, sorted(tracks.items())):
            print(f"Bundled course song {song_id}", flush=True)
    (args.output / "library.json").write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")),
                                              encoding="utf-8")


if __name__ == "__main__":
    main()
