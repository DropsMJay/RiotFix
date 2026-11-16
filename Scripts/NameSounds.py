"""NameSounds - puts the package and record of each sound next to the sizes in RiotFix.log.

RiotFix logs a sound by the size of the buffer DirectSound was asked for. That size is the size
of the decoded data in the game's own sound package, so it names the sound: this script reads
every .sdu under the game folder, builds a table of (bytes, rate, channels) -> record, and
rewrites a log with the name of each sound added.

    python NameSounds.py "C:\\Games\\The Suffering" RiotFix.log > RiotFix.named.log
    python NameSounds.py "C:\\Games\\The Suffering" --list      (every sound in every package)

The .sdu format: the magic "SRSC", then one record after another, each a header of 0x40 bytes
followed by the sound's data, the next record starting at the next multiple of 0x20. In the
header, the word at +0x2C holds the channels and the bits per sample, +0x30 the sample rate and
+0x38 the number of bytes of data. The package's own number (the 2 of "2_019a") is the "id" line
of the .db file beside it; the record's place in the file is not always the number SurrealEd
shows, so a sound is named here as "<package> record <n>".
"""

import os
import re
import struct
import sys


def ReadPackageNumber(Folder, Name):
    """The 'id' line of the package's .db file, or None."""
    Path = os.path.join(Folder, Name + '.db')
    try:
        with open(Path, 'r', encoding='latin-1') as File:
            for Line in File:
                if Line.startswith('id '):
                    return int(Line[3:].strip())
    except (OSError, ValueError):
        pass
    return None


def LooksLikeHeader(Fields):
    """A record header, by the fields that hold steady across every package: the channel count
    and bits per sample packed in one word, a sane sample rate, and a length that is a whole
    number of samples. The word at +0x0C is not the format tag: one package alone shows eleven
    different values there."""
    Channels, Bits, Rate, Bytes = Fields[11] & 0xFFFF, (Fields[11] >> 16) & 0xFFFF, Fields[12], Fields[14]
    if Channels not in (1, 2) or Bits not in (8, 16) or not 1000 <= Rate <= 48000:
        return False
    Sample = Channels * (Bits // 8)
    return 0 < Bytes <= 0x4000000 and Bytes % Sample == 0


def ReadPackage(Path):
    """Every sound record of one .sdu file.

    The packages that are loaded whole put one record straight after another, so the next header
    is at the end of the data rounded up to a multiple of 0x20. The streamed packages (the levels,
    World Common, NPCs) keep a block of their own between records whose length varies, so the
    reader looks for the next header's signature instead of counting on the stride."""
    Records = []
    Size = os.path.getsize(Path)
    with open(Path, 'rb') as File:
        if File.read(4) != b'SRSC':
            return Records
        Offset = 0x20
        while Offset + 0x40 <= Size:
            File.seek(Offset)
            Header = File.read(0x40)
            if len(Header) < 0x40:
                break
            Fields = struct.unpack('<16I', Header)
            if not LooksLikeHeader(Fields) or Offset + 0x40 + Fields[14] > Size:
                break
            Bytes = Fields[14]
            Channels, Bits = Fields[11] & 0xFFFF, (Fields[11] >> 16) & 0xFFFF
            Rate = Fields[12]
            Frame = Rate * Channels * (Bits // 8)
            Records.append({
                'Index': len(Records), 'Bytes': Bytes, 'Rate': Rate, 'Channels': Channels,
                'Bits': Bits, 'Milliseconds': Bytes / Frame * 1000.0 if Frame else 0.0,
            })
            Next = (Offset + 0x40 + Bytes + 0x1F) & ~0x1F
            File.seek(Next)
            Ahead = File.read(0x40)
            if len(Ahead) == 0x40 and LooksLikeHeader(struct.unpack('<16I', Ahead)):
                Offset = Next
                continue
            Offset = FindHeader(File, Next, Size)
            if Offset is None:
                break
    return Records


def FindHeader(File, From, Size, Window = 0x4000):
    """The next record header at or after From, found by its signature. None at the end."""
    Place = From
    while Place < Size:
        File.seek(Place)
        Block = File.read(Window + 0x40)
        if len(Block) < 0x40:
            return None
        for Step in range(0, len(Block) - 0x40, 4):
            if LooksLikeHeader(struct.unpack_from('<16I', Block, Step)):
                return Place + Step
        Place += Window
    return None


def ReadGame(GameFolder):
    """Every .sdu under the game folder, as (Name, Number, Records)."""
    Packages = []
    for Folder, _, Files in os.walk(GameFolder):
        for File in Files:
            if File.lower().endswith('.sdu'):
                Name = File[:-4]
                Records = ReadPackage(os.path.join(Folder, File))
                if Records:
                    Packages.append((Name, ReadPackageNumber(Folder, Name), Records))
    return Packages


def BuildTable(Packages):
    """(bytes, rate, channels) -> the records that match it."""
    Table = {}
    for Name, Number, Records in Packages:
        for Record in Records:
            Key = (Record['Bytes'], Record['Rate'], Record['Channels'])
            Table.setdefault(Key, []).append((Name, Number, Record['Index']))
    return Table


def Describe(Hits):
    """How to write one match: a single name, a list, or nothing."""
    if not Hits:
        return None
    if len(Hits) > 3:
        return '%d sounds of the same size' % len(Hits)
    return ', '.join('%s%s record %d' % (Name, '' if Number is None else ' (%d)' % Number, Index)
                     for Name, Number, Index in Hits)


BufferLine = re.compile(r'buffer (\d+) bytes \((\d+) ms, (\d+) Hz, (\d+) channels\)')


def NameLog(LogPath, Table, Output):
    Named = Ambiguous = Missing = 0
    Seen = set()
    with open(LogPath, 'r', encoding='utf-8', errors='replace') as File:
        for Line in File:
            Match = BufferLine.search(Line)
            if Match:
                Key = (int(Match.group(1)), int(Match.group(3)), int(Match.group(4)))
                Hits = Table.get(Key, [])
                if Key not in Seen:
                    Seen.add(Key)
                    if len(Hits) == 1:
                        Named += 1
                    elif Hits:
                        Ambiguous += 1
                    else:
                        Missing += 1
                Text = Describe(Hits)
                if Text:
                    Line = Line.rstrip('\n') + '  = ' + Text + '\n'
            Output.write(Line)
    return Named, Ambiguous, Missing


def Main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    GameFolder, Second = sys.argv[1], sys.argv[2]
    Packages = ReadGame(GameFolder)
    if not Packages:
        sys.stderr.write('No .sdu package found under %s\n' % GameFolder)
        return 1
    for Name, Number, Records in sorted(Packages):
        sys.stderr.write('%-20s %-6s %5d sounds\n'
                         % (Name, '' if Number is None else '(%d)' % Number, len(Records)))
    if Second == '--list':
        for Name, Number, Records in sorted(Packages):
            for Record in Records:
                print('%s%s record %d: %d bytes, %d Hz, %d channels, %.1f ms'
                      % (Name, '' if Number is None else ' (%d)' % Number, Record['Index'],
                         Record['Bytes'], Record['Rate'], Record['Channels'], Record['Milliseconds']))
        return 0
    Named, Ambiguous, Missing = NameLog(Second, BuildTable(Packages), sys.stdout)
    sys.stderr.write('\nDistinct sounds in the log: %d named, %d of an ambiguous size, %d not in any package\n'
                     % (Named, Ambiguous, Missing))
    return 0


if __name__ == '__main__':
    sys.exit(Main())
