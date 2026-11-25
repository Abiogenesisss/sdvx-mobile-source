"""Read the bundled selector strings and sticker dimensions from the supported DLL."""
import argparse
import json
from importlib import import_module
from pathlib import Path

import pefile

layout = import_module("extract-stage-registration")


POSES = {address: slot for slot, address in enumerate((0x18006A752,0x18006A76C,0x18006A78A,
                                                        0x18006A900,0x18006A91D,0x18006A93B))}


class Metadata(layout.Initializer):
    def text(self, ptr):
        if not ptr:
            return ""
        return self.dll.get_string_at_rva(ptr - self.base, 512).decode("cp932")

    def call(self, instruction, target):
        reg = self.registers
        stack = reg["rsp"]
        arg = lambda offset: self.read(stack + offset, 8)
        if target == 0x26A210 and instruction.address in POSES:
            import struct
            x, y = (v - (1 << 32) if v >= 1 << 31 else v for v in (reg["r8"],reg["r9"]))
            scale = struct.unpack("<f", (reg["xmm1"] & 0xffffffff).to_bytes(4,"little"))[0]
            self.rows[POSES[instruction.address]] = [scale,scale,x,y]
        elif target in (0x3E4960, 0x3E4A30):
            wide = target == 0x3E4960
            self.rows[reg["rdx"]] = dict(width=reg["r8"], height=reg["r9"] if wide else reg["r8"],
                                         name=self.text(arg(0x30 if wide else 0x28)))
        elif target in (0x5D9720, 0x5D9630):
            # The first registrations initialize their record in the caller's stack frame.
            buf = reg["r8"] if target == 0x5D9630 else reg["rdx"]
            self.record = (self.read(buf,4), self.read(buf+4,4), self.read(buf+8,4),
                           [self.text(self.read(buf+o,8)) for o in (0x28,0x30,0x38,0x40)])
        elif target in (0x5D7FF0, 0x5D8270, 0x5D87A0, 0x5D8150):
            if target == 0x5D7FF0:
                offsets = (0x30, None, 0x40, 0x48)
            elif target == 0x5D8270:
                offsets = (0x28, 0x30, 0x38, 0x40)
            elif target == 0x5D87A0:
                offsets = (0x28, 0x30, None, 0x40)
            else:
                offsets = (0x28, None, 0x38, None)
            self.record = (reg["r8"], reg.get("r9",0) if target in (0x5D7FF0,0x5D8270) else 0,
                           self.read(stack+0x20,4) if target in (0x5D7FF0,0x5D8270) else (6 if target==0x5D87A0 else 0),
                           [self.text(arg(o)) if o else "" for o in offsets])
        elif target == 0x5D8680:
            # This constructor has a single call and includes its own RIP-relative art references.
            ptrs=[]
            for i in self.decoder.disasm(self.dll.get_data(target,0x120),self.base+target):
                if i.mnemonic == "lea" and i.operands[1].type == layout.x86.X86_OP_MEM and i.reg_name(i.operands[1].mem.base)=="rip":
                    ptrs.append(self.text(i.address + i.size + i.operands[1].mem.disp))
            self.record = (171,0,5,[next(x for x in ptrs if x.startswith("icon_")), "",
                                     next(x for x in ptrs if x.startswith("name")),next(x for x in ptrs if x.startswith("psd_"))])
        elif target == 0x5D83C0:
            self.record = None  # Model-only registry, absent from the selector.
        elif target == 0x5D96E0 and self.record:
            ident,style,order,fields = self.record
            if ident is not None and 0 < ident <= 200 and fields[0]:
                self.rows[ident,style] = dict(id=ident,style=style,order=order,fields=fields)
            self.record = None
        # Only the stack probe returns a value used by the following address calculations.
        if target != 0x7C5460:
            for name in layout.VOLATILE:
                reg.pop(name,None)


def read(dll, start, end):
    import capstone
    reader=Metadata(dll,start,end)
    reader.record=None
    reader.decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
    reader.decoder.detail=True
    return reader.decode()


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module",type=Path)
    parser.add_argument("work",type=Path)
    args=parser.parse_args()
    dll=pefile.PE(str(args.module),fast_load=True)
    args.work.mkdir(parents=True,exist_ok=True)
    crews=read(dll,0x5CEA20,0x5D7F8D)
    poses=read(dll,0x65E50,0x6A940)
    (args.work/"crew-placements.json").write_text(json.dumps([poses[i] for i in range(6)]),encoding="utf-8")
    stickers=read(dll,0x3E4C10,0x3EAC2E)
    if len(crews)!=196 or len(stickers)!=107:
        raise ValueError("Incomplete selector registration in HDD")
    (args.work/"crew-selector.json").write_text(json.dumps(list(crews.values()),ensure_ascii=False),encoding="utf-8")
    (args.work/"sticker-metadata.json").write_text(json.dumps(stickers,ensure_ascii=False),encoding="utf-8")
    print(f"Read {len(crews)} selectable styles and {len(stickers)} sticker records")
