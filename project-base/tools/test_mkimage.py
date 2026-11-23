"""Unit tests for tools/mkimage.py.

Run them from project-base/ with:

    python3 -m unittest discover -s tools -v

Each test works in its own temporary folder, so it never touches build/.
"""

import contextlib
import io
import os
from pathlib import Path
import struct
import tempfile
import unittest

import mkimage

HEADER = "<4sIIII"  # magic, version, image_size, count, directory_offset
ENTRY = "<16sII"  # name, offset, size


class MkimageTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = self._tmp.name
        self.out = os.path.join(self.dir, "fs.img")

    def tearDown(self):
        self._tmp.cleanup()

    # -- helpers ------------------------------------------------------------

    def make_file(self, name, size, fill=b"\x7f"):
        """Write a file of exactly `size` bytes and return its path."""
        path = os.path.join(self.dir, name)
        with open(path, "wb") as f:
            f.write((fill * size)[:size])
        return path

    def run_tool(self, *args):
        """Run mkimage.main(); return (exit status, stdout, stderr)."""
        out, err = io.StringIO(), io.StringIO()
        status = 0
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                mkimage.main(list(args))
            except SystemExit as exc:
                status = exc.code
        return status, out.getvalue(), err.getvalue()

    def assert_error(self, *args, message):
        """The tool must fail with status 2 and write nothing."""
        status, _, err = self.run_tool(*args)
        self.assertEqual(status, 2, err)
        self.assertIn("mkimage.py: error:", err)
        self.assertIn(message, err)
        self.assertFalse(os.path.exists(self.out), "no output on error")

    # -- valid images -------------------------------------------------------

    def test_valid_image_layout(self):
        a = self.make_file("a.elf", 41, b"A")
        b = self.make_file("b.elf", 40, b"B")
        status, out, err = self.run_tool("image", self.out, "first=" + a, "second=" + b)
        self.assertEqual(status, 0, err)
        self.assertIn("2 programs", out)

        image = Path(self.out).read_bytes()
        magic, version, size, count, dir_off = struct.unpack_from(HEADER, image, 0)
        self.assertEqual((magic, version, count, dir_off), (b"IMG1", 1, 2, 20))
        self.assertEqual(size, len(image))

        name0, off0, size0 = struct.unpack_from(ENTRY, image, 20)
        name1, off1, size1 = struct.unpack_from(ENTRY, image, 44)
        # Command-line order, names NUL-padded, offsets aligned to 4.
        self.assertEqual(name0, b"first".ljust(16, b"\0"))
        self.assertEqual(name1, b"second".ljust(16, b"\0"))
        self.assertEqual((off0, size0), (68, 41))  # 20 + 2 * 24 = 68
        self.assertEqual((off1, size1), (112, 40))  # 68 + 41 = 109 -> 112
        # Files copied byte for byte; image_size ends with the last file.
        self.assertEqual(image[off0 : off0 + size0], b"A" * 41)
        self.assertEqual(image[off1 : off1 + size1], b"B" * 40)
        self.assertEqual(size, off1 + size1)
        # Padding is zero.
        self.assertEqual(image[off0 + size0 : off1], b"\0\0\0")

    def test_files_are_not_checked_as_elf(self):
        # The kernel validates ELF files, not the packer.
        junk = self.make_file("junk.bin", 64, b"X")
        status, _, err = self.run_tool("image", self.out, "junk=" + junk)
        self.assertEqual(status, 0, err)

    def test_longest_valid_name(self):
        f = self.make_file("p.elf", 40)
        status, _, err = self.run_tool("image", self.out, "a" * 15 + "=" + f)
        self.assertEqual(status, 0, err)

    def test_thirty_two_programs(self):
        f = self.make_file("p.elf", 40)
        specs = ["p%d=%s" % (i, f) for i in range(32)]
        status, _, err = self.run_tool("image", self.out, *specs)
        self.assertEqual(status, 0, err)

    # -- errors ---------------------------------------------------------------

    def test_name_too_long(self):
        f = self.make_file("p.elf", 40)
        self.assert_error(
            "image", self.out, "a" * 16 + "=" + f, message="invalid program name"
        )

    def test_name_with_bad_character(self):
        f = self.make_file("p.elf", 40)
        self.assert_error(
            "image", self.out, "bad.name=" + f, message="invalid program name"
        )

    def test_missing_equal_sign(self):
        f = self.make_file("p.elf", 40)
        self.assert_error("image", self.out, f, message="expected NAME=PROGRAM.elf")

    def test_empty_name(self):
        f = self.make_file("p.elf", 40)
        self.assert_error(
            "image", self.out, "=" + f, message="expected NAME=PROGRAM.elf"
        )

    def test_duplicate_name(self):
        f = self.make_file("p.elf", 40)
        self.assert_error(
            "image", self.out, "x=" + f, "x=" + f, message="duplicate program name"
        )

    def test_missing_file(self):
        missing = os.path.join(self.dir, "missing.elf")
        self.assert_error("image", self.out, "x=" + missing, message="cannot read")

    def test_file_too_small(self):
        f = self.make_file("small.elf", 39)
        self.assert_error("image", self.out, "x=" + f, message="too small")

    def test_too_many_programs(self):
        f = self.make_file("p.elf", 40)
        specs = ["p%d=%s" % (i, f) for i in range(33)]
        self.assert_error("image", self.out, *specs, message="too many programs")

    def test_image_too_large(self):
        big = self.make_file("big.elf", 0x00200000)  # 2 MiB, plus header and directory
        self.assert_error("image", self.out, "big=" + big, message="image too large")

    def test_no_programs(self):
        status, _, _ = self.run_tool("image", self.out)
        self.assertEqual(status, 2)
        self.assertFalse(os.path.exists(self.out))

    def test_unknown_command(self):
        f = self.make_file("p.elf", 40)
        status, _, _ = self.run_tool("pack", self.out, "x=" + f)
        self.assertEqual(status, 2)

    def test_error_keeps_existing_output(self):
        with open(self.out, "wb") as f:
            f.write(b"old image")
        small = self.make_file("small.elf", 10)
        status, _, _ = self.run_tool("image", self.out, "x=" + small)
        self.assertEqual(status, 2)
        self.assertEqual(Path(self.out).read_bytes(), b"old image")


if __name__ == "__main__":
    unittest.main()
