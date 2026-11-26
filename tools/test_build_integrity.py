"""Focused regressions for HDD judgment extraction and interrupted pack publication."""
from contextlib import redirect_stdout
from importlib import import_module
import io
import json
from pathlib import Path
import struct
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
from zipfile import ZipFile

from judgment_windows import WINDOWS, read_judgment_tables, validate_judgment_tables

packs = import_module("build-incremental-packs")


class JudgmentTests(unittest.TestCase):
    def test_bad_boundaries_and_cached_tables(self):
        last = max(offset for entries in WINDOWS.values() for offset, _, _ in entries if offset is not None)
        dll = bytearray(last + 4)
        for layout in WINDOWS.values():
            for offset, _, ms in layout:
                if offset is not None:
                    struct.pack_into("<f", dll, offset, ms / 1000)
        tables = read_judgment_tables(dll)
        with self.assertRaisesRegex(ValueError, "Missing chip"):
            read_judgment_tables(dll[:-1])
        # These finite replacements preserve ascending order in their own tables.
        for name, i, wrong in (("chip", 0, -30.0), ("hold_narrow", 0, 0.012),
                               ("laser_slam", 0, -1e-30), ("chip", 6, 0.06),
                               ("chip", 7, float("inf")), ("chip", 7, float("nan"))):
            with self.subTest(name=name, i=i, wrong=wrong):
                offset = WINDOWS[name][i][0]
                previous = dll[offset:offset+4]
                struct.pack_into("<f", dll, offset, wrong)
                with self.assertRaisesRegex(ValueError, name):
                    read_judgment_tables(dll)
                dll[offset:offset+4] = previous
        tables["chip"][4]["from_seconds"] = 0.0001
        with self.assertRaisesRegex(ValueError, "chip"):
            validate_judgment_tables(tables)


class DeliveryTests(unittest.TestCase):
    def test_interruption_and_retry(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            hdd = root / "contents"
            (hdd / "modules").mkdir(parents=True)
            (hdd / "modules/soundvoltex.dll").touch()
            (hdd / "data/others").mkdir(parents=True)
            (hdd / "data/others/music_db.xml").touch()
            folder = "0001_test"
            music = hdd / "data/music" / folder
            music.mkdir(parents=True)
            chart, song, jacket = "0001_test_1n.vox", "0001_test.s3v", "jk_0001_1.png"
            for name in (chart, song, jacket):
                (music / name).write_bytes(b"old")
            apk = root / "base.apk"
            catalog = [{"id": 1, "folder": folder, "music": song, "charts": [
                {"slot": 1, "bundled": False, "chart": chart, "jacket": jacket}]}]
            with ZipFile(apk, "w") as archive:
                archive.writestr("assets/songs/library.json", json.dumps(catalog))
            output = root / "published"
            args = ["packs", "--hdd", str(hdd), "--apk", str(apk), "--output", str(output)]
            def run():
                with patch.object(sys, "argv", [*args, "--part", "music"]), redirect_stdout(io.StringIO()):
                    packs.main()
            def music_in_release(directory):
                index = json.loads((directory / "music/index.json").read_bytes())
                item, = index["archives"]
                path = directory / "music" / item["file"]
                self.assertEqual(path.stat().st_size, item["bytes"])
                with ZipFile(path) as archive:
                    return archive.read(f"music/{folder}/{song}")
            def current():
                return output / json.loads((output / "latest.json").read_bytes())["directory"]

            run()
            first = current()
            (music / song).write_bytes(b"new audio")
            # Simulate a failed second category after the first has already finished.
            with patch.object(sys, "argv", args), patch.dict(sys.modules, {"imageio_ffmpeg": unittest.mock.Mock()}), \
                    patch.object(packs.subprocess, "run", side_effect=KeyboardInterrupt), \
                    redirect_stdout(io.StringIO()), self.assertRaises(KeyboardInterrupt):
                packs.main()
            self.assertEqual(current(), first)
            self.assertEqual(list(output.glob("release-*")), [first])
            self.assertEqual(music_in_release(first), b"old")

            replace = Path.replace
            def interrupt_at_switch(src, dst):
                if dst == output / "latest.json":
                    raise KeyboardInterrupt
                return replace(src, dst)
            with patch.object(Path, "replace", interrupt_at_switch), self.assertRaises(KeyboardInterrupt):
                run()
            self.assertEqual(current(), first)
            self.assertEqual(music_in_release(first), b"old")
            run()
            self.assertNotEqual(current(), first)
            self.assertEqual(music_in_release(current()), b"new audio")
            self.assertEqual(music_in_release(first), b"old")


if __name__ == "__main__":
    unittest.main()
