"""Export the character selector catalog recovered from the original program."""
import argparse
from concurrent.futures import ProcessPoolExecutor
from importlib import import_module
import json
from pathlib import Path
import shutil
from tempfile import TemporaryDirectory
from xml.etree import ElementTree

from bemani.format import IFS
from PIL import Image


PORTRAIT_MOTIONS = {
    "In": ["chara_anime", "in"], "Idle": ["chara_anime", "neu_loop"],
    "Ok": ["chara_anime", "kettei"], "Out": ["chara_anime", "out"],
    **{motion + suffix: ["result_" + reaction, "loop" if suffix else "in"]
       for motion, reaction in (("R_Bad", "bad"), ("R_Good", "good"), ("R_Verygood", "verygood"))
       for suffix in ("", "_Loop")},
}


def crew_placements(placement):
    positions = []
    # 18026c010: identity MATCHING/RESULT entries inherit the default group.
    # The comparison uses FLT_EPSILON (18099d82c), before scene transforms.
    for values, (x, y, scale) in zip(placement, ((420, 1400, 1), (440, 1300, .85), (500, 1300, 1))):
        sx, sy, dx, dy = values
        if abs(sx - 1) <= 2**-23 and abs(sy - 1) <= 2**-23 and dx == 0 and dy == 0:
            sx, sy, dx, dy = placement[0]
        positions.append([x + dx, y + dy, sx * scale, sy * scale])
    return positions


