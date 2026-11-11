#!/usr/bin/env python3
"""
mkimage.py - pack ELF programs into an IMG1 image (big version).

Usage (assignment section 6.3, exact form):

    python3 tools/mkimage.py image build/fs.img NAME=PROGRAM.elf [NAME=PROGRAM.elf ...]

NAME is the name the shell lists and `run` accepts. Each file is copied byte
for byte, in the order given on the command line, and starts at a multiple of 4.
The tool does not check that the files are valid ELF: the kernel does that (B1),
and the test kit packs deliberately broken files on purpose.

On any error nothing is written, the message is printed as
"mkimage.py: error: <reason>" and the exit status is 2.
"""

import argparse
import os
import re
import struct
import sys

# ---------------------------------------------------------------------------
# IMG1 format constants (assignment section 6.1)
# ---------------------------------------------------------------------------
IMG_MAGIC = b"IMG1"            # bytes 49 4d 47 31
IMG_VERSION = 1
HEADER_SIZE = 20               # header size
ENTRY_SIZE = 24                # one directory entry
NAME_FIELD_SIZE = 16           # up to 15 characters + NUL
MAX_PROGRAMS = 32
MAX_IMAGE_SIZE = 0x00200000    # 2 MiB: 0x00200000..0x003fffff
MIN_FILE_SIZE = 40             # every program file is at least 40 bytes
ALIGNMENT = 4                  # every file starts at a multiple of 4

# Valid name: 1 to 15 characters from A-Z a-z 0-9 _ -
NAME_RE = re.compile(r"[A-Za-z0-9_-]{1,15}")


def align_up(value, alignment=ALIGNMENT):
    """Round 'value' up to a multiple of 'alignment'."""
    return (value + alignment - 1) // alignment * alignment


# ---------------------------------------------------------------------------
# Step 1: read NAME=PATH arguments and the files they point to
# ---------------------------------------------------------------------------
def read_programs(parser, specs):
    """
    Return [(name, file_bytes), ...] in command-line order. Every problem is
    reported with parser.error() before anything is written.
    """
    entries = []
    seen = set()
    for spec in specs:
        name, separator, path = spec.partition("=")
        if not separator or not name or not path:
            parser.error("'%s': expected NAME=PROGRAM.elf" % spec)
        if not NAME_RE.fullmatch(name):
            parser.error(
                "invalid program name '%s': use 1 to 15 characters from "
                "A-Z a-z 0-9 _ -" % name
            )
        if name in seen:
            parser.error("duplicate program name '%s'" % name)
        seen.add(name)

        if not os.path.isfile(path):
            parser.error("cannot read '%s': no such file" % path)
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as exc:
            parser.error("cannot read '%s': %s" % (path, exc.strerror))

        # Not checked here on purpose: the kernel validates the ELF (B1).
        if len(data) < MIN_FILE_SIZE:
            parser.error(
                "'%s' is too small (%d bytes, at least %d required)"
                % (path, len(data), MIN_FILE_SIZE)
            )
        entries.append((name, data))

    if len(entries) > MAX_PROGRAMS:
        parser.error("too many programs: %d (maximum is %d)"
                     % (len(entries), MAX_PROGRAMS))
    return entries


# ---------------------------------------------------------------------------
# Steps 2 and 3: layout and image construction
# ---------------------------------------------------------------------------
def build_image(entries):
    """Return the whole image as a bytearray. ValueError if it is too large."""
    count = len(entries)
    directory_offset = HEADER_SIZE                       # always 20
    directory_end = directory_offset + count * ENTRY_SIZE
    contents_start = align_up(directory_end)

    # Each file goes right after the previous one, aligned to 4.
    offsets = []
    cursor = contents_start
    for _name, data in entries:
        offsets.append(cursor)
        cursor = align_up(cursor + len(data))

    # image_size ends with the last file, without trailing padding.
    image_size = offsets[-1] + len(entries[-1][1])
    if image_size > MAX_IMAGE_SIZE:
        raise ValueError(
            "image too large: %d bytes (maximum is %d)"
            % (image_size, MAX_IMAGE_SIZE)
        )

    # All zeros: padding and the bytes after each name's NUL are already 0.
    image = bytearray(image_size)

    # Header (20 bytes)
    struct.pack_into(
        "<4sIIII", image, 0,
        IMG_MAGIC, IMG_VERSION, image_size, count, directory_offset,
    )

    # Directory (24 bytes per entry); "16s" pads the name with NUL bytes.
    for index, ((name, data), offset) in enumerate(zip(entries, offsets)):
        struct.pack_into(
            "<16sII", image, directory_offset + index * ENTRY_SIZE,
            name.encode("ascii"), offset, len(data),
        )

    # Program files, copied byte for byte.
    for (_name, data), offset in zip(entries, offsets):
        image[offset:offset + len(data)] = data

    return image


