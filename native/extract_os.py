"""Decode a Model:Cycles OS .syx into its firmware sections (section_3.bin = MAIN OS, loaded at 0x40000400).

    python3 extract_os.py <model-cycles_OS1.13.syx> <Modded-Cycles>/tools <out_dir> [flash.bin]

With flash.bin: also writes a 2 MB SPI-flash image (erased, firmware container at 0x20000, where the OS
reads its own section table) for the virtual board.

Uses Modded-Cycles' mtlib: SysEx unpacking with checksums, ELE3 container, aPLib decompression.
"""
import hashlib
import pathlib
import sys

sys.path.insert(0, sys.argv[2])
from mtlib import syx, container, aplib  # noqa: E402

stream, info = syx.unwrap(pathlib.Path(sys.argv[1]).read_bytes())
c = container.parse(stream)
print(f"{info['name']} OS {c['version']}: {info['count']} packets, checksums OK")
out = pathlib.Path(sys.argv[3])
out.mkdir(parents=True, exist_ok=True)
for s in c['sections']:
    blob = c['blob'][s['off']:s['off'] + s['size']]
    try:
        data, _ = aplib.depack(blob)
        kind = 'aPLib'
    except ValueError:
        data, kind = blob, 'stored'
    (out / f"section_{s['id']}.bin").write_bytes(data)
    print(f"  section {s['id']}: load {s['attr']:#010x}, {kind}, {len(data)} bytes, "
          f"sha256 {hashlib.sha256(data).hexdigest()[:16]}")

if len(sys.argv) > 4:
    flash = bytearray(b'\xff' * 0x200000)
    flash[0x20000:0x20000 + len(c['blob'])] = c['blob']
    pathlib.Path(sys.argv[4]).write_bytes(flash)
    print(f"  flash image: container at 0x20000 -> {sys.argv[4]}")
