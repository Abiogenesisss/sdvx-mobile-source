"""Read AFP shape records preserved from original cabinet packages."""
import struct

from bemani.format.afp import Shape, SWF
from bemani.format.afp.swf import AP2Tag

class MenuSWF(SWF):
    def _SWF__parse_tag(self, ap2_version, afp_version, data, tag, size, offset, *args, **kwargs):
        # The new image-backed shape form appends the corresponding AP2 image ID.
        if tag == AP2Tag.AP2_SHAPE and size == 8 and struct.unpack_from('<H', data, offset)[0] == 6:
            size = 4
        result = super()._SWF__parse_tag(ap2_version, afp_version, data, tag, size, offset, *args, **kwargs)
        if self.name in {'shutter_default', 'parts_en', 'title_anime', 'title'} and tag == AP2Tag.AP2_PLACE_OBJECT:
            flags, = struct.unpack_from('<I', data, offset)
            if flags & 0x80000000:
                more, = struct.unpack_from('<I', data, offset + 8)
                if more & 8:
                    # Channel 0 is a four-point 2D mask in local twips.
                    # The optional 0x10 and 0x20 records occupy eight bytes each after alignment.
                    end = offset + size - 8 * (bool(more & 0x10) + bool(more & 0x20))
                    for point_format, mode in [('8i', 0x910d), ('8h', 0x910c)]:
                        fmt = '<IHH' + point_format
                        start = end - struct.calcsize(fmt)
                        if start < offset + 12:
                            continue
                        channel, mode_read, count, *xy = struct.unpack_from(fmt, data, start)
                        if (channel, mode_read, count) == (1, mode, 4):
                            result.mask_points = tuple((x / 20, y / 20) for x, y in zip(xy[::2], xy[1::2]))
                            break
                    else:
                        raise ValueError(f'Unsupported NABLA shutter mask at {offset:#x} in {self.name}')
        return result


def menu_shape(name, source):
    data = bytearray(source)
    if data[:4] == b'GE2D' and struct.unpack_from('>I', data, 16)[0] == 0x24:
        struct.pack_into('>I', data, 16, 4)
    return Shape(name, bytes(data))
