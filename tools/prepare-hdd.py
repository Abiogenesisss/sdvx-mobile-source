"""Prepare local assets for the Android build. Call with a contents, data or HDD directory."""
import argparse
from importlib import import_module
from pathlib import Path
import subprocess
import sys

from hdd_inputs import locate_contents, require_profile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hdd",type=Path)
    parser.add_argument("--sdk",type=Path,required=True)
    parser.add_argument("--ffmpeg",type=Path,required=True)
    parser.add_argument("--service",type=Path)
    parser.add_argument("--steps",nargs="+",choices=("songs","player","graphics"),
                        default=("songs","player","graphics"),help="Resume only named asset groups")
    parser.add_argument("--song-ids",type=int,nargs="+",help="Only embed these original music folders and jackets")
    args=parser.parse_args()
    contents=locate_contents(args.hdd)
    require_profile(contents)
    repo=Path(__file__).resolve().parents[1]
    app=repo/"development/android-native"
    print(f"Using local data in {contents}; supported DLL extraction profile 2026101300.",flush=True)
    if "songs" in args.steps:
        import_module("prepare-songs").prepare(contents,args.service,args.song_ids)
    if "player" in args.steps:
        import_module("prepare-player-assets").prepare(contents,args.ffmpeg,args.sdk)
    if "graphics" in args.steps:
        subprocess.run([sys.executable, str(repo/"tools/prepare-graphics.py"), str(contents/"data"),
                        str(app/"assets-reference"),str(app/"src/main/assets"),"--ffmpeg",str(args.ffmpeg),
                        "--registration",str(repo/"local-inputs/stage-registration.json")],check=True)


if __name__ == "__main__":
    main()
