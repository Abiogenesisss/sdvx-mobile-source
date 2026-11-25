"""Build the selected songs, jackets and seven-generation registration from a local HDD."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

from hdd_inputs import locate_contents, require_profile

def prepare(contents, service=None, song_ids=None):
    game = contents / "data"
    repo = Path(__file__).resolve().parents[1]
    app = repo / "development/android-native"
    tools = app / "tools"
    assets = app / "src/main/assets"
    songs = assets / "songs"
    work = repo / "local-inputs"
    work.mkdir(exist_ok=True)
    (app / "assets-reference").mkdir(exist_ok=True)
    dll = (contents / "modules/soundvoltex.dll").read_bytes()

    # File offsets of binary32 constants used by the window initializers in this checked DLL.
    # Each pair is a lower-bound inclusive time and the runtime result ID.
    chip = [(10079660,1),(10079648,2),(10079616,3),(10079612,4),(None,6),(10076992,7),(10077060,8),(10077180,10)]
    narrow = [(10079596,5),(10076976,10)]
    windows = {"chip":chip, "fx_chip_special":[(chip[0][0],0),(chip[1][0],3),*chip[3:]],
               "hold_wide":[(10079648,5),(10077132,10)], "hold_narrow":narrow, "laser_track":narrow,
               "laser_slam":[(10079620,5),(10077064,10)]}
    tables = {}
    for name, entries in windows.items():
        rows = []
        for offset, result in entries:
            word = dll[offset:offset+4] if offset is not None else bytes(4)
            rows.append({"from_seconds":struct.unpack("<f",word)[0], "result_id":result,
                         "float_bits": f"0x{int.from_bytes(word,'little'):08x}"})
        tables[name] = rows
    # Timing offset application is owned by stageJudgeTime / stageDrawTime in the runtime.
    songs.mkdir(parents=True,exist_ok=True)
    judgment = songs / "judgment.json"
    judgment.write_text(json.dumps({"judgment":{"tables":tables},"offsets":{"unit":"milliseconds",
        "player_setting_range":[-64,64]}},separators=(",",":")), encoding="utf-8")

    def run(script, *values):
        subprocess.run([sys.executable, str(tools / script), *map(str,values)],check=True)

    if song_ids is None:
        recipe = (repo / "docs/SONGS.md").read_text(encoding="utf-8").split("```text\n",1)[1].split("```",1)[0]
        song_ids = [int(value) for value in recipe.split()]
    # 1438 is dynamically arranged and has no ordinary VOX. Publish the arrangement only if complete.
    from tempfile import TemporaryDirectory
    import shutil
    from importlib import import_module
    sys.path.insert(0,str(tools))
    arrangement=songs/"1438"/"arrangement.json"
    if arrangement.is_file():
        # Retire the previous generated phrase banks, while keeping its independent background.
        base=arrangement.parent.resolve()
        for track in json.loads(arrangement.read_bytes())["tracks"]:
            folder=base/str(int(track["id"]))
            if folder.is_dir():
                if folder.resolve().parent != base or folder.is_symlink() or folder.is_junction():
                    raise ValueError(f"Unexpected linked arrangement bank: {folder}")
                shutil.rmtree(folder)
        (base/"jacket-1.png").unlink(missing_ok=True)
        arrangement.unlink()
    if 1438 in song_ids and (game/"sound/automa/waves/wave_info_2019apr.xml").is_file() and list((game/"music").glob("1438_*")):
        with TemporaryDirectory(prefix="sdvx-arrangement-",dir=work) as directory:
            scratch=Path(directory)
            try:
                import_module("prepare-automation-paradise").prepare(game,scratch,judgment)
            except (FileNotFoundError,StopIteration) as error:
                print(f"Skipped incomplete dynamic song 1438: {error}")
            else:
                shutil.copytree(scratch/"songs/1438",songs/"1438",dirs_exist_ok=True)
    run("prepare-song-catalog.py",game,songs,"--songs",*song_ids)
    service = service or contents.parent / "asphyxia/plugins/sdvx@asphyxia/data/nbl.ts"
    run("prepare-selection.py",game,*( [service] if service.is_file() else [] ),songs / "selection.json")
    run("compile-skill-selector.py",game,*( [service] if service.is_file() else [] ),assets/"skill_selector","--courses-only")
    run("prepare-entry-notice.py","--contents",contents)
    run("extract-stage-registration.py","--root",repo,"--contents",contents,"--output",work/"stage-registration.json",
        "--runtime-inc",app/"src/main/cpp/stage_registration_2026101300.inc")
    print("Prepared catalog, bundled charts, arrangement, selection and native registration.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hdd", type=Path)
    parser.add_argument("--service", type=Path, help="Optional local nbl.ts containing service-specific folder data")
    parser.add_argument("--song-ids",type=int,nargs="+")
    args = parser.parse_args()
    contents = locate_contents(args.hdd)
    require_profile(contents)
    prepare(contents,args.service,args.song_ids)
