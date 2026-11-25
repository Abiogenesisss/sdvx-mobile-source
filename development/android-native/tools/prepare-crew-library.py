"""Export the original Cubism files and S3P voice slots for the native crew library."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from hashlib import sha256
from importlib import import_module
import json
from pathlib import Path
import shutil
import subprocess


# NABLA leaves these PIN slots silent. Use the same character's original recordings.
PIN_VOICE_BANKS = {"vo032_rasis_nabra": "vo001_rasis", "vo033_grace_nabra": "vo004_grace"}
PIN_VOICE_SLOTS = {"entry07", "entry14"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_data", type=Path)
    parser.add_argument("ffmpeg", type=Path)
    parser.add_argument("output", type=Path, help="Runtime crew directory")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    original = args.game_data / "graphics/live2d"
    files = set()
    for setting in original.rglob("*.model3.json"):
        refs = json.loads(setting.read_text(encoding="utf-8-sig"))["FileReferences"]
        files.add(setting)
        files.update(setting.parent / refs[key] for key in ("Moc", "Physics", "Pose", "DisplayInfo", "UserData")
                     if refs.get(key))
        files.update(setting.parent / name for name in refs["Textures"])
        files.update(setting.parent / entry["File"] for group in refs["Motions"].values() for entry in group)
        files.update(setting.parent / entry["File"] for entry in refs.get("Expressions", ()))
    for file in sorted(files):
        if not file.is_file():
            print(f"Missing on original disc: {file.relative_to(original)}", flush=True)
            continue
        target = args.output / "live2d" / file.relative_to(original)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(file, target)
    print(f"Copied {len(files)} original Cubism files", flush=True)

    read_bank = import_module("extract-audio-bank").read_bank
    folder = args.output / "voice"
    folder.mkdir(parents=True, exist_ok=True)
    slots, unique = {}, set()

    def encode(payload, path):
        temporary = path.with_suffix(".partial")
        subprocess.run([str(args.ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "asf", "-i", "pipe:0", "-map", "0:a:0", "-sample_fmt", "s16",
                        "-compression_level", "5", "-f", "flac", str(temporary)], input=payload, check=True)
        temporary.replace(path)

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for bank in sorted((args.game_data / "sound/voice").glob("vo*")):
            voices, pending = {}, []
            for archive in sorted(bank.glob("*.s3p")):
                for entry, payload in read_bank(archive):
                    name = sha256(payload).hexdigest()[:24] + ".flac"
                    voices[entry["name"]] = [name, entry["s3v_gain_db"]]
                    if name not in unique:
                        unique.add(name)
                        path = folder / name
                        if not path.is_file():
                            pending.append(pool.submit(encode, payload, path))
            if bank.name in PIN_VOICE_BANKS:
                original = slots[PIN_VOICE_BANKS[bank.name]]
                voices.update({slot: original[slot] for slot in PIN_VOICE_SLOTS})
            for task in pending:
                task.result()
            slots[bank.name] = voices
            print(f"{bank.name}: {len(voices)} original slots", flush=True)
    (folder / "index.json").write_text(json.dumps(slots, separators=(",", ":")), encoding="utf-8")
    print(f"{sum(map(len, slots.values()))} voice slots, {len(unique)} unique recordings", flush=True)


if __name__ == "__main__":
    main()
