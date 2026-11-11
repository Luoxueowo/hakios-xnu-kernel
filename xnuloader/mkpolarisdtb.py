#!/usr/bin/env python3
"""Turn the checked-in Polaris Android DTB into a C byte array for the loader."""

import pathlib
import sys


def main() -> None:
	if len(sys.argv) != 2:
		raise SystemExit(f"usage: {sys.argv[0]} android-polaris.dtb")

	blob = pathlib.Path(sys.argv[1]).read_bytes()
	if len(blob) < 40 or blob[:4] != bytes.fromhex("d00dfeed"):
		raise SystemExit("not a flattened device tree")
	if int.from_bytes(blob[4:8], "big") > len(blob):
		raise SystemExit("truncated flattened device tree")

	print("/* Generated from android-polaris.dtb by mkpolarisdtb.py. */")
	print("static const uint8_t polaris_android_dtb[] = {")
	for start in range(0, len(blob), 12):
		line = ", ".join(f"0x{value:02x}" for value in blob[start : start + 12])
		print(f"\t{line},")
	print("};")


if __name__ == "__main__":
	main()
