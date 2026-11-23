#!/usr/bin/env python3
"""Generate deterministic malformed SFT1 files and a combined IMG1 image."""

import argparse
import struct
from pathlib import Path

IMAGE_MAGIC = 0x31474D49
SFT_MAGIC = 0x31544653
VERSION = 1
IMAGE_HEADER_SIZE = 20
DIRECTORY_ENTRY_SIZE = 24
SFT_HEADER_SIZE = 40


def flat_header(*, magic=SFT_MAGIC, version=VERSION, code_size=4,
                data_size=0, bss_size=0, entry=1, relocations=()):
    initialized = code_size + data_size
    reloc_offset = SFT_HEADER_SIZE + initialized
    file_size = reloc_offset + 4 * len(relocations)
    header = struct.pack(
        "<10I", magic, version, SFT_HEADER_SIZE, code_size, data_size,
        bss_size, entry, reloc_offset, len(relocations), file_size)
    payload = b"\x00" * initialized
    relocation_data = b"".join(struct.pack("<I", value)
                                 for value in relocations)
    return header + payload + relocation_data


def pack_image(programs):
    directory_offset = IMAGE_HEADER_SIZE
    content_offset = directory_offset + len(programs) * DIRECTORY_ENTRY_SIZE
    directory = bytearray()
    contents = bytearray()
    for name, data in programs:
        while (content_offset + len(contents)) & 3:
            contents.append(0)
        offset = content_offset + len(contents)
        encoded_name = name.encode("ascii") + b"\0"
        directory += encoded_name.ljust(16, b"\0")
        directory += struct.pack("<II", offset, len(data))
        contents += data
    image_size = content_offset + len(contents)
    return (struct.pack("<5I", IMAGE_MAGIC, VERSION, image_size,
                        len(programs), directory_offset)
            + directory + contents)


def parse_program(specification):
    if "=" not in specification:
        raise ValueError(f"expected NAME=FILE, got {specification!r}")
    name, filename = specification.split("=", 1)
    if not name or len(name) > 15 or not all(
            character.isascii()
            and (character.isalnum() or character in "_-")
            for character in name):
        raise ValueError(f"invalid IMG1 name {name!r}")
    return name, Path(filename).read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("program", nargs="+", metavar="NAME=FILE")
    args = parser.parse_args()

    programs = [parse_program(item) for item in args.program]
    malformed = [
        ("bad_magic", flat_header(magic=0x21444142)),
        ("bad_header", flat_header(version=2)),
        ("too_large", flat_header(bss_size=3072)),
        ("bad_reloc", flat_header(relocations=(4,))),
    ]
    all_programs = programs + malformed

    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, data in all_programs:
        (args.output_dir / f"{name}.sft").write_bytes(data)
    image = pack_image(all_programs)
    args.image.parent.mkdir(parents=True, exist_ok=True)
    args.image.write_bytes(image)
    print(f"IMG1: {args.image} ({len(image)} bytes, {len(all_programs)} programs)")


if __name__ == "__main__":
    main()