# ---------------------------------------------------------------------------
# Step 4: self-check with the same rules the kernel will apply
# ---------------------------------------------------------------------------
def verify_image(image):
    """Re-read the image as the kernel does. ValueError if a rule is broken."""
    total = len(image)
    if total < HEADER_SIZE:
        raise ValueError("image shorter than the header")

    magic, version, image_size, count, dir_off = struct.unpack_from(
        "<4sIIII", image, 0)

    if magic != IMG_MAGIC:
        raise ValueError("bad magic")
    if version != IMG_VERSION:
        raise ValueError("bad version")
    if image_size != total or not (HEADER_SIZE <= image_size <= MAX_IMAGE_SIZE):
        raise ValueError("bad image_size")
    if not (1 <= count <= MAX_PROGRAMS):
        raise ValueError("bad count")
    if dir_off % 4 != 0 or dir_off < HEADER_SIZE:
        raise ValueError("bad directory_offset")

    dir_end = dir_off + count * ENTRY_SIZE
    if dir_end > image_size:
        raise ValueError("directory outside the image")
    contents_start = align_up(dir_end)

    names = set()
    ranges = []
    for i in range(count):
        base = dir_off + i * ENTRY_SIZE
        raw_name = bytes(image[base:base + NAME_FIELD_SIZE])
        offset, size = struct.unpack_from("<II", image, base + NAME_FIELD_SIZE)

        nul = raw_name.find(b"\0")
        if nul < 0:
            raise ValueError("entry %d: name without NUL" % i)
        try:
            name = raw_name[:nul].decode("ascii")
        except UnicodeDecodeError:
            raise ValueError("entry %d: bad name" % i)
        if not NAME_RE.fullmatch(name):
            raise ValueError("entry %d: bad name" % i)
        if name in names:
            raise ValueError("entry %d: duplicate name" % i)
        names.add(name)

        if offset % 4 != 0:
            raise ValueError("entry %d: offset not aligned" % i)
        if size < MIN_FILE_SIZE:
            raise ValueError("entry %d: size too small" % i)
        if offset < contents_start:
            raise ValueError("entry %d: file overlaps the directory" % i)
        if offset + size > image_size:
            raise ValueError("entry %d: file outside the image" % i)
        ranges.append((offset, offset + size))

    ranges.sort()
    for (_s1, e1), (s2, _e2) in zip(ranges, ranges[1:]):
        if s2 < e1:
            raise ValueError("two files overlap")


# ---------------------------------------------------------------------------
# Step 5: write the output (temporary file first, then rename)
# ---------------------------------------------------------------------------
def write_output(parser, output, image):
    try:
        out_dir = os.path.dirname(output)
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)      # creates build/ if missing
        tmp_path = output + ".tmp"
        with open(tmp_path, "wb") as f:
            f.write(image)
        os.replace(tmp_path, output)
    except OSError as exc:
        parser.error("cannot write '%s': %s" % (output, exc.strerror))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="mkimage.py",
        description="Pack ELF programs into an IMG1 image.",
    )
    parser.add_argument("command", choices=["image"])
    parser.add_argument("output", metavar="OUTPUT",
                        help="image file to write, e.g. build/fs.img")
    parser.add_argument("programs", metavar="NAME=PROGRAM.elf", nargs="+",
                        help="program name and its ELF file")
    args = parser.parse_args(argv)

    entries = read_programs(parser, args.programs)

    try:
        image = build_image(entries)
    except ValueError as exc:
        parser.error(str(exc))

    try:
        verify_image(image)
    except ValueError as exc:
        # Should never happen: if it does, this script has a bug.
        sys.exit("mkimage.py: internal error: %s" % exc)

    write_output(parser, args.output, image)
    print("IMG1: %s (%d bytes, %d programs)"
          % (args.output, len(image), len(entries)))


if __name__ == "__main__":
    main()
