#!/usr/bin/env python3
"""Choose the part of the Darwin root filesystem that goes into the in-memory system image.

usage: select-system.py rootfs.slt system.files system.manifest

rootfs.slt is the technical listing of the root filesystem image ("7z l -slt rootfs.dmg").
Two files are written:
  system.files      the regular files to extract ("7z x -orootfs rootfs.dmg @system.files")
  system.manifest   every chosen file, directory and symbolic link with its owner, group and mode,
                    tab separated: mode(octal, with the type bits) uid gid path [link target].
                    tools/apfs/mkapfs.py builds the image from the extracted files and this manifest,
                    because extraction on Linux keeps neither owners nor all of the symlinks.

The whole root is 7.9 GB, and the image is now taken with the Chinese localizations, every font, every
sound, the input methods' dictionaries and the GPU's shader libraries in it (about 4.7 GB), so:
  - programs, libraries and their data outside the framework directories are taken whole;
  - from the framework directories (the code itself is in the dyld shared cache) what a
    program may open at run time and is not huge: property lists, string tables, resources,
    plug-ins, and the XPC services and helper executables bundled with frameworks;
  - the applications (the home screen is made of their icons), media and all; those that Darwin
    keeps in /private/var/staged_system_apps until its first boot installs them (Maps, Notes,
    Weather, ...) are put straight into /Applications, where they are system applications like
    the rest: system.files names them where they are in the image, system.manifest where they go
    (userland/build.sh moves the extracted files), media and all;
  - machine-learning models are left out, and so are a few very big daemons that have nothing to do
    with the display; every language of English and Chinese, every font (colour emoji and PingFang
    whole), every sound (the system's, the ringtones, the audio plug-ins and this model's tunings),
    the input methods' dictionaries and the GPU shader libraries are all taken;
  - all of /Library (the wallpapers, regional tables, sounds);
  - of pictures, everything the applications draw with, whatever its size;
  - the empty LANGUAGE.lproj directories of the bundles whose strings are in one .loctable file
    for all languages: they are how CoreFoundation knows which languages a bundle has, and
    without them every string comes out as its key.
"""
import re
import stat
import sys

WHOLE = ('usr/libexec/', 'usr/sbin/', 'usr/bin/', 'usr/lib/', 'usr/share/', 'sbin/', 'bin/', 'private/etc/',
         'System/Library/', 'Applications/', 'Library/')
# ... except these parts of /System/Library: big, and not needed to get a picture on the screen
# (not OnBoardingBundles, the privacy notices: an application's first screen links to its notice, and
# News refuses to start without its own -- "a legal requirement"; nor Health, the plug-ins the Health
# application and its daemon are made of: both stop with a fatal error when the directory is not there.
# Neither LinguisticData, the input methods' dictionaries, nor Audio: both are taken whole now.)
SKIP_SYSTEM_LIBRARY = ('PreinstalledAssetsV2', 'NanoTimeKit',
                       'Assistant', 'AccessibilityBundles',
                       'VideoProcessors', 'ScreenReader', 'Photos', 'Carrier Bundles', 'NanoPreferenceBundles',
                       'ImagingNetworks', 'ProductDocuments', 'AppPlaceholders', 'UserNotifications',
                       'Caches')
FRAMEWORKS = ('System/Library/Frameworks/', 'System/Library/PrivateFrameworks/')
SMALL = 4 << 20
# never from frameworks: machine-learning models and stores.
# (Not the data models, .mom: an application that keeps its data with Core Data dies without its
# model -- Notes did; nor the animations and vector pictures the interface is drawn with.)
SKIP_EXT = ('.weights', '.weights_nonane', '.espresso.net', '.espresso.shape', '.ds', '.dat', '.bin')
# taken whatever their size: the media an application or a framework draws with, and the GPU shader
# libraries (a .metallib is 10-100 MB and is what Metal compiles against at run time)
MEDIA_EXT = ('.mov', '.mp4', '.m4a', '.caf', '.wav', '.aiff', '.heic', '.usdz', '.scnz', '.art',
             '.scube', '.pcm')
