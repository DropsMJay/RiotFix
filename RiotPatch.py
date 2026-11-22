"""
RiotPatch — optional exe patches for The Suffering / Ties That Bind
===================================================================

PT-BR
-----
Patches permanentes no executável (o RiotFix faz o mesmo na memória, sem mexer em arquivos).
Antes de alterar, confere os bytes originais em cada endereço e cria <exe>.orig; --restore volta ao original.

  python RiotPatch.py "E:\\Jogos\\The Suffering\\Suffering.exe" --shadow 512
  python RiotPatch.py "E:\\Jogos\\The Suffering TTB\\SufferingTTB.exe" --shadow 1024 --lod0
  python RiotPatch.py <exe> --restore

  --shadow N   resolução da textura de sombra de cada personagem (original 256; 128..2048, potência de 2).
               No primeiro jogo, acima de 256 use o RiotFix: só ele corrige o cache de texturas (1.3.4).
  --lod0       personagens sempre no LOD 0 (malha completa, em qualquer distância)
  --laa        liga a flag Large Address Aware (4 GB de endereços no Windows de 64 bits)
  --restore    volta ao .orig salvo no primeiro patch

EN
--
Permanent exe patches (RiotFix does the same in memory without touching files). Original bytes are checked at
every site and <exe>.orig is created first; --restore brings the original back. Options as above.
"""
import os
import shutil
import struct
import sys

# PT: deslocamentos no ARQUIVO (não endereços virtuais). EN: FILE offsets (not virtual addresses).
Sites = {
    'Suffering.exe': {
        'ShadowPush': (0x91EE7, bytes.fromhex('6800010000')),     # 0x492AE7 push 0x100 (shadow size, call 0x492AF2)
        'ShadowCmp':  (0xCE336, bytes.fromhex('81FD00010000')),   # 0x4CEF36 cmp ebp, 0x100 / ja
        'LodDist':    (0x91F07, bytes.fromhex('8B542420')),       # 0x492B07 mov edx,[esp+0x20] (dist² for 0x46B220)
    },
    'SufferingTTB.exe': {
        'ShadowPush': (0x95F35, bytes.fromhex('6800010000')),     # 0x496B35
        'ShadowCmp':  (0xDBA13, bytes.fromhex('81FD00010000')),   # 0x4DC613
        'LodDist':    (0x95F55, bytes.fromhex('8B542420')),       # 0x496B55 (LOD pick 0x470CC0)
    },
}


def Main(Arguments):
    if not Arguments:
        print(__doc__)
        return 1
    ExePath = Arguments[0]
    Name = os.path.basename(ExePath)
    Name = next((Key for Key in Sites if Key.lower() == Name.lower()), Name)   # SUFFERING.EXE = Suffering.exe
    if Name not in Sites:
        print('Unknown executable:', Name)
        return 1
    BackupPath = ExePath + '.orig'
    if '--restore' in Arguments:
        if not os.path.exists(BackupPath):
            print('No .orig to restore.')
            return 1
        shutil.copyfile(BackupPath, ExePath)
        print('Restored from', BackupPath)
        return 0

    Source = open(BackupPath if os.path.exists(BackupPath) else ExePath, 'rb').read()
    GameSites = Sites[Name]
    for Key, (Offset, Expected) in GameSites.items():
        if Source[Offset:Offset + len(Expected)] != Expected:
            print(f'Unexpected bytes at {Key} ({Offset:#x}); different exe version? Nothing was changed.')
            return 1
    Data = bytearray(Source)

    if '--shadow' in Arguments:
        Size = int(Arguments[Arguments.index('--shadow') + 1])
        if Size < 128 or Size > 2048 or Size & (Size - 1):
            print('Use 128, 256, 512, 1024 or 2048.')
            return 1
        Offset, _ = GameSites['ShadowPush']
        Data[Offset:Offset + 5] = b'\x68' + struct.pack('<I', Size)
        Offset, _ = GameSites['ShadowCmp']
        Data[Offset:Offset + 6] = b'\x81\xFD' + struct.pack('<I', Size)
        print(f'Shadow: {Size}x{Size}')
    if '--lod0' in Arguments:
        Offset, _ = GameSites['LodDist']
        Data[Offset:Offset + 4] = bytes.fromhex('31D29090')   # xor edx, edx; nop; nop
        print('LOD: always 0')
    if '--laa' in Arguments:
        PeOffset = struct.unpack_from('<I', Data, 0x3C)[0]
        if Data[PeOffset:PeOffset + 4] != b'PE\0\0':
            print('PE header not found; nothing was changed.')
            return 1
        Flags = struct.unpack_from('<H', Data, PeOffset + 22)[0]
        struct.pack_into('<H', Data, PeOffset + 22, Flags | 0x20)
        print('Large Address Aware: on' + (' (already was)' if Flags & 0x20 else ''))

    if not os.path.exists(BackupPath):
        shutil.copyfile(ExePath, BackupPath)
        print('Backup:', BackupPath)
    open(ExePath, 'wb').write(Data)
    print('Written:', ExePath)
    return 0


if __name__ == '__main__':
    sys.exit(Main(sys.argv[1:]))
