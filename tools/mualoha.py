#!/usr/bin/env python3
"""Where mu_aloha_platforms keeps what mkxnuimg.py packs: the UEFI image, the boot shim, the device tree.

  tools/mualoha.py [-t TREE] [-d DEVICE] [-m MODE] target
  tools/mualoha.py ... fd | dtb | ramdisk | mkbootimg | shim | built

TREE     a mu_aloha_platforms checkout (default: mu_aloha_platforms/ here, or $MUALOHA)
DEVICE   a folder of Platforms/<Pkg>/Device/, the phone's name there, e.g. polaris
         (default: $MUALOHA_DEVICE, else polaris)
MODE     RELEASE or DEBUG (default: RELEASE)

Nothing about the firmware is written down here: the package comes from where the device folder is, and the
SoC (which names the FD) and the shim's base and size from build_cfg/<soc>.json -- what mu_aloha_platforms'
own build_uefi.py reads. So a device it learns to build is ours to pack at once.
"""
import argparse, glob, json, os, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAYLOAD_BASE = 0xF0000000          # where the boot shim drops the "XNUP" payload for the loader


def where(path):
    """PATH for a message: relative when it is under this repository, absolute otherwise."""
    rel = os.path.relpath(path, HERE)
    return path if rel.startswith('..') else rel


class Problem(Exception):
    pass


class Target:
    def __init__(self, tree, device, package, soc, base, size, devdir):
        self.tree, self.device, self.package, self.soc = tree, device, package, soc
        self.base, self.size, self.devdir = base, size, devdir

    def path(self, kind, mode):
        if kind == 'fd':
            # build_uefi.py names it after the SoC of build_cfg/<soc>.json: SM7325_EFI.fd, SDM845_EFI.fd
            return os.path.join(self.tree, 'Build', self.package, '%s_CLANGPDB' % mode, 'FV',
                                '%s_EFI.fd' % self.soc.upper())
        if kind == 'dtb':
            found = sorted(glob.glob(os.path.join(self.devdir, 'DeviceTreeBlob', 'Android', '*.dtb')))
            if not found:
                raise Problem('%s has no DeviceTreeBlob/Android/*.dtb' % os.path.relpath(self.devdir, self.tree))
            named = [f for f in found if self.device in os.path.basename(f)]
            return (named or found)[0]
        if kind == 'ramdisk':
            return os.path.join(self.tree, 'PythonLibs', 'emptyramdisk')
        if kind == 'mkbootimg':
            return os.path.join(self.tree, 'PythonLibs', 'mkbootimg.py')
        raise Problem(kind)

    def values(self, mode):
        return [('device', self.device), ('package', self.package), ('soc', self.soc),
                ('base', '%#x' % self.base), ('size', '%#x' % self.size),
                ('fd', self.path('fd', mode)), ('dtb', self.path('dtb', mode)),
                ('ramdisk', self.path('ramdisk', mode)), ('mkbootimg', self.path('mkbootimg', mode))]


def resolve(tree, device):
    if not os.path.isdir(tree):
        raise Problem('%s is not there: uefi/apply.sh makes it (uefi/README.md)' % where(tree))
    found = []
    for path in sorted(glob.glob(os.path.join(tree, 'Platforms', '*', 'Device', '*'))):
        name = os.path.basename(path)
        if name == device or name.endswith('-' + device) or name.endswith('_' + device):
            found.append(path)
    if not found:
        known = sorted(set(os.path.basename(p).split('-', 1)[-1]
                           for p in glob.glob(os.path.join(tree, 'Platforms', '*', 'Device', '*'))))
        raise Problem('%s has no device %r (it has: %s)' % (where(tree), device,
                                                           ', '.join(known) or 'none'))
    devdir = found[0]
    package = os.path.basename(os.path.dirname(os.path.dirname(devdir)))

    # build_cfg/<soc>.json: one per package, naming both the SoC and the shim's base and size
    base = size = soc = None
    for path in sorted(glob.glob(os.path.join(tree, 'build_cfg', '*.json'))):
        try:
            cfg = json.load(open(path))
        except ValueError:
            continue
        if cfg.get('package') == package and cfg.get('bootshim'):
            soc = cfg['platform']
            base = int(str(cfg['bootshim']['UEFI_BASE']), 0)
            size = int(str(cfg['bootshim']['UEFI_SIZE']), 0)
            break
    if base is None:
        raise Problem('build_cfg/ has no entry for %s: its UEFI_BASE and UEFI_SIZE are unknown' % package)
    return Target(tree, os.path.basename(devdir).split('-', 1)[-1], package, soc, base, size, devdir)


