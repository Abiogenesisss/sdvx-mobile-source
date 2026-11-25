"""Pack original S3V audio, artwork and every installed difficulty for selected songs."""
import argparse
import json
from importlib import import_module
from pathlib import Path
import shutil
from xml.etree import ElementTree



CHART_TYPES = {
    1: ("novice", "NOV"), 2: ("advanced", "ADV"), 3: ("exhaust", "EXH"),
    5: ("maximum", "MXM"), 6: ("ultimate", "ULT"),
}
EXTRA_TYPES = {
    2: ("infinite", "INF"), 3: ("gravity", "GRV"), 4: ("heavenly", "HVN"),
    5: ("vivid", "VVD"), 6: ("exceed", "XCD"), 7: ("ultimate", "ULT"),
    8: ("nabla", "NBL"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("judgment", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("songs", nargs="+", help="Original song ID and optional existing Android directory as ID:directory")
    args = parser.parse_args()
    database = ElementTree.fromstring((args.game / "others/music_db.xml").read_bytes().decode("cp932"))
    judgment = json.loads(args.judgment.read_text(encoding="utf-8"))
    compile_chart = import_module("compile-vox").compile_chart
    library = []
    for spec in args.songs:
        song, _, alias = spec.partition(":")
        song_id = int(song)
        entry = database.find(f"music[@id='{song_id}']")
        info = entry.find("info")
        source, = (args.game / "music").glob(f"{song_id:04d}_*")
        folder = alias or str(song_id)
        output = args.output / folder
        output.mkdir(parents=True, exist_ok=True)
        song_data = {
            "id": song_id, "folder": folder, "title": info.findtext("title_name"),
            "artist": info.findtext("artist_name"), "version": int(info.findtext("version")),
            "bpm_min": int(info.findtext("bpm_min")), "bpm_max": int(info.findtext("bpm_max")),
            "background": int(info.findtext("bg_no")),
            "charts": [],
        }
        shared_music = source / f"{source.name}.s3v"
        if shared_music.is_file():
            shutil.copyfile(shared_music, output / "music.s3v")
            song_data["music"] = "music.s3v"
        for vox in sorted(source.glob("*.vox")):
            slot = int(vox.stem.rsplit("_", 1)[1][0])
            category, label = EXTRA_TYPES[int(info.findtext("inf_ver"))] if slot == 4 else CHART_TYPES[slot]
            # The database calls slot 4 "infinite" even for GRV/HVN/XCD.
            row = entry.find(f"difficulty/{'infinite' if slot == 4 else category}")
            level = int(row.findtext("difnum"))
            jacket = source / f"jk_{song_id:04d}_{slot}_b.png"
            if not jacket.is_file():
                jacket = source / f"jk_{song_id:04d}_1_b.png"
            name = f"{category}.json"
            artwork = f"{category}.png"
            metadata = {
                "song_id": song_id, "title": song_data["title"], "artist": song_data["artist"],
                "difficulty": category.upper(), "source_vox": vox.name,
                "level_tenths": level, "max_exscore": int(row.findtext("max_exscore")),
            }
            chart = compile_chart(vox.read_bytes(), judgment, metadata)
            (output / name).write_text(json.dumps(chart, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
            # NEXT AREA uses the original 676-pixel jacket at authored scale.
            shutil.copyfile(jacket,output/artwork)
            chart_entry = {
                "name": metadata["difficulty"], "label": label, "level": level,
                "chart": name, "jacket": artwork, "effect": row.findtext("effected_by", ""),
                "illustrator": row.findtext("illustrator", ""),
            }
            if "music" not in song_data:
                audio = f"{category}.s3v"
                shutil.copyfile(vox.with_suffix(".s3v"), output / audio)
                chart_entry["music"] = audio
            song_data["charts"].append(chart_entry)
        shutil.copyfile(source / f"{source.name}_pre.s3v", output / "preview.s3v")
        library.append(song_data)
        print(f"{song_data['title']}: {len(song_data['charts'])} original charts", flush=True)
    (args.output / "library.json").write_text(json.dumps(library, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")


if __name__ == "__main__":
    main()
