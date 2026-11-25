"""Extract original stage registration and asset references from 2026101300.

Requires capstone and pefile. Run from the SDVX MOBILE project root, or
pass --root. The output preserves native registration order and priorities.
"""

import argparse
import json
import struct
from pathlib import Path
from xml.etree import ElementTree

import capstone
from capstone import x86
import pefile


# 180482516 supplies this table. 180427a8e and 1804284d6 use it for AFP and Live2D.
PROFILE_RVAS = (0x8D4800, 0x8D47CC, 0x8D4808, 0x8D4804, 0x8D4810, 0x8D480C,
                0x8D4818, 0x8D4814, 0x8D4820, 0x8D481C, 0x8D4824)
STATIC_STACK = 0x400000000
REGISTRATION_RANGES = {"ordinary": (0x485EA0, 0x4A0406), "special": (0x4A0890, 0x4A43C7)}
ARCHIVE_REGISTRATION_RANGE = (0x5B6F10, 0x5BDD54)
VOLATILE = ("rax", "rcx", "rdx", "r8", "r9", "r10", "r11")
# 1804a464d–1804a46b6 resolves the song and chart overrides.
SPECIAL_ROUTES = (
    {"song_id": 610, "difficulty_id": 3, "chart": "exceed.json", "key": 0},
    {"song_id": 642, "difficulty_id": 3, "chart": "exceed.json", "key": 2},
    {"song_id": 271, "difficulty_id": 3, "chart": "exceed.json", "key": 3},
    {"song_id": 636, "difficulty_id": 5, "chart": "ultimate.json", "key": 4},
)


def parent_register(name):
    if name in ("sil", "dil", "bpl", "spl"):
        return "r" + name[:2]
    if name.startswith("xmm") or name == "rip":
        return name
    if name.startswith("r") and name[1:2].isdigit():
        return name.rstrip("dwb")
    if name.startswith("e"):
        return "r" + name[1:]
    if name.startswith("r"):
        return name
    if len(name) == 2 and name[-1] in "lhx":
        return "r" + name[0] + "x"
    return "r" + name


