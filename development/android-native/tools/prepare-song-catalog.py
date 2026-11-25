"""Index HDD song metadata and materialize the selected original song directories."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import shutil
from stat import FILE_ATTRIBUTE_REPARSE_POINT
import unicodedata
from xml.etree import ElementTree

from importlib import import_module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data", type=Path)
    parser.add_argument("output", type=Path, help="Generated songs/ index directory (sibling of music/)")
    parser.add_argument("--songs", nargs="*", type=int, required=True)
    args = parser.parse_args()
    requested = set(args.songs)
    db = ElementTree.fromstring((args.data / "others/music_db.xml").read_bytes().decode("cp932"))
    sources = defaultdict(list)
    for directory in (args.data / "music").iterdir():
        prefix, sep, _ = directory.name.partition("_")
        if sep and prefix.isdecimal() and directory.is_dir():
            sources[int(prefix)].append(directory)
    args.output = args.output.resolve()
    source_music = (args.data/"music").resolve()
    if args.output.is_relative_to(source_music) or (args.output.parent/"music").resolve() == source_music:
        raise ValueError("Use a generated output directory outside HDD music")
    args.output.mkdir(parents=True, exist_ok=True)
    music = args.output.parent / "music"
    music.mkdir(exist_ok=True)
    # Conversion validates selected charts; their original bytes are what gets copied.
    compile_chart = import_module("compile-vox").compile_chart
    common_judgment = json.loads((args.output / "judgment.json").read_bytes())
    catalog, details, skipped = [], {}, []
    bundled_folders = set()
    audit = args.output/"catalog-audit.json"
    previous = json.loads(audit.read_bytes()).get("bundled_folders",[]) if audit.is_file() else []
    def is_link(path):
        return path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT
    if is_link(music):
        raise ValueError("The generated music root must not be a link")
    def sync_folder(source, target):
        if is_link(source) or (target.exists() and is_link(target)):
            raise ValueError(f"Unexpected linked song folder: {source.name}")
        target.mkdir(exist_ok=True)
        desired = set()
        for file in source.rglob("*"):
            if is_link(file):
                raise ValueError(f"Unexpected linked HDD entry: {file}")
            relative=file.relative_to(source)
            desired.add(relative)
            out=target/relative
            if out.exists() and is_link(out):
                raise ValueError(f"Unexpected linked build output: {out}")
            if file.is_dir():
                out.mkdir(exist_ok=True)
            else:
                info=file.stat()
                if not out.is_file() or (out.stat().st_size,out.stat().st_mtime_ns)!=(info.st_size,info.st_mtime_ns):
                    shutil.copy2(file,out)
        # This tree belongs to the exporter. Remove obsolete outputs when a song changes.
        for out in sorted(target.rglob("*"),key=lambda p:len(p.parts),reverse=True):
            if out.relative_to(target) not in desired:
                if is_link(out): raise ValueError(f"Unexpected linked build output: {out}")
                if out.is_dir(): out.rmdir()
                else: out.unlink()

    regular = {"novice":(1,"1n","NOV"),"advanced":(2,"2a","ADV"),"exhaust":(3,"3e","EXH"),
               "maximum":(5,"5m","MXM"),"ultimate":(6,"6u","ULT")}
    extra = {1:"INF",2:"INF",3:"GRV",4:"HVN",5:"VVD",6:"XCD",7:"ULT",8:"NBL"}
    categories = {"INF":"infinite","GRV":"gravity","HVN":"heavenly","VVD":"vivid","XCD":"exceed","ULT":"ultimate","NBL":"nabla"}
    for entry in db.findall("music"):
        song_id = int(entry.attrib["id"])
        info = entry.find("info")
        candidates = sources.get(song_id, [])
        if len(candidates) > 1:
            skipped.append([song_id,"ambiguous source directories"])
            continue
        source = candidates[0] if candidates else None
        folder = source.name if source else f"{song_id:04d}_{info.findtext('ascii','')}"
        if Path(folder).name != folder or "/" in folder or "\\" in folder:
            raise ValueError(f"Invalid directory name for ID {song_id}")
        shared = folder + ".s3v"
        selected = source is not None and song_id in requested
        if song_id in requested and source is None:
            skipped.append([song_id,"missing local song directory"])
        song = dict(id=song_id,folder=folder,title=info.findtext("title_name",""),artist=info.findtext("artist_name",""),
                    version=int(info.findtext("version")),bpm_min=int(info.findtext("bpm_min")),
                    bpm_max=int(info.findtext("bpm_max")),background=int(info.findtext("bg_no")),music=shared,
                    preview=folder+"_pre.s3v",charts=[])
        radar = {}
        for row in entry.findall("difficulty/*"):
            level = int(row.findtext("difnum","0"))
            if level <= 0:
                continue
            if row.tag in regular:
                slot, suffix, label = regular[row.tag]
                category = row.tag
            else:
                label = extra.get(int(info.findtext("inf_ver","0")))
                if not label:
                    continue
                slot,suffix,category = 4,"4i",categories[label]
            vox = f"{folder}_{suffix}.vox"
            track_audio = f"{folder}_{suffix}.s3v"
            base_jacket = f"jk_{song_id:04d}_{slot}"
            available = False
            generated = song_id == 1438
            if selected:
                jackets = [base_jacket+"_b.png",base_jacket+".png",f"jk_{song_id:04d}_1_b.png",f"jk_{song_id:04d}_1.png",base_jacket+"_s.png",f"jk_{song_id:04d}_1_s.png"]
                available = any((source/k).is_file() for k in jackets)
                if generated:
                    available = available and (args.output/"1438/arrangement.json").is_file()
                else:
                    available = available and (source/vox).is_file() and (
                        (source/shared).is_file() or (source/track_audio).is_file())
                if available and not generated:
                    try:
                        compile_chart((source/vox).read_bytes(),common_judgment,{})
                    except (ValueError, KeyError, IndexError, ArithmeticError) as error:
                        available = False
                        skipped.append([song_id,vox,str(error)])
                if not available:
                    skipped.append([song_id,suffix,"missing or invalid input"])
            record = dict(name=category.upper(),label=label,level=level,chart=vox,jacket=base_jacket+"_b.png",
                          slot=slot,max_exscore=int(row.findtext("max_exscore","0")),
                          effect=row.findtext("effected_by",""),illustrator=row.findtext("illustrator",""),bundled=available)
            if source and not (source/shared).is_file():
                record["music"] = track_audio
            song["charts"].append(record)
            radar[category] = {"radar":{x.tag:int(x.text) for x in row.findall("radar/*") if x.text and x.text.isdecimal()}}
        if not song["charts"]:
            continue
        if any(chart["bundled"] for chart in song["charts"]):
            # Only original files enter music/<folder>. Indexes and game-specific conversions live beside music/.
            sync_folder(source,music/folder)
            bundled_folders.add(folder)
        words=[song["title"],song["artist"],info.findtext("title_yomigana",""),info.findtext("artist_yomigana","")]
        words.extend(c["effect"]+"\n"+c["illustrator"] for c in song["charts"])
        text=unicodedata.normalize("NFKC","\n".join(words)).lower()
        song["search_key"]="".join(chr(ord(c)+0x60) if "\u3041"<=c<="\u3096" else c for c in text)
        catalog.append(song)
        details[str(song_id)]={"charts":radar}
    for old in set(previous)-bundled_folders:
        if not isinstance(old,str) or Path(old).name!=old or not old.partition("_")[0].isdecimal():
            raise ValueError("Invalid previously generated folder name")
        path=music/old
        if path.exists():
            if path.resolve().parent!=music.resolve() or is_link(path) or any(is_link(f) for f in path.rglob("*")):
                raise ValueError(f"Unexpected linked output: {path}")
            shutil.rmtree(path)
    for obj,name in ((catalog,"library.json"),(details,"details.json")):
        (args.output/name).write_text(json.dumps(obj,ensure_ascii=False,separators=(",",":")),encoding="utf-8")
    report = dict(indexed=len(catalog),bundled_folders=sorted(bundled_folders),skipped=skipped,
                  unknown_requested_ids=sorted(requested-{s["id"] for s in catalog}))
    (args.output/"catalog-audit.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
    print(f"Indexed {len(catalog)} songs; copied {len(bundled_folders)} original song folders, "
          f"{sum(c['bundled'] for s in catalog for c in s['charts'])} local charts. See catalog-audit.json.")


if __name__ == "__main__":
    main()
