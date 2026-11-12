# Scratch-OS Project Test Kit

This is the published test kit for the [scratch-OS run-time program loader
assignment](../assignment.md). It contains the exact public suites used for
assessment. The runner uses only the Python standard library and starts a fresh
QEMU process for every test.

## Contents

- `run_tests.py`: QEMU runner and exact UART transcript comparator;
- `suites/`: the 10-test small manifest and 14-test big manifest;
- `small/programs/`: common test-program sources used by both versions;
- `big/programs/`: the two additional W^X test-program sources;
- `small/generate_fixtures.py`: deterministic SFT1 malformed-file and IMG1
  generator;
- `big/generate_fixtures.py`: deterministic malformed-ELF and IMG1 generator;
- `small/expected/` and `big/expected/`: exact expected UART transcripts.

No prebuilt program, image, or kernel is included. Build the sources with your
project's program startup, address-zero linker script, system-call stubs,
`printf` implementation, and ARM bare-metal compiler settings required by the
assignment. Keep the listed IMG1 names unchanged because the suites send those
names to the shell.

## Build The Small-Suite Image

Compile every source in `small/programs/`. Convert the resulting ELF files with
your `tools/mkimage.py flat` command. The example below assumes the converted
files are under `build/test-kit/flats/`:

```sh
python3 small/generate_fixtures.py \
  --output-dir build/test-kit/small/files \
  --image build/test-kit/small/fs.img \
  print_plain=build/test-kit/flats/print_plain.sft \
  global_state=build/test-kit/flats/global_state.sft \
  thumb_ptr=build/test-kit/flats/thumb_ptr.sft \
  nonzero_slot=build/test-kit/flats/nonzero_slot.sft \
  normal_return=build/test-kit/flats/normal_return.sft \
  explicit_exit=build/test-kit/flats/explicit_exit.sft \
  kernel_store=build/test-kit/flats/kernel_store.sft
```

The generator appends `bad_magic`, `bad_header`, `too_large`, and `bad_reloc`
and writes one combined IMG1 image. Its arguments are `NAME=FILE`, so adapt only
the file paths if your build uses another output layout.

## Build The Big-Suite Image

Compile every source in `small/programs/` and `big/programs/` as unchanged ELF
files satisfying the assignment ABI. The first argument must be `print_plain`;
the generator uses that valid ELF reproducibly as the basis for malformed ELF
fixtures.

```sh
python3 big/generate_fixtures.py \
  --output-dir build/test-kit/big/files \
  --image build/test-kit/big/fs.img \
  print_plain=build/test-kit/programs/print_plain.elf \
  global_state=build/test-kit/programs/global_state.elf \
  thumb_ptr=build/test-kit/programs/thumb_ptr.elf \
  nonzero_slot=build/test-kit/programs/nonzero_slot.elf \
  normal_return=build/test-kit/programs/normal_return.elf \
  explicit_exit=build/test-kit/programs/explicit_exit.elf \
  kernel_store=build/test-kit/programs/kernel_store.elf \
  wx_write_code=build/test-kit/programs/wx_write_code.elf \
  wx_exec_data=build/test-kit/programs/wx_exec_data.elf
```

The generator preserves the supplied valid ELFs byte for byte, adds the four
common malformed cases plus `bad_rel_type` and `truncated_elf`, and packs them
into IMG1.

## List And Run Tests

Run these commands from this directory. Listing validates the selected suite
configuration and does not require QEMU or build artifacts:

```sh
python3 run_tests.py --suite small --list
python3 run_tests.py --suite big --list
```

Run a complete suite with your kernel and the matching generated image:

```sh
python3 run_tests.py --suite small \
  --kernel /path/to/project/build/scratch-os.axf \
  --image /path/to/project/build/test-kit/small/fs.img

python3 run_tests.py --suite big \
  --kernel /path/to/project/build/scratch-os.axf \
  --image /path/to/project/build/test-kit/big/fs.img
```

Repeat `--test NAME` to select tests. The startup, per-test, and whole-run
limits default to 5, 5, and 90 seconds; change them with `--startup-timeout`,
`--test-timeout`, and `--global-timeout`. Use `--qemu COMMAND` if
`qemu-system-arm` is not on `PATH` under that name.

Configuration errors return 2, a test or QEMU failure returns 1, and a fully
passing suite returns 0. Each result is `PASS <name>` or
`FAIL <name>: <reason>`, followed by a deterministic summary. Comparison is
exact after converting host CRLF to LF; spaces, prompts, addresses, ids, and
all other bytes remain significant. The only exception is an expected
transcript that declares `address_ranges`: there a placeholder such as
`<CODE_ADDRESS>` accepts any 8-digit hex address in the stated range (used by
`wx_write_code`, whose fault address depends on where your link order puts
`main`).
