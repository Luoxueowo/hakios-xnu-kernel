#!/usr/bin/env python3
"""The Apple logo HakiOS's UEFI shows in place of the one mu_aloha_platforms ships: a white apple silhouette on black,
because the phone's screen is black when the firmware draws it. Drawn here from smooth curves, with nothing to install,
so that anyone can change it and make it again.

The file is the one Frontpage.fdf.inc embeds as AndromedaPkg/Include/Resources/BootLogo.bmp: an 8-bit BMP with a
256-shade grey palette, 236x236.

usage: branding/banner.py OUTDIR [SIZE]        (uefi/apply.sh runs it into the tree; SIZE defaults to 236)
"""
import math
import os
import struct
import sys

SS = 4                                          # drawn this many times larger, then averaged
SIZE = 236                                      # what AndromedaPkg/Include/Resources/BootLogo.bmp is


class Canvas:
    def __init__(self, size):
        self.size, self.n = size, size * SS
        self.px = [(0, 0, 0)] * (self.n * self.n)

    def fill(self, points, c):
        """Fill a closed polygon using scanlines at the supersampled resolution."""
        n = self.n
        edges = list(zip(points, points[1:] + points[:1]))
        min_y = max(0, int(min(y for _, y in points)))
        max_y = min(n, int(max(y for _, y in points)) + 1)
        for y in range(min_y, max_y):
            scan_y = y + .5
            intersections = []
            for (x0, y0), (x1, y1) in edges:
                if (y0 <= scan_y < y1) or (y1 <= scan_y < y0):
                    intersections.append(x0 + (scan_y - y0) * (x1 - x0) / (y1 - y0))
            intersections.sort()
            for left, right in zip(intersections[::2], intersections[1::2]):
                start = max(0, math.ceil(left - .5))
                end = min(n, math.ceil(right - .5))
                for x in range(start, end):
                    self.px[y * n + x] = c

    def apple(self, cx, top, size):
        """Draw a white Apple silhouette, fitted to a 64-unit square."""
        u = size * SS / 64
        ox, oy = cx * SS - 32 * u, top * SS
        p = lambda x, y: (ox + x * u, oy + y * u)

        def path(segments, k=96):
            points = []
            for p0, p1, p2, p3 in segments:
                for i in range(k):
                    t = i / k
                    q = 1 - t
                    points.append(p(q ** 3 * p0[0] + 3 * q ** 2 * t * p1[0] +
                                    3 * q * t ** 2 * p2[0] + t ** 3 * p3[0],
                                    q ** 3 * p0[1] + 3 * q ** 2 * t * p1[1] +
                                    3 * q * t ** 2 * p2[1] + t ** 3 * p3[1]))
            return points

        white = (255, 255, 255)
        body = [
            ((32, 23), (35, 19), (41, 16), (47, 18)),
            ((47, 18), (52, 20), (55, 23), (56, 27)),
            # The inward curve and its return make the bite on the right edge.
            ((56, 27), (51, 27), (47, 30), (47, 34)),
            ((47, 34), (47, 38), (52, 40), (57, 42)),
            ((57, 42), (55, 51), (50, 59), (43, 63)),
            ((43, 63), (39, 65), (36, 61), (33, 61)),
            ((33, 61), (30, 61), (27, 64), (23, 63)),
            ((23, 63), (14, 61), (6, 49), (6, 37)),
            ((6, 37), (6, 26), (11, 18), (20, 18)),
            ((20, 18), (25, 17), (29, 20), (32, 23)),
        ]
        leaf = [
            ((34, 18), (33.5, 11), (37.5, 5.5), (47, 4)),
            ((47, 4), (47, 11.5), (43, 17), (36, 19.5)),
            ((36, 19.5), (35, 19.5), (34.5, 19), (34, 18)),
        ]
        self.fill(path(body), white)
        self.fill(path(leaf), white)

    def bmp(self, path):
        """An 8-bit BMP (BITMAPINFOHEADER, greyscale palette, rows bottom-up, each a multiple of 4 bytes)."""
        size, n = self.size, self.n
        palette = [(i, i, i) for i in range(256)]
        index = bytearray(size * size)
        for y in range(size):
            for x in range(size):
                acc = [0, 0, 0]
                for dy in range(SS):
                    base = (y * SS + dy) * n + x * SS
                    for dx in range(SS):
                        q = self.px[base + dx]
                        acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]
                index[y * size + x] = round((acc[0] + acc[1] + acc[2]) / (3 * SS * SS))
        stride = (size + 3) & ~3
        pal = b"".join(struct.pack("<BBBB", b, g, r, 0) for r, g, b in palette)
        rows = b"".join(bytes(index[y * size:(y + 1) * size]) + b"\0" * (stride - size) for y in reversed(range(size)))
        offset = 14 + 40 + len(pal)
        header = struct.pack("<2sIHHI", b"BM", offset + len(rows), 0, 0, offset)
        info = struct.pack("<IiiHHIIiiII", 40, size, size, 1, 8, 0, len(rows), 3779, 3779, 256, 256)
        with open(path, "wb") as f:
            f.write(header + info + pal + rows)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    out, size = sys.argv[1], int(sys.argv[2]) if len(sys.argv) == 3 else SIZE
    os.makedirs(out, exist_ok=True)

    logo = Canvas(size)
    logo.apple(size // 2, size // 5, size * 3 // 5)
    path = os.path.join(out, "BootLogo.bmp")
    logo.bmp(path)
    print("banner: %s (%dx%d)" % (path, size, size))


if __name__ == "__main__":
    main()
