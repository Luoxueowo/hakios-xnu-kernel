#!/usr/bin/env python3
"""
Pack the phone's UEFI and an optional payload into an Android boot image for `fastboot boot`:

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a mu_aloha_platforms tree built here (mu_aloha_platforms/, see uefi/README.md)
or, when there is none, from devices/<device>/prebuilt/. The device is the phone's name in that tree
(polaris: Platforms/NapaliPkg/Device/xiaomi-polaris); the package, the SoC that names the FD, the FD's base
and size and the device tree are read from the tree by tools/mualoha.py, not written down here.

usage: mkxnuimg.py [-d device] [-p payload.bin] [-o out.img] [--mode RELEASE]
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'tools'))   # where mualoha.py is: it reads the UEFI tree for us
import mualoha                                    # type: ignore  (found through the path above)

PAYLOAD_BASE, PAYLOAD_MAX = 0xF0000000, 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-d', '--device', default=os.environ.get('MUALOHA_DEVICE') or 'polaris',
                help='the phone in the mu_aloha_platforms tree: polaris, qrd845, ... (default: polaris)')
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output')
ap.add_argument('--mode', default=os.environ.get('MUALOHA_MODE') or 'RELEASE')
args = ap.parse_args()

output = args.output or os.path.join(HERE, 'xnu-%s.img' % args.device)
PREBUILT = os.path.join(HERE, 'devices', args.device, 'prebuilt')


def prebuilt_conf(directory):
    """base, size, package and soc from devices/<device>/prebuilt/uefi.conf (tools/update-prebuilt.sh writes it)."""
    path = os.path.join(directory, 'uefi.conf')
    if not os.path.isfile(path):
        return {}
    conf = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        fields = line.split()
        if len(fields) >= 2 and fields[0] in ('device', 'package', 'soc', 'base', 'size'):
            conf[fields[0]] = fields[1]
    return conf


tree = os.environ.get('MUALOHA') or os.path.join(HERE, 'mu_aloha_platforms')
try:
    target = mualoha.resolve(tree, args.device)
except mualoha.Problem as e:
    target, why = None, str(e)
else:
    why = 'the UEFI of %s is not built: ./build_uefi.py -d %s' % (args.device, os.path.basename(target.devdir))

if target is not None and os.path.isfile(target.path('fd', args.mode)):
    try:
        parts = {'shim': mualoha.build_shim(target), 'fd': target.path('fd', args.mode),
                 'dtb': target.path('dtb', args.mode), 'ramdisk': target.path('ramdisk', args.mode),
                 'mkbootimg': target.path('mkbootimg', args.mode)}
    except mualoha.Problem as e:
        sys.exit('mkxnuimg.py: %s' % e)
    fd_size = target.size
    label = 'UEFI: %s (%s %s, base %#x, %d bytes)' % (mualoha.where(parts['fd']), target.package,
                                                      target.soc, target.base, fd_size)
else:
    conf = prebuilt_conf(PREBUILT)
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'device-tree.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
    fd_size = int(conf['size'], 0) if 'size' in conf else None
    label = 'UEFI: %s (prebuilt)' % mualoha.where(parts['fd'])

for name in ('fd', 'shim', 'dtb', 'ramdisk', 'mkbootimg'):
    path = parts[name]
    if not os.path.isfile(path):
        hint = ''
        if name == 'fd':
            hint = ('\n  The UEFI image is not in the repository: it carries the phone vendor\'s own drivers, which\n'
                    '  are not redistributed here. Build it from your own phone\'s firmware (uefi/README.md), or run\n'
                    '  tools/update-prebuilt.sh where one is built.\n  %s' % why)
        sys.exit('mkxnuimg.py: %s not found%s' % (path, hint))

print(label)

shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
if fd_size is None:
    sys.exit('mkxnuimg.py: %s needs a size: devices/%s/prebuilt/uefi.conf (tools/update-prebuilt.sh)'
             % (mualoha.where(parts['fd']), args.device))
if len(fd) != fd_size:
    sys.exit('mkxnuimg.py: %s is %d bytes, the shim copies %d (%#x): rebuild the UEFI or refresh the prebuilt'
             % (mualoha.where(parts['fd']), len(fd), fd_size, fd_size))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print('payload: %d bytes, bytesum=%08x' % (len(data), sum(data) & 0xffffffff))

dtb = open(parts['dtb'], 'rb').read()
output = os.path.abspath(output)
kernel = output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    # mkbootimg.py imports its neighbours (generate_gki_certificate): run it from its own directory
    subprocess.check_call([sys.executable, os.path.basename(parts['mkbootimg']), '--kernel', kernel,
                           '--ramdisk', os.path.abspath(parts['ramdisk']), '--header_version', '1', '-o', output],
                          cwd=os.path.dirname(parts['mkbootimg']))
finally:
    os.unlink(kernel)
print('%s: %d bytes (shim %d, uncompressed %d)' % (output, os.path.getsize(output), len(shim), len(blob)))
