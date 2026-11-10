#!/usr/bin/env python3
"""The data volume of Darwin for the phone: an empty HFS Plus volume in darwin past the Darwin volume and the cryptexes,
as an Android sparse image of the whole of darwin that writes that volume and nothing else.

usage: mkdatavol.py OUT.simg [MKFS]      (MKFS: mkfs.hfsplus, default the one in $PATH)

    fastboot flash darwin OUT.simg

The kernel gives that stretch of darwin out as /dev/ufs_darwin_data (drivers/storage/hakios.ufs.c, ufs_write=1) and
init mounts it on /private/var (userland/src/init.c, var_hfs=ufs_darwin_data). Flashing this again empties it.

The image says DONT_CARE for darwin's blocks before the volume -- the Darwin volume and both cryptexes, which the
bootloader therefore leaves as they are -- and, inside the volume, for every block the new volume does not use: what
is in free space does not matter to HFS Plus, and twelve gigabytes of zeros need not go over USB. The blocks it uses
(the volume headers, the allocation bitmap, the catalog and extents trees, the journal) are written whole, zeros
included: which those are is read from the volume's own allocation bitmap.

Case-sensitive (HFSX), as Darwin's own data volume is, and journaled.
"""
import argparse
import importlib.util
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

BS = 4096
DARWIN_BLOCKS = 5242875            # darwin, in blocks of 4096 (README, "The port's partition")
DATA_FIRST = 2042368               # where the volume begins: drivers/storage/hakios.ufs.c, UFS_WRITE_FIRST
MAGIC, RAW, FILL, DONT_CARE = 0xED26FF3A, 0xCAC1, 0xCAC2, 0xCAC3


def chunk(kind, blocks, data_size):
    return struct.pack('<HHII', kind, 0, blocks, 12 + data_size)


def used_blocks(img):
    """The volume's allocation blocks marked in use, from its header and bitmap; the block size must be BS."""
    img.seek(1024)
    vh = img.read(512)
    sig, = struct.unpack('>H', vh[0:2])
    if sig not in (0x482B, 0x4858):                     # H+, HX
        sys.exit('mkdatavol.py: no HFS Plus volume header in what mkfs made')
    block_size, total = struct.unpack('>II', vh[40:48])
    if block_size != BS:
        sys.exit(f'mkdatavol.py: the volume has blocks of {block_size}, not {BS}')
    fork = vh[112:192]                                  # allocationFile: HFSPlusForkData
    extents = [struct.unpack('>II', fork[16 + 8 * i:24 + 8 * i]) for i in range(8)]
    bitmap = bytearray()
    for start, count in extents:
        if count:
            img.seek(start * BS)
            bitmap += img.read(count * BS)
    if len(bitmap) * 8 < total:
        sys.exit('mkdatavol.py: the allocation file is in more pieces than its fork says')
    return total, lambda b: bitmap[b >> 3] & (0x80 >> (b & 7))


def main():
    global DARWIN_BLOCKS, DATA_FIRST
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--layout', type=Path, help='fixed layout JSON, required for Polaris')
    parser.add_argument('output')
    parser.add_argument('mkfs', nargs='?', default='mkfs.hfsplus')
    args = parser.parse_args()
    out, mkfs = args.output, args.mkfs
    if args.layout:
        spec = importlib.util.spec_from_file_location('polaris_storage', Path(__file__).with_name('polaris-storage.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        config = module.layout(args.layout)
        DARWIN_BLOCKS = config['partition_bytes'] // BS
        DATA_FIRST = config['data_offset'] // BS
    blocks = DARWIN_BLOCKS - DATA_FIRST
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'data.hfs')
        with open(path, 'wb') as f:
            f.truncate(blocks * BS)
        # its root is /private/var: root's, 0755, as on an iPhone
        options = ['-s', '-J', '-b', str(BS), '-v', 'Data']
        options += ['-U', '0', '-G', '0', '-M', '755']
        attached = None
        try:
            if os.path.basename(mkfs) == 'newfs_hfs':
                # Apple's formatter takes a device, not a regular sparse file.
                import re
                result = subprocess.check_output(['hdiutil', 'attach', '-nomount', '-nobrowse',
                    '-imagekey', 'diskimage-class=CRawDiskImage', path], text=True)
                devices = re.findall(r'^(/dev/disk[0-9]+)\s', result, re.M)
                if len(devices) != 1:
                    raise RuntimeError('unexpected disk-image attachment: ' + result)
                attached = devices[0]
            subprocess.run([mkfs] + options + [attached or path], check=True)
        finally:
            if attached:
                subprocess.run(['hdiutil', 'detach', attached], check=True)
        with open(path, 'rb') as img:
            total, used = used_blocks(img)
            if total != blocks:
                sys.exit(f'mkdatavol.py: the volume has {total} blocks, darwin has {blocks} for it')
            runs = []                                   # [kind, first, count], in order
            for b in range(total):
                kind = RAW if used(b) else DONT_CARE
                if runs and runs[-1][0] == kind and runs[-1][1] + runs[-1][2] == b:
                    runs[-1][2] += 1
                else:
                    runs.append([kind, b, 1])
            chunks, written = [chunk(DONT_CARE, DATA_FIRST, 0)], 0
            for kind, first, count in runs:
                if kind == DONT_CARE:
                    chunks.append(chunk(DONT_CARE, count, 0))
                    continue
                img.seek(first * BS)
                data = img.read(count * BS)
                # zeros in a used stretch go as FILL, which costs nothing to send; everything else as RAW
                at = 0
                while at < count:
                    zero = data[at * BS:(at + 1) * BS] == bytes(BS)
                    end = at
                    while end < count and (data[end * BS:(end + 1) * BS] == bytes(BS)) == zero:
                        end += 1
                    if zero:
                        chunks.append(chunk(FILL, end - at, 4) + bytes(4))
                    else:
                        chunks.append(chunk(RAW, end - at, (end - at) * BS) + data[at * BS:end * BS])
                    written += end - at
                    at = end
    with open(out, 'wb') as f:
        f.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BS, DARWIN_BLOCKS, len(chunks), 0))
        for c in chunks:
            f.write(c)
    print(f'{out}: {os.path.getsize(out)} bytes; the volume: {blocks} blocks ({blocks * BS >> 20} MB) from darwin '
          f'block {DATA_FIRST}, {written} of them written, the rest of darwin left as it is')


if __name__ == '__main__':
    main()