class Initializer:
    """Follow the straight-line record construction in the native initializers."""

    def __init__(self, dll, start, end):
        self.dll = dll
        self.base = dll.OPTIONAL_HEADER.ImageBase
        decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        decoder.detail = True
        self.instructions = decoder.disasm(dll.get_data(start, end - start), self.base + start)
        self.registers = {"rsp": STATIC_STACK}
        self.memory = {}
        self.strings = {}
        self.vectors = {}
        self.events = []
        self.rows = {}
        self.archive_paths = {}
        self.profile = [self.read(self.base + rva, 4) for rva in PROFILE_RVAS]

    def read(self, address, size):
        if address is None:
            return None
        if all(address + i in self.memory for i in range(size)):
            return int.from_bytes(bytes(self.memory[address + i] for i in range(size)), "little")
        if self.base <= address < self.base + self.dll.OPTIONAL_HEADER.SizeOfImage:
            data = self.dll.get_data(address - self.base, size)
            if len(data) == size:
                return int.from_bytes(data, "little")
        return None

    def write(self, address, size, value):
        if address is None:
            return
        for i in range(size):
            if value is None:
                self.memory.pop(address + i, None)
            else:
                self.memory[address + i] = value >> (8 * i) & 255

    def register(self, instruction, number):
        return parent_register(instruction.reg_name(number))

    def address(self, instruction, operand):
        mem = operand.mem
        if not mem.base:
            base = 0
        elif self.register(instruction, mem.base) == "rip":
            base = instruction.address + instruction.size
        else:
            base = self.registers.get(self.register(instruction, mem.base))
        index = self.registers.get(self.register(instruction, mem.index)) if mem.index else 0
        if base is not None and index is not None:
            return base + index * mem.scale + mem.disp
        return None

    def value(self, instruction, operand):
        if operand.type == x86.X86_OP_IMM:
            return operand.imm
        if operand.type == x86.X86_OP_REG:
            value = self.registers.get(self.register(instruction, operand.reg))
            return None if value is None else value & ((1 << (operand.size * 8)) - 1)
        if operand.type == x86.X86_OP_MEM:
            return self.read(self.address(instruction, operand), operand.size)
        return None

    def store(self, instruction, operand, value):
        if operand.type == x86.X86_OP_REG:
            name = self.register(instruction, operand.reg)
            if operand.size < 4 and value is not None and name in self.registers:
                mask = (1 << (8 * operand.size)) - 1
                prior = self.registers[name]
                value = value if prior is None else ((prior & ~mask) | (value & mask))
            if value is not None:
                value &= (1 << (8 * max(4, operand.size))) - 1
            self.registers[name] = value
        elif operand.type == x86.X86_OP_MEM:
            self.write(self.address(instruction, operand), operand.size, value)

    def string(self, address):
        if address is None:
            return None
        if address in self.strings:
            return self.strings[address]
        if self.base <= address < self.base + self.dll.OPTIONAL_HEADER.SizeOfImage:
            return self.dll.get_string_at_rva(address - self.base, 256).decode("ascii", "replace")
        return None

    def integer(self, address):
        value = self.read(address, 4)
        return value if value is None or value < 0x80000000 else value - 0x100000000

    def scalar(self, address):
        value = self.read(address, 4)
        return None if value is None else struct.unpack("<f", value.to_bytes(4, "little"))[0]

    def vector_span(self, ptr):
        begin, end = self.read(ptr, 8), self.read(ptr + 8, 8)
        return (begin, end) if begin is not None and end is not None else None

    def vector_rows(self, offset, stride):
        entry = self.vectors.get(offset)
        if entry is None:
            return []
        (_target, begin, end, _call) = entry
        if (end - begin) % stride or not 0 <= end - begin <= 256 * stride:
            raise ValueError((hex(offset), hex(begin), hex(end), hex(stride)))
        return range(begin, end, stride)

    def priority(self, index):
        return self.profile[index] if index is not None and 0 <= index < len(self.profile) else None

    def snapshot(self, address, cfg):
        row = {"registration": hex(address)}
        base_offset = cfg + 0x18
        base_archive = self.integer(base_offset)
        if base_archive is None:
            raise ValueError(f"Unresolved base archive at {address:#x}")
        # 1804c06a0 assigns both the base AFP and 3D tunnel to bucket 64.
        row["base"] = {
            "archive_id": base_archive,
            "scale": self.scalar(base_offset + 4),
            "flags": self.read(base_offset + 0xC, 2) or 0,
            "clip": self.string(base_offset + 0xD0) if base_archive >= 0 else None,
            "priority": 64 if base_archive >= 0 else None,
            "gauge_children": [self.string(child) for child in self.vector_rows(base_offset + 0x78, 0x20)],
        }
        row["tunnels"] = [
            {"preset": self.integer(p + 0x20), "name": self.string(p),
             "priority": 64, "angle": self.scalar(p + 0x24), "density": self.scalar(p + 0x28),
             "params": [self.scalar(p + 0x2C), self.scalar(p + 0x30)]}
            for p in self.vector_rows(cfg, 0x38)
        ]
        row["afp"] = []
        for p in self.vector_rows(cfg + 0x178, 0x130):
            index = self.integer(p + 0x2C)
            row["afp"].append({"archive_id": self.integer(p), "clip": self.string(p + 8),
                               "mode": self.integer(p + 0x28), "profile_index": index,
                               "priority": self.priority(index), "scale": self.scalar(p + 4),
                               "gauge_children": [self.string(child) for child in self.vector_rows(p + 0x38, 0x20)],
                               "flags": self.read(p + 0x30, 2)})
        row["extra_archive_ids"] = [self.integer(p) for p in self.vector_rows(cfg + 0x1A8, 4)]
        row["live2d"] = [
            {"id": self.integer(p), "profile_index": self.integer(p + 4),
             "priority": self.priority(self.integer(p + 4))}
            for p in self.vector_rows(cfg + 0x190, 0x14)
        ]
        row["post"] = []
        for p in self.vector_rows(cfg + 0x1C0, 0x40):
            # 18038666b passes the half-open priority range to the capture dispatcher.
            low, high = self.integer(p + 0x38), self.integer(p + 0x3C)
            explicit_priority = self.read(p + 0x34, 1)
            original_output = self.integer(p + 0x30) if explicit_priority else None
            captures = []
            for entry in self.vector_rows(p + 0x18, 0x50):
                captures.append({"movie": self.string(entry),
                                 "raw_targets": [self.string(q) for q in self.vector_rows(entry + 0x20, 0x20)],
                                 "effect_targets": [{"target": self.string(q), "stage": self.integer(q + 0x20)}
                                                    for q in self.vector_rows(entry + 0x38, 0x28)]})
            row["post"].append({
                "types": [self.integer(q) for q in self.vector_rows(p, 4)],
                "captures": captures,
                "input_priorities": [low, high],
                "processing_priority": low,
                "present_priority": low if original_output is None else original_output,
                "explicit_present_priority": original_output,
                "flags": explicit_priority,
            })
        row["initialization_calls"] = self.events
        self.events = []
        return row

    def call(self, instruction, target):
        args = self.registers
        dst, src, end = args.get("rcx"), args.get("rdx"), args.get("r8")
        result = None
        if target == 0x0F27F0 and dst is not None:
            self.write(dst, 8, src)
            self.write(dst + 8, 8, end)
            result = dst
        elif target in (0x0BCA40, 0x0BCA00) and dst is not None:
            self.strings[dst] = self.string(src)
            result = dst
        elif target == 0x242BF0 and dst is not None:
            self.write(dst, 4, self.read(src, 4))
            self.write(dst + 4, 4, self.read(end, 4))
            result = dst
        elif target == 0x0EA840 and dst is not None:
            self.write(dst, 4, self.read(src, 4))
            self.write(dst + 4, 1, 1)
            result = dst
        elif target == 0x0B8890 and dst is not None:
            self.write(dst, 8, 0)
            result = dst
        elif target == 0x14A1E0 and dst is not None and src is not None:
            key = self.integer(dst - 8)
            if key is None or key in self.rows:
                raise ValueError((hex(instruction.address), key, src))
            self.rows[key] = self.snapshot(instruction.address, src)
            result = dst
        elif target in (0x169070, 0x139620, 0x4A5240, 0x4A5270, 0x4A52A0,
                        0x4A52D0, 0x4A5300, 0x4A5330, 0x4A5360, 0x484AC0) and dst is not None and src is not None:
            span = self.vector_span(src)
            if span:
                self.vectors[dst] = (target, *span, instruction.address)
            self.events.append((hex(instruction.address), hex(target), hex(dst - STATIC_STACK),
                                [hex(p - STATIC_STACK) for p in span] if span else None))
            result = dst
        elif target == 0x4A5500:
            key = self.integer(src)
            if key is None or end is None or key in self.rows:
                raise ValueError((hex(instruction.address), key, end))
            self.rows[key] = self.snapshot(instruction.address, end)
            result = dst
        elif target == 0x5BE2D0:
            first, last = self.vector_span(src)
            self.archive_paths = {self.integer(p): self.string(self.read(p + 8, 8))
                                  for p in range(first, last, 0x20)}
        elif target == 0x7C5460:
            result = args.get("rax")
        elif target in (0x176140, 0x2727B0, 0x2727D0, 0x2727E0) and dst is not None:
            result = dst
        for register in VOLATILE:
            args.pop(register, None)
        args["rax"] = result

    def decode(self):
        for instruction in self.instructions:
            op = instruction.operands
            mnemonic = instruction.mnemonic
            if mnemonic in ("mov", "movabs", "movzx", "movss", "movsd", "movaps", "movups", "movdqa", "movdqu"):
                self.store(instruction, op[0], self.value(instruction, op[1]))
            elif mnemonic == "lea":
                self.store(instruction, op[0], self.address(instruction, op[1]))
            elif mnemonic in ("xor", "xorps") and op[0].reg == op[1].reg:
                self.store(instruction, op[0], 0)
            elif mnemonic == "sub":
                left, right = (self.value(instruction, item) for item in op)
                self.store(instruction, op[0], None if left is None or right is None else left - right)
            elif mnemonic == "push":
                self.registers["rsp"] -= 8
            elif mnemonic == "call":
                self.call(instruction, op[0].imm - self.base)
        return self.rows


