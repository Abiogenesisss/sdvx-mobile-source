"""Read the 2026101300 crew, Live2D, and voice registry from a local HDD DLL."""

import argparse
from importlib import import_module
import json
import math
from pathlib import Path
import struct
import sys

from capstone import x86
import pefile

layout = import_module("extract-stage-registration")


class Registry(layout.Initializer):
    def __init__(self, dll, start, end):
        super().__init__(dll, start, end)
        self.record = None

    def text(self, pointer):
        if not pointer:
            return ""
        return self.dll.get_string_at_rva(pointer - self.base, 512).decode("cp932")

    def value(self, instruction, operand):
        # These three placement loads leave lane zero for the following MOVSS.
        if instruction.address - self.base in (0x68E10, 0x68E25, 0x68E3E):
            if operand.type == x86.X86_OP_MEM:
                upper = self.read(self.address(instruction, operand) + 4, 12)
                return upper << 32
        return super().value(instruction, operand)

    def store(self, instruction, operand, value):
        if (instruction.mnemonic == "movss" and operand.type == x86.X86_OP_REG
                and instruction.operands[1].type == x86.X86_OP_REG):
            # Register-to-register MOVSS replaces just the lowest 32 bits.
            name = self.register(instruction, operand.reg)
            self.registers[name] = (self.registers[name] & ~0xffffffff) | value
            return
        super().store(instruction, operand, value)

    def call(self, instruction, target):
        regs = self.registers
        dst, src, end = (regs.get(name) for name in ("rcx", "rdx", "r8"))
        stack = regs["rsp"]

        def arg(offset, size=8):
            return self.read(stack + offset, size)

        result = None
        if target in (0x5D9720, 0x5D9630):
            record = end if target == 0x5D9630 else src
            header = [self.read(record + offset, 4) for offset in range(0, 24, 4)]
            strings = [self.read(record + offset, 8) for offset in range(0x18, 0x50, 8)]
            self.record = (header, strings, [], 0, None)
        elif target == 0x5D84F0:
            constructor = layout.Initializer(self.dll, 0x5D84F0, 0x5D8576)
            constructor.decode()
            frame = constructor.registers["rbp"]
            header = [constructor.read(frame - 0x49 + offset, 4) for offset in range(0, 24, 4)]
            strings = [constructor.read(frame - 0x31 + offset, 8) for offset in range(0, 56, 8)]
            self.record = (header, strings, [], 0, None)
        elif target == 0x5D7FF0:
            self.record = ([end, regs["r9"], arg(0x20, 4), 1, arg(0x28, 4), arg(0x60, 4)],
                           [arg(p) if p is not None else 0
                            for p in (0x50, 0x58, 0x30, None, 0x40, 0x48, None)],
                           [], 0, None)
        elif target == 0x5D8270:
            bank = arg(0x50)
            self.record = ([end, regs["r9"], arg(0x20, 4), 1, 1, 0],
                           [0, 0, arg(0x28), arg(0x30), arg(0x38), arg(0x40), arg(0x60)],
                           self.vectors.get(arg(0x48), []), 0,
                           bank & 0xffffffff if bank is not None and bank >> 32 & 0xff else None)
        elif target == 0x5D83C0:
            self.record = ([end, 0, 0, 0, 0, 0], [0] * 7, self.vectors[arg(0x20)], 0, None)
        elif target == 0x5D8150:
            self.record = ([end, 0, 0, 2, arg(0x20, 4), 0],
                           [0, 0, arg(0x28), 0, arg(0x38), 0, 0], [], 0, None)
        elif target == 0x5D8680:
            constructor = layout.Initializer(self.dll, 0x5D8680, 0x5D8713)
            constructor.decode()
            frame = constructor.registers["rbp"]
            header = [constructor.read(frame - 0x51 + offset, 4) for offset in range(0, 24, 4)]
            strings = [constructor.read(frame - 0x39 + offset, 8) for offset in range(0, 56, 8)]
            self.record = (header, strings, [], 0, constructor.read(frame + 0x1f, 4))
        elif target == 0x5D87A0:
            self.record = ([end, 0, 6, 1, 1, 0],
                           [0, 0, arg(0x28), arg(0x30), 0, arg(0x40), 0], [], arg(0x48, 4), None)
        elif target == 0x5D96E0:
            header, strings, models, model, bank = self.record
            address = instruction.address + instruction.size
            # The no-crew slot uses the regular Grace header but registers under ID zero.
            ident = 0 if address - self.base == 0x5CEBAB else header[0]
            self.rows[address] = dict(address=hex(address), id=ident, style=header[1], header=header,
                                      strings=[self.text(ptr) for ptr in strings],
                                      models=models, model=model, bank=bank)
        elif target == 0x0F27F0:
            # The initializer-list object holds the beginning and end of the actor index span.
            self.write(dst, 8, src)
            self.write(dst + 8, 8, end)
            result = dst
        elif target == 0x139620:
            first, last = self.read(src, 8), self.read(src + 8, 8)
            self.vectors[dst] = [self.read(address, 4) for address in range(first, last, 4)]
        elif target == 0x0B8890:
            self.write(dst, 8, 0)
            result = dst
        elif target == 0x0EA840:
            self.write(dst, 8, self.read(src, 4) | (1 << 32))
            result = dst
        elif target == 0x26A250:
            # Model descriptor copied from the stack to the global registry by the caller.
            self.write(dst, 8, src)
            self.write(dst + 8, 4, end)
            self.write(dst + 12, 8, self.read(regs["r9"], 8))
            self.write(dst + 20, 1, arg(0x20, 1))
            regs["rax"] = dst
            return
        elif target in (0x26A210, 0x26A230):
            if target == 0x26A210:
                scale = regs["xmm1"] & 0xffffffff
                x, y = end, regs["r9"]
            else:
                scale, x, y = 0x3f800000, 0, 0
            for offset, value in enumerate((scale, scale, x, y)):
                self.write(dst + 4 * offset, 4, value)
            regs["rax"] = dst
            return
        elif target == 0x7C5460:
            result = regs.get("rax")

        for name in layout.VOLATILE:
            regs.pop(name, None)
        regs["rax"] = result


