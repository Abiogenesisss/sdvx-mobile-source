"""Export the 2019 April Fool phrase table, charts and source audio for song 1438."""
import argparse
from importlib import import_module
import json
from pathlib import Path
import shutil
from xml.etree import ElementTree


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("assets", type=Path)
    args = parser.parse_args()
    waves = args.game / "sound/automa/waves"
    db = ElementTree.fromstring((waves / "wave_info_2019apr.xml").read_bytes().decode("cp932"))
    # Reuse the judgment and offset tables used by the other special songs.
    fixture = json.loads((Path(__file__).resolve().parents[1] / "assets-reference/songs/840/gravity.json")
                         .read_text(encoding="utf-8"))
    compile_chart = import_module("compile-vox").compile_chart
    output = args.assets / "songs/1438"
    output.mkdir(parents=True, exist_ok=True)
    original, = (args.game / "music").glob("1438_*")
    shutil.copyfile(original / "jk_1438_1.png", output / "jacket-1.png")
    tracks = []
    for entry in db.findall("mdb/music"):
        name, path = entry.findtext("name"), entry.findtext("meta/path")
        song_id = int(entry.findtext("meta/music_id"))
        source = waves / path
        track = dict(id=song_id, phrases=[])
        tracks.append(track)
        destination = output / str(song_id)
        destination.mkdir(exist_ok=True)
        if song_id < 99001:
            original, = (args.game / "music").glob(f"{song_id:04d}_*")
            shutil.copyfile(original / f"jk_{song_id:04d}_1.png", destination / "jacket.png")
        for row in db.findall(f"phrases/{name}/phrase"):
            get = row.findtext
            track["phrases"].append(dict(name=get("meta/name"), wave=int(get("meta/wave")),
                start=float(get("meta/start")), length=float(get("meta/length")),
                pre=float(get("meta/preMargin")), post=float(get("meta/postMargin")),
                previous=get("link/previous"), next=get("link/next"), part=get("part/type"),
                attributes=int(get("attributes/value"))))
        for wave in sorted({row["wave"] for row in track["phrases"]}):
            prefix = source / f"{path}_{wave}"
            shutil.copyfile(prefix.with_suffix(".s3v"), destination / f"{wave}.s3v")
            if song_id == 99002:
                continue
            for index, slot in enumerate(("1n", "2a", "3e", "4i")):
                vox = prefix.with_name(f"{prefix.name}_{slot}.vox")
                if not vox.is_file() and index == 3:
                    vox = prefix.with_name(f"{prefix.name}_3e.vox")
                chart = compile_chart(vox.read_bytes(), fixture, {})
                if chart["ticks_per_quarter"] != 48 or chart["meter"] != [4, 4] or len(chart["meter_map"]) != 1:
                    raise ValueError(f"Unexpected Automation Paradise grid in {vox}")
                # The arranged chart rebuilds timing at the database BPM.
                for event in chart["events"]:
                    event.pop("position")
                    event.pop("time_ms")
                    if "hold_end" in event:
                        event["hold_end"].pop("time_ms")
                for cue in chart["controllers"]:
                    cue.pop("position")
                    cue.pop("time_ms")
                write_json(destination / f"{wave}-{index}.json", {
                    "events": chart["events"], "controllers": chart["controllers"],
                    "other_vox_sections": {key: value for key, value in chart["other_vox_sections"].items()
                        if key in ("FORMAT VERSION", "TAB EFFECT INFO", "FXBUTTON EFFECT INFO", "TAB PARAM ASSIGN INFO")},
                })
    write_json(output / "arrangement.json", {
        "bpm": float(db.findtext("mdb/music/meta/bpm")), "tracks": tracks,
        "judgment": fixture["judgment"], "offsets": fixture["offsets"],
    })
    print(f"Exported {len(tracks) - 2} source songs and two common transition banks")


if __name__ == "__main__":
    main()
