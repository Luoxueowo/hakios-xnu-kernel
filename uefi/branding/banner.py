#!/usr/bin/env python3
"""Render a white Apple logo on black as an 8-bit, greyscale BootLogo.bmp.

Usage: branding/banner.py OUTDIR [SIZE] (default: 236 pixels square).
uefi/apply.sh invokes this generator for the firmware resource directory.
No external libraries or network access are needed to generate the bitmap.
"""
import math
import os
import struct
import sys

SIZE = 236
SS = 4
COVERAGE = .90  # The actual outline occupies 90% of the canvas height.

# Apple outline from Simple Icons (CC0), converted to absolute cubic curves.
# https://github.com/simple-icons/simple-icons/blob/develop/icons/apple.svg
# Each contour contains its start point followed by (control1, control2, end).
APPLE_CONTOURS = (
    (
        (12.152, 6.896),
        ((11.204, 6.896), (9.737, 5.818), (8.192, 5.856)),
        ((6.152, 5.883), (4.282, 7.039), (3.231, 8.87)),
        ((1.114, 12.545), (2.685, 17.973), (4.75, 20.96)),
        ((5.763, 22.414), (6.958, 24.05), (8.542, 23.999)),
        ((10.062, 23.934), (10.632, 23.012), (12.477, 23.012)),
        ((14.308, 23.012), (14.827, 23.999), (16.437, 23.96)),
        ((18.074, 23.934), (19.113, 22.48), (20.113, 21.012)),
        ((21.269, 19.324), (21.749, 17.687), (21.775, 17.597)),
        ((21.736, 17.584), (18.593, 16.376), (18.555, 12.74)),
        ((18.529, 9.7), (21.035, 8.246), (21.152, 8.181)),
        ((19.723, 6.091), (17.529, 5.857), (16.762, 5.805)),
        ((14.762, 5.649), (13.087, 6.895), (12.152, 6.895)),
    ),
    (
        (15.53, 3.83),
        ((16.373, 2.818), (16.93, 1.403), (16.775, 0.0)),
        ((15.568, 0.052), (14.113, 0.805), (13.243, 1.818)),
        ((12.463, 2.714), (11.789, 4.156), (11.97, 5.532)),
        ((13.308, 5.636), (14.685, 4.844), (15.529, 3.831)),
    ),
)


def outline():
    """Flatten the source curves without changing the logo's proportions."""
    contours = []
    for source in APPLE_CONTOURS:
        point = source[0]
        points = [point]
        for control1, control2, end in source[1:]:
            for step in range(1, 97):
                t = step / 96
                q = 1 - t
                points.append(tuple(q ** 3 * a + 3 * q ** 2 * t * b +
                                    3 * q * t ** 2 * c + t ** 3 * d
                                    for a, b, c, d in zip(point, control1, control2, end)))
            point = end
        contours.append(points)
    return contours


def render(size):
    """Center the real outline bounds and rasterize with supersampling."""
    contours = outline()
    all_points = [point for contour in contours for point in contour]
    left = min(x for x, y in all_points)
    right = max(x for x, y in all_points)
    top = min(y for x, y in all_points)
    bottom = max(y for x, y in all_points)
    n = size * SS
    scale = n * COVERAGE / max(right - left, bottom - top)
    ox = (n - (right - left) * scale) / 2 - left * scale
    oy = (n - (bottom - top) * scale) / 2 - top * scale
    pixels = bytearray(n * n)
    for contour in contours:
        points = [(ox + x * scale, oy + y * scale) for x, y in contour]
        edges = list(zip(points, points[1:] + points[:1]))
        first = max(0, math.floor(min(y for x, y in points)))
        last = min(n, math.ceil(max(y for x, y in points)))
        for y in range(first, last):
            scan = y + .5
            crossings = sorted(x0 + (scan - y0) * (x1 - x0) / (y1 - y0)
                               for (x0, y0), (x1, y1) in edges
                               if (y0 <= scan < y1) or (y1 <= scan < y0))
            for x0, x1 in zip(crossings[::2], crossings[1::2]):
                start = max(0, math.ceil(x0 - .5))
                end = min(n, math.ceil(x1 - .5))
                pixels[y * n + start:y * n + end] = b"\xff" * (end - start)
    result = bytearray(size * size)
    for y in range(size):
        for x in range(size):
            total = sum(pixels[(y * SS + dy) * n + x * SS + dx]
                        for dy in range(SS) for dx in range(SS))
            result[y * size + x] = round(total / (SS * SS))
    return result


def write_bmp(path, size, pixels):
    """Write a bottom-up, uncompressed, 256-shade BMP with padded rows."""
    stride = (size + 3) & ~3
    palette = b"".join(struct.pack("<BBBB", i, i, i, 0) for i in range(256))
    offset = 14 + 40 + len(palette)
    data_size = stride * size
    header = struct.pack("<2sIHHI", b"BM", offset + data_size, 0, 0, offset)
    info = struct.pack("<IiiHHIIiiII", 40, size, size, 1, 8, 0, data_size,
                       3779, 3779, 256, 256)
    with open(path, "wb") as output:
        output.write(header + info + palette)
        for y in reversed(range(size)):
            output.write(pixels[y * size:(y + 1) * size])
            output.write(b"\0" * (stride - size))


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    try:
        size = int(sys.argv[2]) if len(sys.argv) == 3 else SIZE
        if size <= 0:
            raise ValueError
    except ValueError:
        sys.exit("banner: SIZE must be a positive integer")
    os.makedirs(sys.argv[1], exist_ok=True)
    path = os.path.join(sys.argv[1], "BootLogo.bmp")
    write_bmp(path, size, render(size))
    print("banner: %s (%dx%d)" % (path, size, size))


if __name__ == "__main__":
    main()