def extract(dll):
    crew = Registry(dll, 0x5CEA20, 0x5D7F8D)
    crew.decode()
    model = Registry(dll, 0x65E50, 0x6B766)
    model.decode()
    models = []
    for index in range(110):
        record = model.base + 0x1294310 + index * 0x58
        path = model.text(model.read(record, 8))
        placement = []
        for offset in (0x18, 0x28, 0x38):
            values = model.read(record + offset, 16)
            if values is None and index >= 107:
                values = 0  # Three unused zero-initialized slots at the end of the table.
            placement.append(list(struct.unpack("<ffii", values.to_bytes(16, "little"))))
        models.append(dict(index=index, path=path, placement=placement))
    banks = [crew.text(crew.read(crew.base + 0x8D8810 + 0x10 * index, 8))
             for index in range(39)]
    rows = list(crew.rows.values())
    if (len(rows) != 216 or len({(row["id"], row["style"]) for row in rows}) != len(rows)
            or sum(0 < row["id"] <= 200 and bool(row["strings"][2]) for row in rows) != 196
            or any(not bank for bank in banks)
            or any(not entry["path"].endswith(".model3.json") for entry in models[1:107])
            or any(not math.isfinite(value) for entry in models for pose in entry["placement"] for value in pose)):
        raise ValueError("Incomplete crew registration in 2026101300 soundvoltex.dll")
    return dict(rows=rows, models=models, banks=banks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
    from hdd_inputs import require_module_profile

    module_data = args.module.read_bytes()
    require_module_profile(module_data)
    dll = pefile.PE(data=module_data, fast_load=True)
    data = extract(dll)
    args.output.write_text(json.dumps(data, ensure_ascii=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