# ... but '.dat' and '.bin' are also what these keep the tables in that the user interface is made with:
# the layouts of the on-screen keyboard, the emoji, the list of public domain suffixes
DATA_FRAMEWORKS = tuple(f'System/Library/{name}.framework/' for name in (
    'PrivateFrameworks/TextInputUI', 'PrivateFrameworks/TextInput', 'PrivateFrameworks/CoreEmoji',
    'Frameworks/CFNetwork'))
# pictures the user interface is drawn with: small images, and asset catalogs up to this size
# (the one with all the SF Symbols alone is 170 MB and is left out)
MAX_ASSET_CATALOG = 4 << 20
# fonts: every one, whole -- the colour emoji (130 MB) and PingFang, the Chinese faces (74 MB), included,
# no trimming after extraction. Without them Safari and the rest drew emoji and Chinese characters as
# boxes with a question mark.
# string tables hold every language; the biggest (Shortcuts' actions, Home) are 5-12 MB
MAX_STRING_TABLE = 16 << 20
# applications waiting here to be installed by the first boot go to /Applications instead
STAGED_APPS = 'private/var/staged_system_apps/'
# frameworks whose pictures are wanted whatever their size
ALL_PICTURES = ('System/Library/PrivateFrameworks/ProgressUI.framework/',)
# ... and asset catalogs: the SF Symbols (UIKit crashes on the first system image without them)
BIG_CATALOGS = ('System/Library/PrivateFrameworks/SFSymbols.framework/',)
# taken whole in principle, but not these: big and of no use until much later
# (/Library -- the wallpapers and the regional settings' tables -- and its sounds are taken whole)
HEALTH = 'System/Library/Health/'
SKIP_PREFIX = ('usr/share/firmware/', 'usr/standalone/') + \
    tuple(f'System/Library/{name}/' for name in SKIP_SYSTEM_LIBRARY)
BIG_PROGRAM = 10 << 20           # daemons bigger than this (locationd, lskdd, ...) are not on the way to the screen
# every language of English and Chinese: the .lproj directories whose strings and pictures are taken.
# (Chinese: the display names this root filesystem has -- zh-Hans, zh_CN, zh_TW, zh_HK.)
KEEP_LANG = ('en', 'Base', 'English', 'en_US', 'en_GB', 'zh-Hans', 'zh-Hant', 'zh_CN', 'zh_TW', 'zh_HK')


def entries(listing):
    """Yield one dict per entry of a 7z technical listing."""
    entry = {}
    for line in open(listing, errors='replace'):
        line = line.rstrip('\n')
        if not line:
            if 'Path' in entry and 'Mode' in entry:
                yield entry
            entry = {}
        elif ' = ' in line or line.endswith(' ='):
            key, _, value = line.partition(' =')
            entry[key] = value[1:] if value.startswith(' ') else value
    if 'Path' in entry and 'Mode' in entry:
        yield entry


def mode_bits(text):
    """'-rwsr-xr-x' -> st_mode"""
    kinds = {'-': stat.S_IFREG, 'd': stat.S_IFDIR, 'l': stat.S_IFLNK}
    if text[0] not in kinds:
        return None
    mode = kinds[text[0]]
    for i, ch in enumerate(text[1:10]):
        if ch in 'rwxst':
            mode |= 0o400 >> i
        if ch in 'sS' and i == 2:
            mode |= stat.S_ISUID
        if ch in 'sS' and i == 5:
            mode |= stat.S_ISGID
        if ch in 'tT' and i == 8:
            mode |= stat.S_ISVTX
        if ch in 'ST':
            mode &= ~(0o400 >> i)
    return mode


