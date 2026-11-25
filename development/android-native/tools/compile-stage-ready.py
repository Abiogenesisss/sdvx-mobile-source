"""Export original READY AFP and its full transition sound."""
from pathlib import Path
from importlib import import_module
import argparse
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graphics", type=Path, help="Original graphics/ver07 directory")
    parser.add_argument("sound", type=Path, help="Decoded original se_08_big01_main_ready.wav")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    afp = import_module("prepare-cabinet-afp")
    afp.prepare(args.graphics, args.output, *afp.SCENES["stage_ready"])
    shutil.copyfile(args.sound, args.output / "ready.wav")


if __name__ == "__main__":
    main()