def compact_rows(rows):
    return {key: {name: value for name, value in record.items() if name != "initialization_calls"}
            for key, record in sorted(rows.items())}


def compact_json(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def write_runtime_include(path, registrations, alternates):
    rows = compact_rows(registrations)
    specials = compact_rows(alternates)
    # Put one record per line for review. Whitespace between JSON values is insignificant.
    ordinary_lines = ",\n".join(f'"{key}":{compact_json(row)}' for key, row in rows.items())
    special_lines = ",\n".join(f'"{key}":{compact_json(row)}' for key, row in specials.items())
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write("// Generated by tools/extract-stage-registration.py from 2026101300 soundvoltex.dll.\n")
        output.write(f'R"SDVX_STAGE({{"special_routes":{compact_json(SPECIAL_ROUTES)},"ordinary":{{\n')
        output.write(ordinary_lines)
        output.write('\n},"special":{\n')
        output.write(special_lines)
        output.write('\n}})SDVX_STAGE"\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compact", type=Path, help="Write the compact bundled registration index")
    parser.add_argument("--runtime-inc", type=Path, help="Write the C++ include for all registered stages")
    parser.add_argument("--contents", type=Path, help="Local HDD contents directory")
    args = parser.parse_args()
    root = args.root
    contents = args.contents or root / "2026101300/LAZY_KFC/contents"
    dll = pefile.PE(str(contents / "modules/soundvoltex.dll"))
    library = root / "development/android-native/src/main/assets/songs/library.json"
    music_db = ElementTree.fromstring((contents / "data/others/music_db.xml").read_text(encoding="cp932"))
    source_backgrounds = {int(x.attrib["id"]): int(x.findtext("info/bg_no")) for x in music_db.findall("music")}
    songs = [
        {"id": song["id"], "title": song["title"], "bg_no": source_backgrounds[song["id"]],
         "bundled_difficulties": [c["chart"] for c in song["charts"] if c.get("bundled")]}
        for song in json.loads(library.read_text(encoding="utf-8")) if any(c.get("bundled") for c in song["charts"])
    ]
    ordinary = Initializer(dll, *REGISTRATION_RANGES["ordinary"])
    special = Initializer(dll, *REGISTRATION_RANGES["special"])
    archives = Initializer(dll, *ARCHIVE_REGISTRATION_RANGE)
    archives.decode()
    registrations = ordinary.decode()
    alternates = special.decode()
    if set(registrations) != set(range(123)) or set(alternates) != {0, 1, 2, 3, 4, 6} or any(song["bg_no"] not in registrations for song in songs):
        raise ValueError("Missing original registration")
    for row in (*registrations.values(), *alternates.values()):
        for layer in (row["base"], *row["afp"]):
            archive_id = layer["archive_id"]
            layer["archive_path"] = archives.archive_paths[archive_id] if archive_id >= 0 else None
        row["extra_archive_paths"] = [archives.archive_paths[archive_id] for archive_id in row["extra_archive_ids"]]
    result = {"version": "2026101300", "priority_profile": ordinary.profile, "songs": songs,
              "special_routes": SPECIAL_ROUTES,
              "ordinary": registrations, "special": alternates}
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if args.compact:
        by_background = {song["bg_no"] for song in songs}
        bundled = {**result,
                   "ordinary": compact_rows({key: registrations[key] for key in by_background}),
                   "special": compact_rows(alternates)}
        args.compact.write_text(compact_json(bundled) + "\n", encoding="utf-8")
    if args.runtime_inc:
        write_runtime_include(args.runtime_inc, registrations, alternates)


if __name__ == "__main__":
    main()
