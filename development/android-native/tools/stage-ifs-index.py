"""Publish virtual AFP asset paths backed by unmodified HDD IFS archives."""
from fnmatch import fnmatchcase
import json
from pathlib import Path, PurePosixPath
import shutil

from ifstools import IFS
from ifstools.handlers.image_file import ImageFile


class MemberPath:
    """Read the part of an IFS tree used by the AFP exporters."""
    def __init__(self, source, members, relative=PurePosixPath()):
        self.source = source
        self.members = members
        self.relative = relative

    def __truediv__(self, name):
        return MemberPath(self.source, self.members, self.relative / name)

    def __str__(self):
        return f"{self.source}!/{self.relative}"

    def __eq__(self, other):
        return isinstance(other, MemberPath) and (self.source, self.relative) == (other.source, other.relative)

    def __hash__(self):
        return hash((self.source, self.relative))

    @property
    def name(self):
        return self.relative.name or self.source.name

    @property
    def stem(self):
        return self.relative.stem

    @property
    def parent(self):
        return MemberPath(self.source, self.members, self.relative.parent)

    @property
    def entry(self):
        return self.members[self.relative]

    def is_file(self):
        return self.relative in self.members

    def read_bytes(self):
        return self.entry.load()

    def glob(self, pattern):
        return (self / path.name for path in self.members
                if path.parent == self.relative and fnmatchcase(path.name, pattern))


class StageIfsIndex:
    def __init__(self, data, common, override, parts):
        self.data = data
        self.common = common
        self.roots = (common, override)
        self.parts = parts
        self.index = common / "graphics/stage-ifs.json"
        self.archives = {}
        self.sources = {}
        self.entries = {}
        if self.index.is_file():
            previous = json.loads(self.index.read_text(encoding="utf-8"))
            if previous.get("version") != 1:
                raise ValueError(f"Unsupported stage IFS index version: {self.index}")
            for key, value in previous["entries"].items():
                if ("backgrounds" in parts and key.startswith("stage_bg/")) or (
                        "scenes" in parts and key.startswith("songs/")):
                    continue
                self.entries[key] = value

    def __enter__(self):
        return self

    def __exit__(self, *_):
        for archive, _ in self.archives.values():
            archive.close()

    def tree(self, path):
        if path not in self.archives:
            archive = IFS(str(path))
            members = {}
            for folder in archive.tree.all_folders:
                for name, file in folder.files.items():
                    if hasattr(file, "start"):
                        logical = PurePosixPath(*Path(folder.full_path).parts, name)
                        members[logical] = file
            self.archives[path] = (archive, MemberPath(path, members))
        return self.archives[path][1]

    def publish(self, source, destination):
        entry = source.entry
        physical = Path(entry.ifs_data.file.name).resolve()
        archive = physical.relative_to(self.data).as_posix()
        self.sources[archive] = physical
        key = next(destination.relative_to(root).as_posix()
                   for root in self.roots if destination.is_relative_to(root))
        info = {"archive": archive, "offset": entry.ifs_data.offset + entry.start, "size": entry.size}
        if isinstance(entry, ImageFile):
            width, height = entry.img_size
            info.update(width=width, height=height, format=entry.format, compression=entry.compress or "")
        self.entries[key] = info

    def write(self):
        for relative, source in sorted(self.sources.items()):
            target = self.common / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_suffix(".ifs.partial")
            try:
                shutil.copyfile(source, temporary)
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
        self.index.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.index.with_suffix(".json.partial")
        temporary.write_text(json.dumps({"version": 1, "entries": self.entries},
                                        sort_keys=True, separators=(",", ":")), encoding="utf-8")
        temporary.replace(self.index)
        self.remove_loose_exports()
        print(f"Indexed {len(self.entries):,} stage files across "
              f"{len({entry['archive'] for entry in self.entries.values()}):,} original IFS archives", flush=True)

    def remove_loose_exports(self):
        for root in self.roots:
            stage_dirs = []
            if "backgrounds" in self.parts:
                stage_dirs.extend(path for path in (root / "stage_bg").glob("[0-9]*") if path.name.isdecimal())
            if "scenes" in self.parts:
                stage_dirs.extend((root / "songs").glob("*/background"))
            for folder in stage_dirs:
                if not folder.is_dir():
                    continue
                if folder.resolve() != folder or not folder.is_relative_to(root):
                    raise ValueError(f"Unexpected stage output location: {folder}")
                for name in ("afp", "geo", "tex"):
                    loose = folder / name
                    if loose.is_dir():
                        if loose.resolve() != loose:
                            raise ValueError(f"Unexpected loose stage output location: {loose}")
                        shutil.rmtree(loose)
                if folder.name == "background":
                    for file in folder.glob("*.png"):
                        if file.is_file():
                            file.unlink()