def wanted_file(path, size):
    lproj = re.search(r'/([^/]+)\.lproj/', path)
    if lproj and lproj.group(1) not in KEEP_LANG:
        return False
    if path.startswith(SKIP_PREFIX):
        return False
    if path.startswith('System/Library/Fonts/'):
        return True
    if path.startswith(ALL_PICTURES) and path.lower().endswith('.png'):
        return True
    if path.startswith(('usr/libexec/', 'usr/sbin/')):
        return size <= BIG_PROGRAM
    if path.startswith(('Applications/', HEALTH)):
        return '.mlmodelc/' not in path
    if path.startswith(FRAMEWORKS):
        if re.search(r'/(XPCServices|Support|Helpers)/', path):
            return size <= 32 << 20 and not path.lower().endswith(('.png', '.car', '.loctable', '.caml'))
        if path.endswith('.car'):
            return size <= MAX_ASSET_CATALOG or path.startswith(BIG_CATALOGS)
        if path.endswith('.loctable'):
            return size <= MAX_STRING_TABLE
        if path.startswith(DATA_FRAMEWORKS) and path.lower().endswith(('.dat', '.bin')):
            return size <= SMALL
        # media and the GPU's shader libraries, whatever their size
        if path.lower().endswith(MEDIA_EXT) or path.lower().endswith('.metallib'):
            return True
        return size <= SMALL and not path.lower().endswith(SKIP_EXT) and '.mlmodelc/' not in path
    return path.startswith(WHOLE)


def main():
    listing, files_out, manifest_out = sys.argv[1:4]
    meta = {}                   # path in our image -> (mode, uid, gid, target, size)
    source = {}                 # ... -> path in Apple's image, where that is another
    for e in entries(listing):
        mode = mode_bits(e['Mode'])
        path = e['Path']
        if mode is None:
            continue
        if path.startswith(STAGED_APPS):
            source['Applications/' + path[len(STAGED_APPS):]] = path
            path = 'Applications/' + path[len(STAGED_APPS):]
        elif path.startswith('private/var/'):
            continue
        meta[path] = (mode, int(e.get('User ID') or 0), int(e.get('Group ID') or 0),
                      e.get('Symbolic Link', ''), int(e.get('Size') or 0))

    chosen = set()
    for path, (mode, uid, gid, target, size) in meta.items():
        if stat.S_ISREG(mode):
            keep = wanted_file(path, size)
        elif stat.S_ISLNK(mode):
            # a symlink costs nothing: keep every one that lives where we take anything at all
            keep = '/' not in path or path.startswith(WHOLE) and not path.startswith(SKIP_PREFIX)
        else:
            # (the sound plug-ins' too: without its en.lproj the virtual audio device's port names came
            # out NULL -- mediaserverd died naming the microphone)
            lproj = re.search(r'/([^/]+)\.lproj$', path)
            keep = '/' not in path or path in ('private/var', 'private/tmp', 'private/etc', 'private/preboot',
                                              'private/xarts', 'private') or \
                bool(lproj) and lproj.group(1) in KEEP_LANG and \
                path.startswith(WHOLE) and not path.startswith(SKIP_PREFIX)
        if keep:
            chosen.add(path)
    # every directory on the way to something chosen
    for path in list(chosen):
        while '/' in path:
            path = path.rsplit('/', 1)[0]
            chosen.add(path)

    total = count = 0
    with open(files_out, 'w') as files, open(manifest_out, 'w') as manifest:
        for path in sorted(chosen):
            if path not in meta:
                continue
            mode, uid, gid, target, size = meta[path]
            manifest.write(f'{mode:o}\t{uid}\t{gid}\t{path}\t{target if stat.S_ISLNK(mode) else ""}\n')
            if stat.S_ISREG(mode):
                files.write(source.get(path, path) + '\n')
                total += size
                count += 1
    print(f'select-system: {count} files, {total / 1048576:.0f} MB; {len(chosen)} entries in the manifest')


if __name__ == '__main__':
    main()
