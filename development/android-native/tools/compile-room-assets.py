"""Export original VALKYRIE volume banners."""
from pathlib import Path
import argparse
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("graphics", type=Path)
    parser.add_argument("assets", type=Path)
    args = parser.parse_args()
    banners = args.assets / "valkyrie" / "banners"
    banners.mkdir(parents=True, exist_ok=True)
    for volume in range(1, 23):
        name = f"bn_gene_{volume:02}.png"
        shutil.copyfile(args.graphics / "valgene_banner" / name, banners / name)


if __name__ == "__main__":
    main()
