"""Decode the original S3P bank and preserve S3V gain and loop metadata."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import wave


def read_bank(bank):
    raw = bank.read_bytes()
    magic, count = struct.unpack_from("<4sI", raw)
    if magic != b"S3P0" or 8 + 8 * count > len(raw):
        raise ValueError(f"Invalid S3P archive {bank}")
    definitions = {
        int(line.split()[2]): line.split()[1]
        for line in bank.with_suffix(".def").read_text(encoding="cp932").splitlines()
        if line.startswith("#define")
    }
    for index in range(count):
        offset, size = struct.unpack_from("<II", raw, 8 + index * 8)
        magic, header_size, payload_size, _, loop_sample, flags = struct.unpack_from("<4sIIIII", raw, offset)
        if magic != b"S3V0" or header_size != 32 or header_size + payload_size > size or offset + size > len(raw):
            raise ValueError(f"Invalid S3V entry {index}")
        # 18064b545 reads the two signed Q8.8 dB fields and sums them.
        gain, gain_adjustment = struct.unpack_from("<hh", raw, offset + 20)
        yield {"index": index, "name": definitions[index], "s3v_gain_db": (gain + gain_adjustment) / 256,
               "s3v_loop_sample": loop_sample, "s3v_flags": f"{flags:#010x}"}, raw[
                   offset + header_size:offset + header_size + payload_size]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bank", type=Path)
    parser.add_argument("ffmpeg", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    entries = []
    for entry, payload in read_bank(args.bank):
        sample = subprocess.check_output([
            str(args.ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error",
            "-f", "asf", "-i", "pipe:0", "-map", "0:a:0",
            "-ac", "2", "-ar", "44100", "-f", "s16le", "pipe:1",
        ], input=payload)
        file = f"{entry['name']}.wav"
        with wave.open(str(args.output / file), "wb") as out:
            out.setparams((2, 2, 44_100, 0, "NONE", "not compressed"))
            out.writeframes(sample)
        entries.append({**entry, "file": file, "frames": len(sample) // 4, "sample_rate": 44_100})
    (args.output / "bank.json").write_text(json.dumps(entries, indent=2), encoding="utf-8")
    print(f"{args.bank.name}: extracted {len(entries)} samples")


if __name__ == "__main__":
    main()
