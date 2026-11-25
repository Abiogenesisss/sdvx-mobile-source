"""Read original stage AFP with its extended placement and shape records."""
import struct

from bemani.format.afp import SWF
from bemani.format.afp.swf import AP2Tag, Frame

class BackgroundSWF(SWF):
    def _SWF__parse_tags(self, ap2_version, afp_version, data, offset, sprite, prefix=""):
        # Planet uses a 2.57 MiB DefineSprite. AP2 allocates 22 bits for tag sizes.
        _, label_count, frame_count, tag_count, label_offset, frame_offset, tag_offset = struct.unpack_from(
            "<HHIIIII", data, offset)
        frames = []
        tag_to_frame = {}
        for index in range(frame_count):
            frame_at = offset + frame_offset + index * 4
            packed, = struct.unpack_from("<I", data, frame_at)
            self.add_coverage(frame_at, 4)
            first, count = packed & 0xfffff, packed >> 20
            if first + count > tag_count:
                raise ValueError("AFP frame outside tag table")
            frames.append(Frame(first, count))
            for tag in range(first, first + count):
                tag_to_frame[tag] = f"frame_{index}"
        tags = []
        tag_at = offset + tag_offset
        for index in range(tag_count):
            packed, = struct.unpack_from("<I", data, tag_at)
            self.add_coverage(tag_at, 4)
            size = packed & 0x3fffff
            if tag_at + 4 + size > len(data):
                raise ValueError("Truncated AFP tag")
            tags.append(self._SWF__parse_tag(ap2_version, afp_version, data, packed >> 22, size, tag_at + 4,
                                           sprite, tag_to_frame.get(index, "orphan"), prefix=prefix))
            tag_at += 4 + ((size + 3) & ~3)
        labels = {}
        for index in range(label_count):
            label_at = offset + label_offset + index * 4
            frame, name = struct.unpack_from("<HH", data, label_at)
            self.add_coverage(label_at, 4)
            labels[self._SWF__get_string(name)] = frame
        return tags, frames, labels

    def _SWF__parse_tag(self, ap2_version, afp_version, data, tag, size, offset, *args, **kwargs):
        if tag == AP2Tag.AP2_SHAPE and size == 8:
            flags, = struct.unpack_from("<H", data, offset)
            if flags != 6:
                raise ValueError(f"Unexpected extended AFP shape flags {flags:#x}")
            size = 4
        if tag == AP2Tag.AP2_PLACE_OBJECT and size >= 12 and data[offset + 3] & 0x80 and data[offset + 8] & 1:
            # This extended flag carries no additional fields.
            # Keep all extended transform and filter data for the normal parser.
            data = data[:offset + 8] + bytes((data[offset + 8] & ~1,)) + data[offset + 9:]
        parsed = super()._SWF__parse_tag(ap2_version, afp_version, data, tag, size, offset, *args, **kwargs)
        if tag == AP2Tag.AP2_PLACE_OBJECT and size >= 20 and data[offset + 3] & 0x80 and data[offset + 8] & 0x20:
            # The bitmap symbol precedes its dimensions and the final two alignment bytes.
            parsed.bitmap_image, = struct.unpack_from("<H", data, offset + size - 8)
        return parsed


def geometry_name(raw, obfuscated):
    if obfuscated and not raw[:1].isascii():
        raw = bytes(byte ^ 0x80 for byte in raw)
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("cp932", errors="replace")
