"""Copy original AFP resources and extract the textures needed by their GE2D shapes."""

import argparse
from importlib import import_module
import json
import shutil
import struct
from pathlib import Path
from xml.etree import ElementTree

from bemani.format.afp import Shape
from bemani.format.afp.swf import AP2DefineSpriteTag, AP2ImageTag, AP2PlaceObjectTag, AP2ShapeTag
import bemani.format.afp.geo as geometry


def prepare(archive, output, scene, textures=(), extra_afp=None, publish=None):
    publish = publish or copy_asset
    afp = import_module("compile-stage-background")
    extra_afp = extra_afp or {}
    archives = (*dict.fromkeys(extra_afp.values()), archive)
    texture_sources = {
        source.stem: source for directory in (*textures, *(folder / "tex" for folder in archives))
        for source in directory.glob("*.png")
    }
    used_textures = set()
    bitmap_fills = set()
    movies = {}
    shape_sources = {}
    geometry.descramble_text = afp.geometry_name

    def timeline_tags(timeline):
        for tag in timeline.tags:
            yield tag
            if isinstance(tag, AP2DefineSpriteTag):
                yield from timeline_tags(tag)

    output.mkdir(parents=True, exist_ok=True)

    def copy_movie(name, folder):
        if name in movies:
            return movies[name]
        movie = afp.BackgroundSWF(name, (folder / "afp" / name).read_bytes(), (folder / "afp/bsi" / name).read_bytes())
        movie.parse()
        definitions = {tag.id: tag for tag in timeline_tags(movie) if tag.id is not None}
        dependencies = {}
        movies[name] = (movie, definitions, dependencies)
        for directory in ("afp", "afp/bsi"):
            publish(folder / directory / name, output / directory / name)
        for tag in definitions.values():
            if isinstance(tag, AP2ShapeTag):
                source = folder / "geo" / tag.reference
                shape_sources[source.name] = source
                publish(source, output / "geo" / source.name)
        for tag_id, reference in movie.imported_tags.items():
            dependency = reference.swf
            # Global ActionScript packages supply methods, with no drawable assets.
            if dependency.split(".", 1)[0] in ("aeplib", "asdlib"):
                continue
            if dependency == name:
                imported = movie
            else:
                dependency_folder = next((candidate for candidate in (folder, *archives)
                                          if (candidate / "afp" / dependency).is_file()), None)
                if dependency_folder is None:
                    raise FileNotFoundError(f"{folder.name}/{name} imports missing AFP {dependency}")
                imported, _, _ = copy_movie(dependency, dependency_folder)
            dependencies[tag_id] = (dependency, imported.exported_tags[reference.tag])
        return movies[name]

    visited = set()

    def collect(name, timeline):
        _, definitions, dependencies = movies[name]
        placed = set()
        for placement in timeline.tags:
            if not isinstance(placement, AP2PlaceObjectTag):
                continue
            if placement.source_tag_id is not None:
                placed.add(placement.source_tag_id)
            if (bitmap := getattr(placement, "bitmap_image", None)) is not None:
                placed.add(bitmap)
                owner, tag_id = dependencies.get(bitmap, (name, bitmap))
                bitmap_fills.add(movies[owner][1][tag_id].reference)
        for tag_id in sorted(placed):
            owner, tag_id = dependencies.get(tag_id, (name, tag_id))
            if (owner, tag_id) in visited:
                continue
            visited.add((owner, tag_id))
            tag = movies[owner][1].get(tag_id)
            if isinstance(tag, AP2DefineSpriteTag):
                collect(owner, tag)
            elif isinstance(tag, AP2ImageTag):
                used_textures.add(tag.reference)
            elif isinstance(tag, AP2ShapeTag):
                data = bytearray(shape_sources[tag.reference].read_bytes())
                if data[:4] == b"GE2D" and struct.unpack_from(">I", data, 16)[0] == 0x24:
                    struct.pack_into(">I", data, 16, 4)
                shape = Shape(tag.reference, bytes(data))
                shape.parse()
                for draw in shape.draw_params:
                    if not draw.flags & 1:
                        break
                    if draw.flags & 2:
                        used_textures.add(draw.region)
    for name in sorted({entry["name"] for entry in scene["movies"] if "folder" not in entry}):
        movie, _, _ = copy_movie(name, extra_afp.get(name, archive))
        collect(name, movie)
    for name in sorted(used_textures):
        if name not in texture_sources:
            raise FileNotFoundError(f"{archive.name} references missing AFP texture {name!r}")
        source = texture_sources[name]
        publish(source, output / "tex" / source.name)
    bitmap_regions = {}
    for directory in sorted({texture_sources[name].parent for name in bitmap_fills}, key=str):
        index = directory / "texturelist.xml"
        if not index.is_file():
            continue
        for atlas in ElementTree.fromstring(index.read_bytes()).findall("texture"):
            for entry in atlas.findall("image"):
                name = entry.attrib["name"]
                if name not in bitmap_fills or texture_sources[name].parent != directory:
                    continue
                img = [int(v) // 2 for v in entry.findtext("imgrect").split()]
                uv = [int(v) / 2 for v in entry.findtext("uvrect").split()]
                bitmap_regions[name] = {
                    "source_rect": [uv[0] - img[0], uv[2] - img[2], uv[1] - uv[0], uv[3] - uv[2]],
                    "nearest": atlas.attrib["mag_filter"] == "nearest",
                }
    if bitmap_regions:
        (output / "afp-textures.json").write_text(json.dumps(bitmap_regions, sort_keys=True, separators=(",", ":")), encoding="utf-8")
    (output / "afp-scene.json").write_text(json.dumps(scene, separators=(",", ":")), encoding="utf-8")


def copy_asset(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="Extracted original IFS directory")
    parser.add_argument("output", type=Path, help="Android scene asset directory")
    parser.add_argument("scene", type=Path, help="Scene AFP movie and VOX routing data")
    parser.add_argument("--textures", type=Path, action="append", default=[])
    args = parser.parse_args()
    prepare(args.archive, args.output, json.loads(args.scene.read_text(encoding="utf-8")), args.textures)


if __name__ == "__main__":
    main()
