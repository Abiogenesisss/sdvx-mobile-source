"""Compare local asset paths with the repository baseline or supplied source roots."""

import argparse
from collections import defaultdict
from fnmatch import fnmatchcase
from hashlib import sha256
import json
from pathlib import Path
import re
from zipfile import BadZipFile, ZipFile


# Gradle 8.13 uses Ant's default exclusions in addition to the build.gradle rules.
ANT_EXCLUDES = (
    "*~", "#*#", ".#*", "%*%", "._*", "CVS", ".cvsignore", "SCCS", "vssver.scc",
    ".svn", ".git", ".gitattributes", ".gitignore", ".gitmodules", ".hg", ".hgignore",
    ".hgsub", ".hgsubstate", ".hgtags", ".bzr", ".bzrignore", ".DS_Store",
)
REFERENCE_SUBTREES = ("songs/666/background", "menu/options", "transition")
REFERENCE_FILES = {"menu/jacket.png", "stage/console.png", "songs/666/timing.json"}
BASELINE = Path(__file__).resolve().parents[1] / "docs/asset-path-baseline.md"
LOCAL_REPORT = Path(__file__).resolve().parents[1] / "local-inputs/asset-paths-missing.txt"


def source_paths(root, override, bundled, is_reference):
    def excluded(relative):
        path = relative.as_posix()
        name = relative.name
        if any(fnmatchcase(name, pattern) for pattern in ANT_EXCLUDES):
            return True
        if path.startswith("music/") and relative.parts[1] not in bundled:
            return True
        if name.endswith(".log") or path == "songs/catalog-audit.json":
            return True
        if path == "subscreen/avatars" or path.startswith("subscreen/avatars/"):
            return True
        if path.startswith("songs/") and name.endswith(".flac"):
            return True
        if len(relative.parts) == 3 and relative.parts[:2] == ("crew", "kureha") and name.endswith(".webp"):
            return True
        if not is_reference:
            return False
        return (
            path in REFERENCE_FILES or path in REFERENCE_SUBTREES
            or (path.startswith("result/") and name not in {"bg_grid_result.png", "bgm.wav"})
        )

    for folder, dirs, files in root.walk(on_error=raise_walk_error):
        relative = folder.relative_to(root)
        dirs[:] = [name for name in dirs if not excluded(relative / name)]
        for name in files:
            entry = relative / name
            path = entry.as_posix()
            if excluded(entry) or not (folder / name).is_file():
                continue
            yield path


def raise_walk_error(error):
    raise error


def android_packaged(path):
    # Android's default ignoreAssetsPatterns apply to each file or directory name.
    parts = path.lower().split("/")
    return not (
        any(name.startswith(".") or name in {"cvs", "thumbs.db", "picasa.ini"}
            or name.endswith((".scc", "~")) for name in parts)
        or any(name.startswith("_") for name in parts[:-1])
    )


def summarize(paths):
    groups = defaultdict(list)
    for path in sorted(paths):
        group, separator, _ = path.partition("/")
        groups[group if separator else "(root)"].append(path)
    return {
        group: (len(entries), sha256("".join(path + "\n" for path in entries).encode("utf-8")).hexdigest())
        for group, entries in sorted(groups.items())
    }


def print_summary(paths):
    print("| Asset directory | Required paths | Path SHA-256 |")
    print("| --- | ---: | --- |")
    for group, (count, digest) in summarize(paths).items():
        label = group.replace("|", "\\|").replace("\n", "\\n")
        print(f"| {label} | {count} | `{digest}` |")


def compare_baseline(actual):
    rows = re.findall(
        r"^\| ([^|]+?) \| ([0-9]+) \| `([a-f0-9]{64})` \|$",
        BASELINE.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    expected = {group: (int(count), digest) for group, count, digest in rows}
    if not expected or len(expected) != len(rows):
        raise ValueError("Invalid asset path baseline")
    observed = summarize(actual)
    # Songs vary with the local release. Retain its digest as informational data.
    expected.pop("songs", None)  # Derived song presentations are release-specific too.
    observed.pop("songs", None)
    baseline_songs = expected.pop("music", (0, ""))
    actual_songs = observed.pop("music", (0, ""))
    if baseline_songs[0] or actual_songs[0]:
        comparison = "matches baseline" if baseline_songs == actual_songs else "differs from baseline"
        print(f"Songs: {actual_songs[0]} paths, baseline {baseline_songs[0]} ({comparison}; informational).")
    mismatches = [
        group for group in sorted(expected.keys() | observed.keys())
        if expected.get(group) != observed.get(group)
    ]
    if mismatches:
        print("| Asset directory | Baseline paths | Observed paths |")
        print("| --- | ---: | ---: |")
        for group in mismatches:
            label = json.dumps(group, ensure_ascii=True).replace("|", "\\|")
            print(f"| {label} | {expected.get(group, (0,))[0]} | {observed.get(group, (0,))[0]} |")
        print(f"\n{len(mismatches)} asset groups differ from the baseline in count or path digest.")
        return 1
    print(f"All {len(actual) - actual_songs[0]} paths in {len(observed)} checked groups match the repository baseline.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, help="reference root for subset checking")
    parser.add_argument("--override", type=Path, help="override root for subset checking")
    targets = parser.add_mutually_exclusive_group()
    targets.add_argument("--merged", type=Path, help="directory containing staged assets")
    targets.add_argument("--apk", type=Path, help="APK with assets under assets/")
    args = parser.parse_args()

    if (args.reference is None) != (args.override is None):
        parser.error("--reference and --override must be supplied together")
    if args.reference is None and args.merged is None and args.apk is None:
        parser.error("supply --merged or --apk to check the repository baseline")
    for role in ("reference", "override", "merged"):
        directory = getattr(args, role)
        if directory is not None and not directory.is_dir():
            parser.error(f"--{role} must identify an existing directory")

    try:
        if args.apk is not None:
            with ZipFile(args.apk) as apk:
                actual = {entry.filename[7:] for entry in apk.infolist()
                          if entry.filename.startswith("assets/") and not entry.is_dir()}
        elif args.merged is not None:
            actual = {
                (folder / name).relative_to(args.merged).as_posix()
                for folder, _, files in args.merged.walk(on_error=raise_walk_error)
                for name in files if (folder / name).is_file()
            }

        if args.reference is None:
            return compare_baseline(actual)

        catalog = json.loads((args.override / "songs/library.json").read_text(encoding="utf-8"))
        bundled = {song["folder"] for song in catalog if any(chart["bundled"] for chart in song["charts"])}
        expected = set(source_paths(args.override, args.override, bundled, is_reference=False))
        expected.update(source_paths(args.reference, args.override, bundled, is_reference=True))
        if args.apk is not None:
            expected = {path for path in expected if android_packaged(path)}
        print_summary(expected)
        if args.apk is None and args.merged is None:
            return 0

        missing = expected - actual
        LOCAL_REPORT.parent.mkdir(exist_ok=True)
        LOCAL_REPORT.write_text("".join(path + "\n" for path in sorted(missing)), encoding="utf-8", newline="\n")
    except (OSError, ValueError, TypeError, KeyError, BadZipFile) as error:
        parser.error(f"Could not read or compare the asset paths ({type(error).__name__}).")

    if missing:
        print(f"\nMissing {len(missing)} of {len(expected)} required paths. See local-inputs/{LOCAL_REPORT.name}.")
        return 1
    print(f"\nAll {len(expected)} required paths are present.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
