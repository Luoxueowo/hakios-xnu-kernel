#!/bin/bash
# Build Mesa for this machine (x86-64 Linux or macOS) with the AIR frontend in it: the
# frontend itself, ir3 (so a Metal shader can be compiled for the Adreno
# without the phone) and softpipe (so it can be run without a GPU at all).
#
# This is the frontend's development and test build. The one that goes on the
# phone is tools/mk-mesa.sh, which cross-compiles for Darwin; both take the
# frontend from tools/mesa/air, so what is tested here is what runs there.
#
# Nothing of Mesa is in the repository: the pinned release is fetched, patched
# with tools/mesa/*.patch, and src/compiler/air is a symbolic link back to
# tools/mesa/air so that editing the frontend needs no copying.
#
# What comes out, in build/mesa-host/build:
#   src/compiler/air/libair.a, src/freedreno/ir3/libfreedreno_ir3.a, and the NIR/util/device archives used by the
#   Metal driver's host shader sweep. On Linux the optional Mesa command-line tools and air_run are built as before.
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
BISON_VERSION=3.8.2
BISON_SHA256=9bba0214ccf7f1079c5d59210045227bcf619519840ebfa80cd3849cff5a5bf2
if [ "$(uname)" = Darwin ]; then
	HASH=(shasum -a 256)
	HASH_CHECK=(shasum -a 256 -c)
else
	HASH=(sha256sum)
	HASH_CHECK=(sha256sum -c)
fi
B=$ROOT/build/mesa-host
SRC=$B/mesa-$VERSION
mkdir -p "$B"
cd "$B"

# air-*.patch is the frontend and the ir3 entry point; ir3-*.patch is a fix to ir3 itself, and it
# belongs here as much as in the cross build -- this tool is where the corpus's "compiled by ir3"
# column comes from and where its Adreno instructions are read, so a host ir3 that differs from the
# device's would make both of those say something about a compiler nothing runs. The other patches
# in tools/mesa are for the Darwin cross build and are deliberately left out.
PATCHES=$(cat "$ROOT"/tools/mesa/air*.patch "$ROOT"/tools/mesa/ir3-*.patch | "${HASH[@]}" | cut -d' ' -f1)
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.air-patches" 2>/dev/null)" != "$PATCHES" ]; then
	if [ ! -f mesa-$VERSION.tar.xz ]; then
		# the cross build may already have fetched it
		if [ -f "$ROOT/xnu-deps/build/mesa/mesa-$VERSION.tar.xz" ]; then
			cp "$ROOT/xnu-deps/build/mesa/mesa-$VERSION.tar.xz" .
		else
			curl -sL --fail -o mesa-$VERSION.tar.xz \
				https://archive.mesa3d.org/mesa-$VERSION.tar.xz
		fi
	fi
	echo "$SHA256  mesa-$VERSION.tar.xz" | "${HASH_CHECK[@]}" --quiet
	rm -rf "$SRC" build
	tar xJf mesa-$VERSION.tar.xz
	for p in "$ROOT"/tools/mesa/air-*.patch "$ROOT"/tools/mesa/ir3-*.patch; do
		patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p"
	done

	# glibc 2.43 and later declare C11's once_flag and call_once in <stdlib.h>,
	# which clashes with Mesa 25.0's own definitions in src/c11/threads.h. Only
	# this host build hits it; the Darwin cross build has its own libc. Keep the
	# glibc feature test nested so non-glibc preprocessors do not parse its macro.
	python3 - "$SRC" <<-'PY'
		import sys
		root = sys.argv[1]
		p = root + '/src/c11/threads.h'
		s = open(p).read()
		old = "typedef pthread_once_t  once_flag;\n#  define ONCE_FLAG_INIT PTHREAD_ONCE_INIT\n"
		new = ("#  if defined(__GLIBC__)\n"
		       "#    if __GLIBC_USE(ISOC23) && !defined(__cplusplus)\n"
		       "#    define C11_LIBC_HAS_CALL_ONCE 1\n"
		       "#  else\n"
		       "typedef pthread_once_t  once_flag;\n"
		       "#    define ONCE_FLAG_INIT PTHREAD_ONCE_INIT\n"
		       "#    endif\n"
		       "#  else\n"
		       "typedef pthread_once_t  once_flag;\n"
		       "#    define ONCE_FLAG_INIT PTHREAD_ONCE_INIT\n"
		       "#  endif\n")
		if old in s and 'C11_LIBC_HAS_CALL_ONCE' not in s:
		    open(p, 'w').write(s.replace(old, new))
		p = root + '/src/c11/impl/threads_posix.c'
		s = open(p).read()
		old = "call_once(once_flag *flag, void (*func)(void))\n{\n    pthread_once(flag, func);\n}\n"
		if old in s and 'C11_LIBC_HAS_CALL_ONCE' not in s:
		    i = s.index(old)
		    j = s.rfind('\n\n', 0, i)
		    s = (s[:j] + "\n\n#ifndef C11_LIBC_HAS_CALL_ONCE" + s[j:i + len(old)] +
		         "#endif\n" + s[i + len(old):])
		    open(p, 'w').write(s)
	PY

	# the frontend itself, as a link: no copy to forget to refresh. The one
	# line of meson that reaches it comes from tools/mesa/air-frontend.patch,
	# applied above, so this build and the cross build get it the same way.
	rm -rf "$SRC/src/compiler/air"
	ln -s "$ROOT/tools/mesa/air" "$SRC/src/compiler/air"

	echo "$PATCHES" > "$SRC/.air-patches"