def objcopy_of_the_host():
    """llvm-objcopy on PATH, else the one Homebrew's llvm keeps (a Mac's Xcode has none of its own)."""
    found = shutil.which('llvm-objcopy')
    if found:
        return found
    for prefix in ('/opt/homebrew/opt/llvm/bin', '/usr/local/opt/llvm/bin'):
        path = os.path.join(prefix, 'llvm-objcopy')
        if os.path.isfile(path):
            return path
    return None


def elf_to_bin(elf, binary):
    """What "objcopy -O binary" does for the shim, so that a Mac needs no llvm-objcopy (Xcode has none).

    BootShim.elf is a relocatable object (clang -c, no linker runs): its allocatable sections sit at address 0 and
    the image is their bytes, in order. A linked image is laid out at the addresses it was linked for.
    """
    data = open(elf, 'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        raise Problem('%s is not a 64-bit little-endian ELF' % elf)
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x3a)
    sections = []
    for i in range(shnum):
        _, typ, flags, addr, offset, size, _, _, align, _ = struct.unpack_from('<IIQQQQIIQQ', data,
                                                                              shoff + i * shentsize)
        if typ == 8 or not flags & 2:          # SHT_NOBITS, or not SHF_ALLOC: not part of the image
            continue
        sections.append((addr, align, data[offset:offset + size]))
    if not sections:
        raise Problem('%s has nothing to load' % elf)
    base = min(addr for addr, _, _ in sections)
    out = bytearray()
    if base:                                   # a linked image: at the addresses it was linked for
        out = bytearray(max(addr + len(body) for addr, _, body in sections) - base)
        for addr, _, body in sections:
            out[addr - base:addr - base + len(body)] = body
    else:                                      # relocatable (clang -c): one after the other
        for _, align, body in sections:
            out += b'\0' * (-len(out) % (align or 1))
            out += body
    open(binary, 'wb').write(bytes(out))


def build_shim(t, payload_base=PAYLOAD_BASE, cc=None):
    """BootShim.bin, for this target's base and size, with the payload copy HakiOS adds (uefi/hakios.patch)."""
    shim_dir = os.path.join(t.tree, 'BootShim')
    if not os.path.isfile(os.path.join(shim_dir, 'BootShim.S')):
        raise Problem('%s has no BootShim/BootShim.S' % t.tree)
    for name in ('BootShim.elf', 'BootShim.bin'):
        try:
            os.unlink(os.path.join(shim_dir, name))
        except FileNotFoundError:
            pass
    cmd = ['make', 'UEFI_BASE=%#x' % t.base, 'UEFI_SIZE=%#x' % t.size,
           'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE=%#x' % payload_base]
    if cc:
        cmd.append('CC=' + cc)
    objcopy = os.environ.get('MUALOHA_OBJCOPY') or objcopy_of_the_host()
    if objcopy:
        cmd.append('OBJCOPY=' + objcopy)
    else:
        cmd.append('BootShim.elf')      # no llvm-objcopy here (a Mac, Xcode's toolchain): the ELF is the goal
    r = subprocess.run(cmd, cwd=shim_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise Problem('the boot shim did not build:\n%s' % r.stdout.strip())
    shim = os.path.join(shim_dir, 'BootShim.bin')
    if not os.path.isfile(shim):
        elf_to_bin(os.path.join(shim_dir, 'BootShim.elf'), shim)
    if os.path.getsize(shim) % 16:
        raise Problem('%s is not a multiple of 16 bytes' % shim)
    return shim


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('-t', '--tree', default=os.environ.get('MUALOHA') or os.path.join(HERE, 'mu_aloha_platforms'))
    ap.add_argument('-d', '--device', default=os.environ.get('MUALOHA_DEVICE') or 'polaris')
    ap.add_argument('-m', '--mode', default=os.environ.get('MUALOHA_MODE') or 'RELEASE')
    ap.add_argument('command', choices=('target', 'fd', 'dtb', 'ramdisk', 'mkbootimg', 'shim', 'built', 'target-device'))
    a = ap.parse_args()
    try:
        t = resolve(a.tree, a.device)
        if a.command == 'built':
            return 0 if os.path.isfile(t.path('fd', a.mode)) else 1
        if a.command == 'shim':
            print(build_shim(t))
        elif a.command == 'target-device':
            print(os.path.basename(t.devdir))       # what build_uefi.py calls the device: xiaomi-polaris
        elif a.command == 'target':
            for k, v in t.values(a.mode):
                print('%s=%s' % (k, v))
        else:
            print(t.path(a.command, a.mode))
    except Problem as e:
        sys.exit('tools/mualoha.py: %s' % e)
    return 0


if __name__ == '__main__':
    sys.exit(main())
