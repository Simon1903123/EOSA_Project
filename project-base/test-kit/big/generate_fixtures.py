#!/usr/bin/env python3
"""Pack common ELF programs and ELF-form malformed fixtures into IMG1."""

import argparse
import struct
from pathlib import Path

IMAGE_MAGIC = 0x31474D49
VERSION = 1
HEADER_SIZE = 20
ENTRY_SIZE = 24


def parse_program(specification):
    if "=" not in specification:
        raise ValueError(f"expected NAME=FILE, got {specification!r}")
    name, filename = specification.split("=", 1)
    return name, Path(filename).read_bytes()


def headers(data):
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    phoff, phentsize, phnum = header[5], header[9], header[10]
    shoff, shentsize, shnum = header[6], header[11], header[12]
    return phoff, phentsize, phnum, shoff, shentsize, shnum


def allocated_rel_offset(data):
    _phoff, _phentsize, _phnum, shoff, shentsize, shnum = headers(data)
    sections = [struct.unpack_from("<10I", data, shoff + i * shentsize)
                for i in range(shnum)]
    for section in sections:
        if section[1] != 9 or section[5] == 0 or section[7] >= shnum:
            continue
        if sections[section[7]][2] & 2:
            return section[4]
    raise ValueError("source ELF has no allocated REL section")


def malformed_elves(source):
    result = []

    bad_magic = bytearray(source)
    bad_magic[0:4] = b"BAD!"
    result.append(("bad_magic", bytes(bad_magic)))

    bad_header = bytearray(source)
    bad_header[4] = 2
    result.append(("bad_header", bytes(bad_header)))

    too_large = bytearray(source)
    phoff, phentsize, phnum, _shoff, _shentsize, _shnum = headers(too_large)
    changed = False
    for index in range(phnum):
        offset = phoff + index * phentsize
        p_type, _p_offset, p_vaddr = struct.unpack_from("<III", too_large, offset)
        flags = struct.unpack_from("<I", too_large, offset + 24)[0]
        if p_type == 1 and flags & 2:
            struct.pack_into("<I", too_large, offset + 20, 3073 - p_vaddr)
            changed = True
            break
    if not changed:
        raise ValueError("source ELF has no writable PT_LOAD")
    result.append(("too_large", bytes(too_large)))

    bad_reloc = bytearray(source)
    _phoff, _phentsize, _phnum, shoff, shentsize, shnum = headers(bad_reloc)
    sections = [struct.unpack_from("<10I", bad_reloc, shoff + i * shentsize)
                for i in range(shnum)]
    rel_offset = allocated_rel_offset(bad_reloc)
    target_index = next(section[7] for section in sections
                        if section[1] == 9 and section[4] == rel_offset)
    target = sections[target_index]
    struct.pack_into("<I", bad_reloc, rel_offset, target[3] + target[5])
    result.append(("bad_reloc", bytes(bad_reloc)))
    return result


def big_only_malformed(source):
    unsupported = bytearray(source)
    rel_offset = allocated_rel_offset(unsupported)
    info = struct.unpack_from("<I", unsupported, rel_offset + 4)[0]
    struct.pack_into("<I", unsupported, rel_offset + 4, (info & ~0xff) | 3)

    phoff, phentsize, phnum, shoff, shentsize, shnum = headers(source)
    segment_ends = []
    for index in range(phnum):
        offset = phoff + index * phentsize
        p_type, p_offset = struct.unpack_from("<II", source, offset)
        p_filesz = struct.unpack_from("<I", source, offset + 16)[0]
        if p_type == 1 and p_filesz:
            segment_ends.append(p_offset + p_filesz)
    if not segment_ends:
        raise ValueError("source ELF has no non-empty PT_LOAD")
    truncated_size = max(segment_ends) - 1
    if truncated_size <= phoff + phentsize * phnum:
        raise ValueError("cannot truncate source after its program-header table")
    if shoff + shentsize * shnum <= truncated_size:
        raise ValueError("source ELF section table does not follow its loadable segments")

    return [
        ("bad_rel_type", bytes(unsupported)),
        ("truncated_elf", source[:truncated_size]),
    ]


def pack_image(programs):
    content_offset = HEADER_SIZE + len(programs) * ENTRY_SIZE
    directory = bytearray()
    contents = bytearray()
    for name, data in programs:
        while (content_offset + len(contents)) & 3:
            contents.append(0)
        offset = content_offset + len(contents)
        directory += (name.encode("ascii") + b"\0").ljust(16, b"\0")
        directory += struct.pack("<II", offset, len(data))
        contents += data
    size = content_offset + len(contents)
    return struct.pack("<5I", IMAGE_MAGIC, VERSION, size, len(programs), 20) + directory + contents


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("program", nargs="+", metavar="NAME=ELF")
    args = parser.parse_args()

    programs = [parse_program(item) for item in args.program]
    malformed = malformed_elves(programs[0][1]) + big_only_malformed(programs[0][1])
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, data in programs + malformed:
        (args.output_dir / f"{name}.elf").write_bytes(data)
    image = pack_image(programs + malformed)
    args.image.parent.mkdir(parents=True, exist_ok=True)
    args.image.write_bytes(image)
    print(f"IMG1: {args.image} ({len(image)} bytes, {len(programs) + len(malformed)} programs)")


if __name__ == "__main__":
    main()