fi

# Mach-O has no ELF build-id note for Mesa's shader disk-cache key. The same
# Darwin guard used by the device build disables this cache on the host; shader
# compilation and the QuartzCore sweep do not need it.
if [ "$(uname)" = Darwin ]; then
	python3 - "$SRC/src/freedreno/ir3/ir3_disk_cache.c" <<-'PY'
		import sys
		p = sys.argv[1]
		s = open(p).read()
		old = """   if (ir3_shader_debug & IR3_DBG_NOCACHE)
	      return;

	   const char *renderer = fd_dev_name(compiler->dev_id);
"""
		new = """   if (ir3_shader_debug & IR3_DBG_NOCACHE)
	      return;

	#ifdef __APPLE__
	   /* Mach-O has no ELF build-id note; this host compiler does not need a disk cache. */
	   return;
	#else
	   const char *renderer = fd_dev_name(compiler->dev_id);
"""
		section = s[s.index('ir3_disk_cache_init('):s.index('ir3_disk_cache_init_shader_key(')]
		if '#ifdef __APPLE__' not in section:
		    if old not in s:
		        raise SystemExit('could not find the ir3 cache init guard location')
		    s = s.replace(old, new, 1)
		    old = "   compiler->disk_cache = disk_cache_create(renderer, timestamp, driver_flags);\n}"
		    new = "   compiler->disk_cache = disk_cache_create(renderer, timestamp, driver_flags);\n#endif\n}"
		    if old not in s:
		        raise SystemExit('could not find the ir3 cache init end')
		    s = s.replace(old, new, 1)
		    open(p, 'w').write(s)
	PY
fi

if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
fi
if [ ! -x venv/bin/meson ] || [ ! -x venv/bin/ninja ]; then
	venv/bin/pip -q install meson ninja mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH

