"""Preview the AP2 draw stream used by the Android port, without building an APK."""
import argparse
from functools import lru_cache
from importlib import import_module
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw


def original_frames(folder, animation, label, start, end):
    from bemani.format.afp import AFPRenderer, Color, Matrix
    import bemani.format.afp.geo as geometry

    library = folder / "afp-library.json"
    if library.is_file():
        entry = json.loads(library.read_text(encoding="utf-8"))[animation]
        clips = entry["clips"]
        clip = clips[label or next(iter(clips))]
        animation = entry.get("source", animation)
    else:
        scene = json.loads((folder / "afp-scene.json").read_text(encoding="utf-8"))
        entry = next(item for item in scene["movies"] if item["name"] == animation)
        clip = {"start": label or entry.get("label", 0)}

    source = import_module("compile-stage-background")
    shape = import_module("compile-menu").menu_shape
    geometry.descramble_text = source.geometry_name
    renderer = AFPRenderer(single_threaded=True)
    renderer._AFPRenderer__stubbed_swfs.update({"asdlib.asdlib", "asdlib.__Packages.asdlib"})
    for path in (folder / "afp/bsi").iterdir():
        movie = source.BackgroundSWF(path.name, (path.parent.parent / path.name).read_bytes(), path.read_bytes())
        renderer.add_swf(path.name, movie)
    for path in (folder / "geo").iterdir():
        renderer.add_shape(path.name, shape(path.name, path.read_bytes()))
    for path in (folder / "tex").glob("*.png"):
        renderer.add_texture(path.stem, texture(path))

    movie = renderer.swfs[animation]
    first = clip["start"] if isinstance(clip["start"], int) else movie.labels[clip["start"]]
    scale = Matrix.identity()
    scale.a11 = scale.a22 = .5
    frames = list(renderer.render_path(movie.exported_name, background_color=Color(0, 0, 0, 1),
                                       only_frames=list(range(first + start, first + end + 1)),
                                       movie_transform=scale,
                                       overridden_width=round(movie.location.width / 2),
                                       overridden_height=round(movie.location.height / 2)))
    if not frames:
        raise ValueError("No preview frame was produced for the requested range")
    return frames, movie.fps


@lru_cache
def texture(path):
    with Image.open(path) as image:
        return image.convert("RGBA")


def render(folder, commands, viewport, scale):
    import numpy

    size = tuple(round(value * scale) for value in viewport)
    background = Image.new("RGB", size)
    black = Image.new("RGB", size)
    for command in commands:
        source = texture(folder / (command["texture"] + ".png"))
        m = command["matrix"]
        transform = numpy.array([
            [m["a"] * scale, m["c"] * scale, m["tx"] * scale],
            [m["b"] * scale, m["d"] * scale, m["ty"] * scale],
            [m.get("p0", 0), m.get("p1", 0), 1],
        ])
        masks = list(command.get("masks", ()))
        if "source_rect" in command or "geometry" in command:
            left, top, width, height = command.get("source_rect", (0,0,*source.size))
            gx, gy, gw, gh = command.get("geometry", (0,0,width,height))
            boundary = []
            for x, y in ((gx,gy),(gx+gw,gy),(gx+gw,gy+gh),(gx,gy+gh)):
                p = transform @ numpy.array([x, y, 1])
                boundary.append((p[0] / p[2] / scale, p[1] / p[2] / scale))
            masks.append(boundary)
            transform = transform @ numpy.array([[gw/width,0,gx-left*gw/width],
                                                  [0,gh/height,gy-top*gh/height],[0,0,1]])
        if abs(numpy.linalg.det(transform)) < 1e-12:
            continue
        inverse = numpy.linalg.inv(transform).ravel()
        coeff = inverse[:8] / inverse[8]
        color = []
        for channel, name in zip(source.split(), "rgba"):
            multiply = command["multiply"][name]
            add = command["add"][name] * 255
            color.append(channel.point([max(0, min(255, round(x * multiply + add))) for x in range(256)]))
        layer = Image.merge("RGBA", color).transform(size, Image.Transform.PERSPECTIVE, coeff,
                                                    resample=Image.Resampling.NEAREST if command.get("filter") == "nearest"
                                                    else Image.Resampling.BILINEAR)
        alpha = layer.getchannel("A")
        for polygon in masks:
            mask = Image.new("L", size)
            ImageDraw.Draw(mask).polygon([(x * scale, y * scale) for x, y in polygon], fill=255)
            alpha = ImageChops.multiply(alpha, mask)
        pixels = layer.convert("RGB")
        if command["blend"] == 8:
            background = ImageChops.add(background, Image.composite(pixels, black, alpha))
        elif command["blend"] == 3:
            background = Image.composite(ImageChops.multiply(background, pixels), background, alpha)
        else:
            background.paste(pixels, mask=alpha)
    return background


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("animation")
    parser.add_argument("output", type=Path)
    parser.add_argument("--clip")
    parser.add_argument("--frames", default="1", help="One-based frame or inclusive range such as 1-30")
    args = parser.parse_args()
    first, _, last = args.frames.partition("-")
    start, end = int(first), int(last or first)
    if not 1 <= start <= end:
        parser.error("Frame range must be positive and increasing")
    if any((args.folder / entry).is_file() for entry in ("afp-library.json", "afp-scene.json")):
        images, fps = original_frames(args.folder, args.animation, args.clip, start, end)
        images[0].save(args.output, save_all=True, append_images=images[1:],
                       duration=[round((i + 1) * 1000 / fps) - round(i * 1000 / fps) for i in range(len(images))],
                       loop=0, quality=90)
        print(args.output)
        return
    path = args.folder / "timeline.json"
    if not path.is_file():
        path = args.folder / f"timeline-{args.animation}.json"
    animations = json.loads(path.read_text(encoding="utf-8"))
    if args.animation not in animations:
        parser.error(f"Available animations: {', '.join(animations)}")
    animation = animations[args.animation]
    labels = animation["clips"]
    label = args.clip or next(iter(labels))
    if label not in labels:
        parser.error(f"Available clips: {', '.join(labels)}")
    clip = labels[label]
    if "layers" in clip:
        if not 1 <= start <= end:
            parser.error("Frame range must be positive and increasing")
        frames = []
        for index in range(start - 1, end):
            commands = []
            for layer in clip["layers"]:
                count, repeat = len(layer["frames"]), layer["repeat_from"]
                frame = index if index < count else repeat + (index - repeat) % (count - repeat)
                commands.extend(layer["frames"][frame])
            frames.append(commands)
    else:
        frames = clip["frames"]
        if not 1 <= start <= end <= len(frames):
            parser.error(f"Frame range must stay within 1-{len(frames)}")
        frames = frames[start - 1:end]
    images = [render(args.folder, frame, animation["viewport"], 0.5) for frame in frames]
    images[0].save(args.output, save_all=True, append_images=images[1:],
                   duration=[round((i+1)*1000/animation["fps"])-round(i*1000/animation["fps"]) for i in range(len(images))],
                   loop=0, quality=90)
    print(args.output)


if __name__ == "__main__":
    main()
