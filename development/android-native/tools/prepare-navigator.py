"""Copy original Cubism models and export the NABLA navigator's event voice banks.

Temporary decoded banks stay under --work. The output is the runtime crew folder.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


BANKS = {
    "02_entry": ("entry01", "entry02", "entry11", "entry12", "entry13"),
    "03_modeselect": ("mode01", "mode02", "mode05"),
    "04_customize": ("cus01", "cus03", "cus04", "cus05"),
    "05_valgene": ("vg01", "vg02"),
    "07_musicselect": ("music06", "music10", "music11", "music13", "music14", "music15", "music16"),
    "08_matching": ("match01",),
    "09_comp": ("comp01", "comp02", "comp03", "comp04", "comp07"),
    "10_result": tuple(f"result{i:02}" for i in range(1, 21)),
    "11_totalresult": ("total01",),
    "12_end": ("end01", "end02", "end03", "end04", "end06"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_data", type=Path)
    parser.add_argument("sdk", type=Path, help="CubismSdkForNative-5-r.5")
    parser.add_argument("ffmpeg", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    for role, bank in (("rasis", "vo032"), ("grace", "vo033")):
        original = args.game_data / "graphics/live2d" / f"{role}_ver7"
        setting_path = original / f"{role}_ver7.model3.json"
        settings = json.loads(setting_path.read_text(encoding="utf-8"))
        refs = settings["FileReferences"]
        files = [setting_path.name, refs["Moc"], refs["Physics"], refs["DisplayInfo"], *refs["Textures"]]
        files += [motion["File"] for group in refs["Motions"].values() for motion in group]
        model = args.output / role / "model"
        for name in files:
            target = model / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original / name, target)
        for suffix, voices in BANKS.items():
            archive = args.game_data / "sound/voice" / f"{bank}_{role}_nabra" / f"{bank}_{suffix}.s3p"
            decoded = args.work / role / suffix
            subprocess.run([sys.executable, str(Path(__file__).with_name("extract-audio-bank.py")),
                            str(archive), str(args.ffmpeg), str(decoded)], check=True)
            for voice in voices:
                shutil.copyfile(decoded / f"{voice}.wav", args.output / role / f"{voice}.wav")
    shader_root = args.sdk / "Framework/src/Rendering/OpenGL/Shaders/StandardES"
    shutil.copytree(shader_root, args.output / "shaders", dirs_exist_ok=True)
    licenses = args.output / "licenses"
    licenses.mkdir(exist_ok=True)
    for source in (args.sdk / "LICENSE.md", args.sdk / "NOTICE.md", args.sdk / "Framework/LICENSE.md"):
        shutil.copyfile(source, licenses / ("Framework-LICENSE.md" if source.parent.name == "Framework" else source.name))
    print("Prepared original models, SDK shaders and event voices:", args.output)


if __name__ == "__main__":
    main()
