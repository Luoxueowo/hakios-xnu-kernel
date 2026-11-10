#!/usr/bin/env python3
"""Fixed Polaris storage layout. Preparation and updates never format user data.

plan / bootargs: inspect the layout; snapshot: back up the current contiguous APFS
containers over recovery ADB; pack: validate images and make a bounded update manifest;
apply-system: write and verify only the three system images from that manifest.
User data is initialized separately with mkdatavol.py and apply-data --initialize-data.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
DEFAULT = ROOT / 'configs/polaris-storage.json'


def layout(path=DEFAULT, partition_bytes=None):
    c = json.loads(Path(path).read_text())
    if c['version'] != 1 or c['block_size'] != 4096:
        raise ValueError('unsupported storage layout')
    if c['device'] != 'polaris' or c['partition'] != 'darwin':
        raise ValueError('this installer is for Polaris darwin only')
    if [r.get('gib') for r in c['regions']] != [10, 4, 1]:
        raise ValueError('layout v1 reserves 10/4/1 GiB; changing boundaries requires a new layout version and data migration')
    size = c['partition_bytes'] if partition_bytes is None else partition_bytes
    if type(size) is not int or size % 4096:
        raise ValueError('partition size must be a multiple of 4096')
    regions, offset = [], 0
    if [r['name'] for r in c['regions']] != ['system', 'os', 'app']:
        raise ValueError('expected system, os, app in that order')
    for r in c['regions']:
        if type(r['gib']) is not int or r['gib'] <= 0:
            raise ValueError('region sizes must be positive integer GiB')
        length = r['gib'] * 1024**3
        regions.append(dict(name=r['name'], offset=offset, size=length))
        offset += length
    if offset >= size:
        raise ValueError('no room for user data')
    c.update(regions=regions, data_offset=offset, partition_bytes=size)
    return c


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(4 * 1024**2), b''):
            h.update(b)
    return h.hexdigest()


def apfs_size(b):
    if len(b) < 4096 or b[32:36] != b'NXSB':
        raise ValueError('not a raw APFS container (extract DMGs first)')
    bs, blocks = struct.unpack_from('<IQ', b, 36)
    if bs != 4096 or blocks == 0:
        raise ValueError('invalid APFS block geometry')
    return bs * blocks


def adb(command):
    return subprocess.check_output(['adb', 'shell', command]).decode().strip()


def identity(c):
    # Restrict every device operation to the partition observed on this Polaris.
    if adb('getprop ro.product.device') != c['device']:
        raise ValueError('ADB is not the Polaris recovery')
    if adb('readlink -f /dev/block/by-name/darwin') != '/dev/block/sda22':
        raise ValueError('unexpected darwin device')
    if int(adb('blockdev --getsize64 /dev/block/by-name/darwin')) != c['partition_bytes']:
        raise ValueError('partition capacity differs from the layout')
    if adb("grep -E 'sda22|by-name/darwin' /proc/mounts || true"):
        raise ValueError('darwin is mounted; unmount it before accessing the layout')


def remote_read(offset, size):
    if offset % 4096 or size % 4096:
        raise ValueError('unaligned read')
    return subprocess.check_output(['adb', 'exec-out',
        f'dd if=/dev/block/by-name/darwin bs=4096 skip={offset//4096} count={size//4096} 2>/dev/null'])


def snapshot(c, directory):
    identity(c)
    directory.mkdir(parents=True, exist_ok=True)
    fixed = all(remote_read(r['offset'], 4096)[32:36] == b'NXSB' for r in c['regions'][1:])
    offset = 0
    records = []
    for r in c['regions']:
        if fixed:
            offset = r['offset']
        size = apfs_size(remote_read(offset, 4096))
        if size > r['size'] or offset + size > c['partition_bytes']:
            raise ValueError('installed container exceeds the proposed slot or partition')
        path = directory / (r['name'] + '.apfs')
        if path.exists():
            raise ValueError(f'will not overwrite snapshot {path}')
        with path.open('wb') as f:
            subprocess.run(['adb', 'exec-out',
                f'dd if=/dev/block/by-name/darwin bs=4096 skip={offset//4096} count={size//4096} 2>/dev/null'], stdout=f, check=True)
        if path.stat().st_size != size:
            raise ValueError('short snapshot; keep recovery connected')
        records.append(dict(name=r['name'], offset=offset, size=size, sha256=digest(path)))
        offset += size
    # Record physical locations so recovery is possible after either layout.
    (directory / 'snapshot.json').write_text(json.dumps(records, indent=2) + '\n')
    print('Saved the current System/OS/App containers; this does not back up user files.')


def pack(c, args):
    entries = []
    for r in c['regions']:
        path = Path(getattr(args, r['name'])).resolve()
        with path.open('rb') as f:
            size = apfs_size(f.read(4096))
        if size != path.stat().st_size or size > r['size']:
            raise ValueError(f'{r["name"]}: image size/header mismatch or reserved slot exceeded')
        entries.append(dict(**r, image=str(path), image_bytes=size, sha256=digest(path)))
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(dict(layout=c, images=entries), indent=2) + '\n')
    print(out)


def write_image(c, path, offset, limit, expected=None):
    size = path.stat().st_size
    if size == 0 or size % 4096 or offset % 4096 or size > limit:
        raise ValueError('image is empty, unaligned, or exceeds its reserved region')
    sha = digest(path)
    if expected is not None and sha != expected:
        raise ValueError('image changed after packaging')
    identity(c)
    if expected is not None:
        current = adb(f'dd if=/dev/block/by-name/darwin bs=4096 skip={offset//4096} count={size//4096} 2>/dev/null | sha256sum')
        if current.split()[0] == sha:
            print(f'already verified {path.name}: unchanged at byte {offset}', flush=True)
            return
    target = '/data/local/tmp/polaris-storage-stage.img'
    subprocess.run(['adb', 'push', str(path), target], check=True)
    if adb(f'sha256sum {target}').split()[0] != sha:
        raise ValueError('staged checksum mismatch; no partition write performed')
    # Check target identity again in the same shell as the write.
    guard = ('set -e; test "$(getprop ro.product.device)" = polaris; '
             'test "$(readlink -f /dev/block/by-name/darwin)" = /dev/block/sda22; '
             f'test "$(blockdev --getsize64 /dev/block/by-name/darwin)" = {c["partition_bytes"]}; '
             'if grep -E "sda22|by-name/darwin" /proc/mounts; then exit 1; fi; ')
    adb(guard + f'dd if={target} of=/dev/block/by-name/darwin bs=4096 seek={offset//4096} conv=notrunc; sync')
    result = adb(f'dd if=/dev/block/by-name/darwin bs=4096 skip={offset//4096} count={size//4096} 2>/dev/null | sha256sum')
    if result.split()[0] != sha:
        raise ValueError('partition readback checksum mismatch; do not boot')
    adb(f'rm -f {target}')
    print(f'verified {path.name}: bytes {offset}..{offset+size-1}', flush=True)


def apply_system(c, manifest, first_install):
    m = json.loads(manifest.read_text())
    if m['layout'] != c or len(m['images']) != 3:
        raise ValueError('manifest layout differs from the selected layout')
    # Validate every image before the first write; later updates never touch Data.
    for r, e in zip(c['regions'], m['images']):
        if any(e[k] != r[k] for k in ('name', 'offset', 'size')):
            raise ValueError('manifest region mismatch')
        p = Path(e['image'])
        with p.open('rb') as f:
            size = apfs_size(f.read(4096))
        if size != p.stat().st_size or size != e['image_bytes'] or size > r['size'] or digest(p) != e['sha256']:
            raise ValueError('invalid or changed image')
    identity(c)
    fixed = remote_read(c['regions'][1]['offset'], 4096)[32:36] == b'NXSB'
    if not fixed:
        if not first_install:
            raise ValueError('legacy layout: take a snapshot and use --first-install for migration')
        end = 0
        for _ in range(3):
            end += apfs_size(remote_read(end, 4096))
        for offset in {(end + 1024**2 - 1)//1024**2*1024**2, 2042368*4096}:
            if offset+4096 <= c['partition_bytes'] and remote_read(offset, 4096)[1024:1026] in (b'H+', b'HX'):
                raise ValueError('legacy persistent Data detected; back up and migrate user files before repartitioning')
    for e in m['images']:
        write_image(c, Path(e['image']), e['offset'], e['size'], e['sha256'])


def apply_data(c, image, initialize):
    if not initialize:
        raise ValueError('first installation requires --initialize-data; never use it for an update')
    identity(c)
    header = remote_read(c['data_offset'], 4096)
    if header[1024:1026] in (b'H+', b'HX'):
        raise ValueError('Data already contains HFS+/HFSX; refusing to reinitialize it')
    # Validate a sparse image in full before writing anything. DONT_CARE is
    # skipped explicitly, never sent to fastboot or treated as discard/erase.
    runs = []
    with image.open('rb') as f:
        h = f.read(28)
        magic, major, minor, fh, ch, bs, blocks, chunks, checksum = struct.unpack('<IHHHHIIII', h)
        if (magic, major, fh, ch, bs, blocks) != (0xed26ff3a, 1, 28, 12, 4096, c['partition_bytes']//4096):
            raise ValueError('sparse header/layout mismatch')
        block = 0
        for _ in range(chunks):
            kind, reserved, count, total = struct.unpack('<HHII', f.read(12))
            payload = total - 12
            if count == 0 or block + count > blocks:
                raise ValueError('invalid sparse extent')
            if kind == 0xcac3 and payload == 0:
                pass
            elif kind in (0xcac1, 0xcac2) and payload == (count*4096 if kind == 0xcac1 else 4):
                if block*4096 < c['data_offset']:
                    raise ValueError('data formatter would overwrite the system')
                runs.append((kind, block*4096, count*4096, f.tell()))
            else:
                raise ValueError('unsupported sparse chunk')
            f.seek(payload, 1)
            block += count
        if block != blocks or f.tell() != image.stat().st_size:
            raise ValueError('truncated sparse image')
        import tempfile
        # Publish the primary HFS volume header only after its other metadata.
        # An interrupted first initialization can then be retried from recovery.
        runs.sort(key=lambda r: r[1] == c['data_offset'])
        for kind, offset, size, source in runs:
            f.seek(source)
            with tempfile.NamedTemporaryFile() as out:
                pattern = f.read(4) if kind == 0xcac2 else None
                left = size
                while left:
                    n = min(left, 1024**2)
                    b = pattern*(n//4) if pattern is not None else f.read(n)
                    if len(b) != n:
                        raise ValueError('short sparse payload')
                    out.write(b); left -= n
                out.flush()
                write_image(c, Path(out.name), offset, size)
    print('Initialized Data. Omit this step for every subsequent system update.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--layout', type=Path, default=DEFAULT)
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('plan'); sub.add_parser('bootargs')
    s = sub.add_parser('snapshot'); s.add_argument('directory', type=Path)
    s = sub.add_parser('pack')
    for name in ('system', 'os', 'app'): s.add_argument('--'+name, required=True)
    s.add_argument('-o', '--output', default='out/storage/system-update.json')
    s = sub.add_parser('apply-system'); s.add_argument('manifest', type=Path); s.add_argument('--first-install', action='store_true')
    s = sub.add_parser('apply-data'); s.add_argument('image', type=Path); s.add_argument('--initialize-data', action='store_true')
    a = p.parse_args(); c = layout(a.layout)
    if a.cmd == 'plan':
        print(json.dumps(c, indent=2)); print('Data bytes:', c['partition_bytes']-c['data_offset'])
    elif a.cmd == 'bootargs':
        print(f'cryptex_os_block={c["regions"][1]["offset"]//4096} cryptex_app_block={c["regions"][2]["offset"]//4096} darwin_data={c["data_offset"]//4096} var_hfs=ufs_darwin_data data_required=1')
    elif a.cmd == 'snapshot': snapshot(c, a.directory)
    elif a.cmd == 'pack': pack(c, a)
    elif a.cmd == 'apply-system': apply_system(c, a.manifest, a.first_install)
    elif a.cmd == 'apply-data': apply_data(c, a.image, a.initialize_data)


if __name__ == '__main__':
    try: main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError, struct.error) as e:
        sys.exit('polaris-storage: '+str(e))