def export_afp(source, original, target, matching):
    if (target / "afp-library.json").is_file():
        return True
    if original.parent.name == "crew_portrait":
        # Portrait movies share the portrait template and NABLA READY lights.
        library = {"chara_anime": {"source": "crew_pattern_main", "clips": {
            "in": {"start": "in", "end": "loop"},
            "neu_loop": {"start": "loop", "end": "kettei", "continuous": True},
            "kettei": {"start": "kettei", "end": "kettei_end"},
            "out": {"start": "out", "end": "out_end"},
        }}}
        for reaction in ("bad", "good", "verygood"):
            library["result_" + reaction] = {"source": original.stem + "_" + reaction, "clips": {
                "in": {"start": "in", "end": "loop"},
                "loop": {"start": "loop", "continuous": True},
            }}
        import_module("prepare-cabinet-afp").prepare(original.parent.parent / "ver07", target,
            ("ready", "crew_portrait/crew_pattern", "crew_portrait/" + original.stem), library)
        return True
    with TemporaryDirectory(prefix="nabla-crew-") as temporary:
        work = Path(temporary)
        archive = work / original.stem
        raw = IFS(original.read_bytes(), decode_binxml=True, decode_textures=True)
        inventory = source.parents[2] / "待复核" / "graphics/chara" / source.name
        for filename in raw.filenames:
            directory = filename.split("\\", 1)[0]
            if directory not in ("afp", "geo") and not (directory == "tex" and filename.endswith(".png")):
                continue
            relative = Path(*filename.split("\\"))
            destination = archive / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            extracted = next((folder / relative for folder in (source, inventory)
                              if directory == "tex" and (folder / relative).is_file()), None)
            if extracted is not None:
                shutil.copyfile(extracted, destination)
            else:
                destination.write_bytes(raw.read_file(filename))
        library = {}
        read_movie = import_module("compile-stage-background").BackgroundSWF
        names = ("mach_in", "mach_neu", "mach_go") if matching else ("chara_anime",)
        for name in names:
            if not (archive / "afp/bsi" / name).is_file():
                continue
            movie = read_movie(name, (archive / "afp" / name).read_bytes(),
                               (archive / "afp/bsi" / name).read_bytes())
            movie.parse()
            if matching:
                clip = "neu_loop" if name == "mach_neu" else "0"
                start = clip if clip in movie.labels else 0
                clips = {clip: {"start": start, "continuous": name == "mach_neu"}}
            else:
                clips = {start: {"start": start, "end": end, "continuous": start == "neu_loop"}
                         for start, end in (("in", "neu_loop"), ("neu_loop", "kettei"),
                                            ("kettei", "kettei_end"), ("out", "out_end"))}
            library[name] = {"clips": clips}
        if not library:
            return False
        staged = work / "output"
        import_module("prepare-original-afp").prepare(archive, staged, {
            "movies": [{"name": movie.get("source", name)} for name, movie in library.items()]})
        regions = {}
        textures = ElementTree.fromstring(raw.read_file("tex\\texturelist.xml"))
        for atlas in textures.findall("texture"):
            if atlas.attrib["mag_filter"] != "nearest":
                continue
            for entry in atlas.findall("image"):
                name = entry.attrib["name"]
                if (staged / "tex" / (name + ".png")).is_file():
                    regions[name] = {"nearest": True}
        for directory in ("afp", "geo", "tex"):
            shutil.copytree(staged / directory, target / directory, dirs_exist_ok=True)
        if regions:
            (target / "afp-textures.json").write_text(json.dumps(regions), encoding="utf-8")
        (target / "afp-library.json").write_text(json.dumps(library, indent=2) + "\n", encoding="utf-8")
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_data", type=Path)
    parser.add_argument("catalog", type=Path, help="Recovered crew and Live2D tables")
    parser.add_argument("extracted", type=Path, help="Extracted original graphics directory")
    parser.add_argument("output", type=Path, help="Runtime crew directory")
    parser.add_argument("--jobs", type=int, default=5)
    parser.add_argument("--crews", nargs="+", type=int, help="Update only these character IDs in an existing catalog")
    args = parser.parse_args()
    data = json.loads(args.catalog.read_text(encoding="utf-8-sig"))
    graphics = args.game_data / "graphics"
    art = args.output / "art"
    art.mkdir(parents=True, exist_ok=True)
    icons = IFS((graphics / "chara/chara_icon.ifs").read_bytes(), decode_binxml=True)
    xml = ElementTree.fromstring(icons.read_file("tex\\texturelist.xml"))
    regions = {row.attrib["name"]: row for atlas in xml.findall("texture") for row in atlas.findall("image")}

    def export_icon(name):
        if name not in regions:
            return ""
        target = art / (name + ".png")
        if not target.is_file():
            x0, x1, y0, y1 = (int(v) // 2 for v in regions[name].findtext("imgrect").split())
            Image.frombytes("RGBA", (x1 - x0, y1 - y0), icons.read_file("tex\\" + name), "raw", "BGRA").save(target)
        return "crew/art/" + target.name

    actors = []
    for model in data["models"]:
        source = model["path"]
        if not source:
            actors.append({})
            continue
        name = source.removeprefix("/data/graphics/live2d/")
        actor = {"model": "crew/live2d/" + name, "placements": crew_placements(model["placement"])}
        if model["index"] in (52, 54, 56):
            suffix = str((model["index"] - 50) // 2)
            actor["motions"] = {name: name + suffix for name in ("In", "Out", "Ok")}
        actors.append(actor)

    rows = [row for row in data["rows"] if 0 < row["id"] <= 200 and row["strings"][2]]
    # Preserve the two indices used by early local profiles and sort original
    # character groups independently in the selector.
    rows.sort(key=lambda row: ({178: -2, 177: -1}.get(row["id"], row["id"]), row["style"]))
    base = {row["id"]: row for row in rows if row["style"] == 0}
    previous = None
    if args.crews:
        previous = json.loads((args.output / "catalog.json").read_bytes())
        rows = [row for row in rows if row["id"] in args.crews]
        if {row["id"] for row in rows} != set(args.crews):
            raise ValueError("Unknown crew ID")
    crews, pending = [], {}
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for row in rows:
            room, matching, icon, name, name_image, preview, _ = row["strings"]
            bank = row["bank"]
            if bank is None:
                bank = base.get(row["id"], row)["bank"]
            info = {"id": row["id"], "style": row["style"], "order": row["header"][2],
                    "icon": export_icon(icon), "name_image": export_icon(name_image), "name": name,
                    "voice": data["banks"][bank] if bank is not None else "", "actors": row["models"]}
            if preview:
                target = art / (preview + ".png")
                shutil.copyfile(graphics / "crew_icon" / target.name, target)
                info["preview"] = "crew/art/" + target.name
            if row["model"]:
                file = graphics / "crew_portrait" / f"crew_{row['id']:03d}_main.png"
                archive = graphics / "crew_portrait" / f"crew_{row['id']:03d}.ifs"
                if archive.is_file():
                    room = "crew_portrait/" + archive.name
                    info["afp_motions"] = PORTRAIT_MOTIONS
                elif file.is_file():
                    shutil.copyfile(file, art / file.name)
                    info["portrait"] = "crew/art/" + file.name
                else:
                    info["portrait"] = info.get("preview", "")
            for field, source in (("room_afp", room), ("matching_afp", matching)):
                if not source:
                    continue
                key = Path(source).stem
                target = args.output / "afp" / key
                if key not in pending:
                    pending[key] = pool.submit(export_afp, args.extracted / source, graphics / source,
                                               target, field == "matching_afp")
                info[field] = "crew/afp/" + key
            crews.append(info)
        exported = {}
        for key, job in pending.items():
            try:
                exported[key] = job.result()
            except Exception as error:
                exported[key] = False
                print(f"{key}: {error}", flush=True)
        for crew in crews:
            for field in ("room_afp", "matching_afp"):
                if field in crew:
                    key = Path(crew[field]).name
                    if not exported[key]:
                        if "afp_motions" in crew:
                            raise RuntimeError(f"Could not export original portrait AFP {key}")
                        if (args.output / "legacy" / key / "timeline.dat").is_file():
                            crew[field] = "crew/legacy/" + key
                        else:
                            del crew[field]
    if previous is not None:
        replacements = {(crew["id"], crew["style"]): crew for crew in crews}
        crews = [replacements.get((crew["id"], crew["style"]), crew) for crew in previous["crews"]]
    (args.output / "catalog.json").write_text(json.dumps({"actors": actors, "crews": crews}, ensure_ascii=False,
                                                       separators=(",", ":")), encoding="utf-8")
    print(f"{len(crews)} selectable crew styles, {sum(exported.values())} original AFP archives", flush=True)


if __name__ == "__main__":
    main()
