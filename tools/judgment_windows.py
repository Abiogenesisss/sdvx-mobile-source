"""Timing-window layout from the 2026101300 HDD judgment initializers."""
import struct


# (DLL file offset of binary32 seconds, result ID, nominal boundary in ms).
# None is the explicit on-time boundary inserted by the initializer.
CHIP = ((10079660, 1, -233), (10079648, 2, -133), (10079616, 3, -42),
        (10079612, 4, -21), (None, 6, 0), (10076992, 7, 21),
        (10077060, 8, 42), (10077180, 10, 133))
NARROW = ((10079596, 5, -17), (10076976, 10, 17))
WINDOWS = {
    "chip": CHIP,
    "fx_chip_special": ((10079660, 0, -233), (10079648, 3, -133), *CHIP[3:]),
    "hold_wide": ((10079648, 5, -133), (10077132, 10, 100)),
    "hold_narrow": NARROW,
    "laser_track": NARROW,
    "laser_slam": ((10079620, 5, -50), (10077064, 10, 50)),
}


def validate_judgment_tables(tables):
    for name, layout in WINDOWS.items():
        rows = tables.get(name) if isinstance(tables, dict) else None
        if not isinstance(rows, list) or len(rows) != len(layout):
            raise ValueError(f"Invalid judgment table {name}: expected {len(layout)} boundaries")
        previous = float("-inf")
        for i, (row, (offset, result, nominal_ms)) in enumerate(zip(rows, layout)):
            where = f"{name}[{i}]" + (f" (DLL offset {offset:#x})" if offset is not None else "")
            if not isinstance(row, dict) or type(row.get("result_id")) is not int or row["result_id"] != result:
                raise ValueError(f"Invalid judgment result at {where}: expected ID {result}")
            seconds = row.get("from_seconds")
            # Keep the original binary32 value. One millisecond accommodates the
            # rounded reference values; the explicit zero must remain exact.
            tolerance_ms = 0 if offset is None else 1
            lower = (nominal_ms - tolerance_ms) / 1000
            upper = (nominal_ms + tolerance_ms) / 1000
            if type(seconds) not in (float, int) or not (lower <= seconds <= upper and previous < seconds):
                raise ValueError(f"Invalid judgment window at {where}: got {seconds!r} seconds, "
                                 f"expected {nominal_ms} +/- {tolerance_ms} ms")
            previous = seconds


def read_judgment_tables(dll):
    tables = {}
    for name, layout in WINDOWS.items():
        rows = []
        for offset, result, _ in layout:
            word = dll[offset:offset+4] if offset is not None else bytes(4)
            if len(word) != 4:
                raise ValueError(f"Missing {name} judgment window in soundvoltex.dll at {offset}")
            rows.append({"from_seconds": struct.unpack("<f", word)[0], "result_id": result,
                         "float_bits": f"0x{int.from_bytes(word, 'little'):08x}"})
        tables[name] = rows
    validate_judgment_tables(tables)
    return tables
