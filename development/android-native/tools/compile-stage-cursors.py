"""Export the neutral VOL-L and VOL-R cursor sprites from ver06/game_effect.ifs."""
import argparse
from io import BytesIO
from pathlib import Path
import struct

from PIL import Image
from bemani.format import IFS
from bemani.format.afp import AFPRenderer, Color, Shape, SWF
from bemani.format.afp.blend import blend
import bemani.format.afp.render as engine


def source_over(destination, source):
    sa = source[3] / 255
    remainder = destination[3] / 255 * (1 - sa)
    alpha = sa + remainder
    if alpha == 0:
        return destination
    rgb = tuple(round((s * sa + d * remainder) / alpha) for s, d in zip(source[:3], destination[:3]))
    return (*rgb, round(255 * alpha))


def additive_over_transparency(destination, source):
    da = destination[3] / 255
    sa = source[3] / 255
    premultiplied = tuple(min(255, d * da + s * sa) for d, s in zip(destination[:3], source[:3]))
    # An additive black texel contributes no color. For the exported sprite,
    # derive glow coverage from its actual RGB contribution.
    alpha = max(da, max(premultiplied) / 255)
    if alpha == 0:
        return destination
    return (*(round(c / alpha) for c in premultiplied), round(255 * alpha))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    archive = IFS(args.archive.read_bytes(), decode_binxml=True, decode_textures=True)
    renderer = AFPRenderer(shapes={}, textures={}, swfs={}, single_threaded=True)
    # The upstream Python blender's normal branch clamps every nonzero alpha
    # to 255. Preserve source-over alpha for the standalone Android sprites.
    blend.blend_normal = source_over
    blend.blend_addition = additive_over_transparency
    engine.affine_composite = blend.affine_composite
    engine.perspective_composite = blend.perspective_composite
    compositions = {"vol_cursor_wlz_360": "vol_cursor_left", "vol_cursor_wrz_360": "vol_cursor_right"}
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    for name, stem in compositions.items():
        afp = f"afp\\{name}"
        renderer.add_swf(name, SWF(name, archive.read_file(afp), archive.read_file(f"afp\\bsi\\{name}")))
        placeholders = set()
        for path in archive.filenames:
            if not path.startswith(f"geo\\{name}_shape"):
                continue
            source = bytearray(archive.read_file(path))
            if struct.unpack_from(">I", source, 16)[0] == 0x24:
                struct.pack_into(">I", source, 16, 4)
            shape_name = path.split("\\")[-1]
            shape = Shape(shape_name, bytes(source))
            renderer.add_shape(shape_name, shape)
            for draw in shape.draw_params:
                if draw.region is None:
                    placeholders.add(f"{shape_name}, untextured")
                elif draw.region not in renderer.textures:
                    data = archive.read_file(f"tex\\{draw.region}.png")
                    with Image.open(BytesIO(data)) as texture:
                        renderer.add_texture(draw.region, texture)

        # Untyped rectangle shapes are AFP render target placeholders. The
        # cursor body uses the textured geometry only, without the FX target.
        original = renderer._AFPRenderer__render_object

        def render(image, obj, *args, **kwargs):
            if obj.source.reference in placeholders:
                return image
            return original(image, obj, *args, **kwargs)

        renderer._AFPRenderer__render_object = render
        try:
            (frame,) = renderer.render_path(name, background_color=Color(0, 0, 0, 0), only_frames=[1])
        finally:
            renderer._AFPRenderer__render_object = original
        bbox = frame.getbbox()
        if bbox is None or bbox[3] - bbox[1] == frame.height:
            raise ValueError(f"Unexpected cursor bounds {name}: {bbox}")
        frame.crop(bbox).save(output / f"{stem}.png")
        print(f"{name} -> {stem}.png {bbox}")


if __name__ == "__main__":
    main()