# macOS ships an old Bison 2.3, while Mesa 25's ir3 parser uses %code. Keep a
# verified, local Bison for this host build instead of changing system tools.
if [ "$(uname)" = Darwin ]; then
	SYSTEM_BISON=$(command -v bison || true)
	SYSTEM_BISON_MAJOR=0
	if [ -n "$SYSTEM_BISON" ]; then
		SYSTEM_BISON_MAJOR=$($SYSTEM_BISON --version | sed -n '1s/.*) //p' | cut -d. -f1)
	fi
	if [ "${SYSTEM_BISON_MAJOR:-0}" -lt 3 ]; then
		BT=$B/toolchain/bison-$BISON_VERSION
		if [ ! -x "$BT/bin/bison" ]; then
			mkdir -p "$B/toolchain"
			if [ ! -f "$B/bison-$BISON_VERSION.tar.xz" ]; then
				curl -fsSL -o "$B/bison-$BISON_VERSION.tar.xz" \
					https://ftpmirror.gnu.org/bison/bison-$BISON_VERSION.tar.xz
			fi
			echo "$BISON_SHA256  $B/bison-$BISON_VERSION.tar.xz" | "${HASH_CHECK[@]}" --quiet
			tar xJf "$B/bison-$BISON_VERSION.tar.xz" -C "$B/toolchain"
			(cd "$B/toolchain/bison-$BISON_VERSION" && ./configure --prefix="$BT" > "$B/bison-configure.log" && \
				make -j"${BUILD_JOBS:-4}" > "$B/bison-build.log" && make install >> "$B/bison-build.log")
		fi
		PATH=$BT/bin:$PATH
		export PATH
	fi
fi

# Meson records the parser generator's resolved path in build.ninja. If an
# earlier configure cached Apple's Bison 2.3, reconfigure in a fresh generated
# directory so the verified local Bison is actually used by Ninja.
BISON_BIN=$(command -v bison || true)
if [ -f build/build.ninja ] && [ -n "$BISON_BIN" ] && ! grep -Fq "$BISON_BIN" build/build.ninja; then
	OLD_BUILD=build-bison-stale
	[ ! -e "$OLD_BUILD" ] || OLD_BUILD="$OLD_BUILD-$(date +%s)"
	mv build "$OLD_BUILD"
fi

if [ "$(uname)" = Darwin ]; then
	GALLIUM=softpipe
	TOOLS=freedreno
else
	GALLIUM=softpipe,freedreno
	TOOLS=
fi

[ -f build/build.ninja ] || meson setup build "$SRC" --buildtype=debugoptimized \
	-Dplatforms= -Dgallium-drivers="$GALLIUM" -Dtools="$TOOLS" -Dfreedreno-kmds=msm \
	-Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
	-Dopengl=true -Dgles2=enabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= \
	-Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled \
	-Dlmsensors=disabled -Dbuild-tests=true -Dzstd=disabled -Dxmlconfig=disabled \
	-Dexpat=disabled > meson-setup.log

if [ "$(uname)" = Darwin ]; then
	ninja -C build \
		src/compiler/air/libair.a \
		src/freedreno/ir3/libfreedreno_ir3.a \
		src/freedreno/isa/libir3encode.a \
		src/freedreno/isa/libir3decode.a \
		src/compiler/isaspec/libisaspec.a \
		src/compiler/nir/libnir.a \
		src/compiler/libcompiler.a \
		src/freedreno/fdl/libfreedreno_layout.a \
		src/freedreno/common/libfreedreno_common.a \
		src/util/libmesa_util.a \
		src/util/libmesa_util_sse41.a \
		src/util/blake3/libblake3.a \
		src/c11/impl/libmesa_util_c11.a > ninja.log
	echo "mk-host.sh: host shader-compiler archives in $B/build"
else
	ninja -C build src/compiler/air/air2nir \
		src/gallium/drivers/freedreno/ir3_compiler \
		src/gallium/drivers/freedreno/air_run > ninja.log
	echo "mk-host.sh: $B/build/src/compiler/air/air2nir"
	echo "mk-host.sh: $B/build/src/gallium/drivers/freedreno/ir3_compiler"
	echo "mk-host.sh: $B/build/src/gallium/drivers/freedreno/air_run"
fi
