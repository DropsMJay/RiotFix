"""
Args.py - how many stack arguments a function takes, read from the binary.

A hook has to declare the same calling convention as the function it replaces, and getting it wrong corrupts the
stack: RiotFix once hooked a 4-argument method as if it took 6 and the game crashed on the first call, with the
"third argument" turning out to be a return address. The count comes from the "ret imm16" that ends the function,
so the only hard part is finding which "ret" is the end.

Linear disassembly to the first "ret" is wrong: a function's body can be laid out with its tail before an epilogue
that other paths jump to, so the first "ret" often belongs to a branch and the bytes after it are still part of the
same function. This walks the instructions, remembers every branch target inside the code, and accepts a "ret" only
once no branch of the function points past it.

Usage:
    python Args.py <exe or rfl> <address> [<address> ...]
    python Args.py SUFFERING.EXE 0x40C330 0x40D1A0
    python Args.py Suffering.rfl 0x10127b30 --base 0x10000000

A .rfl is loaded flat at its image base, so pass --base for one (PIH 0x10000000, TTB 0x18A0000).
Needs capstone: pip install capstone
"""

import struct
import sys

import capstone


def SectionsOfPe(Data):
    """Returns (ImageBase, [(Name, VirtualAddress, VirtualSize, RawAddress, RawSize)]) for a PE file."""
    HeaderAt = struct.unpack_from("<I", Data, 0x3C)[0]
    ImageBase = struct.unpack_from("<I", Data, HeaderAt + 0x34)[0]
    Count = struct.unpack_from("<H", Data, HeaderAt + 6)[0]
    OptionalSize = struct.unpack_from("<H", Data, HeaderAt + 0x14)[0]
    Table = HeaderAt + 0x18 + OptionalSize
    Sections = []
    for Index in range(Count):
        At = Table + Index * 40
        Name = Data[At:At + 8].split(b"\0")[0].decode("latin-1")
        VirtualSize, VirtualAddress, RawSize, RawAddress = struct.unpack_from("<IIII", Data, At + 8)
        Sections.append((Name, VirtualAddress, VirtualSize, RawAddress, RawSize))
    return ImageBase, Sections


class Image:
    """A PE file, or any file loaded flat at a given base, addressed by virtual address."""

    def __init__(self, Path, FlatBase=None):
        self.Data = open(Path, "rb").read()
        self.FlatBase = FlatBase
        if FlatBase is None:
            self.ImageBase, self.Sections = SectionsOfPe(self.Data)

    def Offset(self, Address):
        if self.FlatBase is not None:
            Offset = Address - self.FlatBase
            return Offset if 0 <= Offset < len(self.Data) else None
        Relative = Address - self.ImageBase
        for _, VirtualAddress, VirtualSize, RawAddress, RawSize in self.Sections:
            if VirtualAddress <= Relative < VirtualAddress + max(VirtualSize, RawSize):
                Offset = RawAddress + (Relative - VirtualAddress)
                return Offset if Offset < len(self.Data) else None
        return None


Jumps = {"jmp", "je", "jne", "jz", "jnz", "ja", "jae", "jb", "jbe", "jg", "jge", "jl", "jle", "jo", "jno",
         "js", "jns", "jp", "jnp", "jcxz", "jecxz", "loop", "loope", "loopne"}


def FunctionEnd(Image, Entry, Limit=4096):
    """Walks from Entry and returns (Address, Mnemonic, Operands) of the "ret" that ends the function.

    Every branch target inside the walked range is remembered; a "ret" ends the function only when no branch
    points past it. Returns None if no such "ret" is found within Limit bytes."""
    Offset = Image.Offset(Entry)
    if Offset is None:
        return None
    Engine = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    Engine.detail = True
    Furthest = Entry
    for Instruction in Engine.disasm(Image.Data[Offset:Offset + Limit], Entry, Limit):
        if Instruction.mnemonic in Jumps and Instruction.operands:
            Operand = Instruction.operands[0]
            if Operand.type == capstone.x86.X86_OP_IMM:
                Target = Operand.imm
                if Entry <= Target < Entry + Limit:
                    Furthest = max(Furthest, Target)
        if Instruction.mnemonic == "ret" and Instruction.address >= Furthest:
            return Instruction.address, Instruction.mnemonic, Instruction.op_str
    return None


def Report(Image, Address):
    End = FunctionEnd(Image, Address)
    if End is None:
        print("0x%08x  no ret found - not code, or longer than the limit" % Address)
        return
    EndAddress, _, Operands = End
    if not Operands:
        print("0x%08x  ends 0x%08x with a plain \"ret\": the caller cleans up (cdecl), or thiscall with no stack "
              "arguments" % (Address, EndAddress))
        return
    Bytes = int(Operands, 0)
    print("0x%08x  ends 0x%08x with \"ret 0x%x\": %d stack argument(s) (plus \"this\" in ecx if it is a method)"
          % (Address, EndAddress, Bytes, Bytes // 4))


def Main(Arguments):
    if len(Arguments) < 2:
        print(__doc__)
        return 1
    FlatBase = None
    if "--base" in Arguments:
        At = Arguments.index("--base")
        FlatBase = int(Arguments[At + 1], 0)
        Arguments = Arguments[:At] + Arguments[At + 2:]
    Picture = Image(Arguments[0], FlatBase)
    for Text in Arguments[1:]:
        Report(Picture, int(Text, 0))
    return 0


if __name__ == "__main__":
    sys.exit(Main(sys.argv[1:]))
