# Embedded Operating Systems and Architecture course
# Licensed under the MIT License. See LICENSE in the repository root.
#
# Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>

# ARM bare-metal cross-compiler: runs on the host, emits Cortex-M code.
CC := arm-none-eabi-gcc

# The time slice of the round-robin scheduler, in ticks (one tick =
# 10 ms). Change it from the command line: `make QUANTUM=2 run`.
QUANTUM ?= 5

# All generated files go under build/; `make clean` just deletes it.
BUILD_DIR := build
# Final linked image. .axf is ARM's name for an ELF carrying debug info.
BIN := $(BUILD_DIR)/scratch-os.axf
IMAGE := $(BUILD_DIR)/fs.img
IMAGE_ADDR := 0x00200000
IMAGE_TOOL ?= tools/mkimage.py
PROGRAM_DIR ?= programs

# The folders that hold our source files, one folder per OS layer.
SRC_DIRS := boot kernel drivers lib src
# Find every .c file (C code) and every .s file (assembly) in those
# folders. `wildcard` is make's way of listing files that match a pattern,
# so a new file is built automatically, with no change to this Makefile.
SOURCES := $(foreach d,$(SRC_DIRS),$(wildcard $(d)/*.c) $(wildcard $(d)/*.s))
# Map each foo.c or foo.s to its build/foo.o object file. The object
# keeps the folder name, e.g. kernel/irq.s becomes build/kernel/irq.o.
# Two nested patsubst calls: the inner one rewrites ".s" sources, the
# outer one rewrites ".c" sources (a word that does not match is left
# alone, so each source is changed by the one that applies to it).
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(patsubst %.s,$(BUILD_DIR)/%.o,$(SOURCES)))

# -mthumb / -mcpu: target the Cortex-M3's Thumb instruction set.
# -nostartfiles: no default C startup code - boot/startup.s is ours.
# -ffreestanding: no hosted environment - no full standard library, no
#  special meaning for main. Together with -nostdlib below, this makes
#  "no C library" true of the image.
# -std=gnu17: C17 plus GNU extensions (used by the inline-asm blocks).
CFLAGS := -mthumb -mcpu=cortex-m3 -nostartfiles -ffreestanding -std=gnu17
# -Iinclude: look for #include "..." headers in the include/ folder,
# so every file can write #include "cmsdk.h" whatever its own folder.
CFLAGS += -Iinclude
# -I.: also look in the project folder itself (the one holding this
# Makefile). Every other header is then written with its folder, e.g.
# #include "drivers/uart.h", the same from whatever file includes it.
CFLAGS += -I.
# -fno-builtin: GCC normally treats names like printf as the C library
# function it knows. There is no C library here (-nostdlib): our own
# printf in lib/printf.c is a plain function like any other.
CFLAGS += -fno-builtin
# Enable the standard and extra warning sets.
CFLAGS += -Wall -Wextra
# -O0 + full debug info: keep the generated code close to the source so
# it can be stepped through instruction by instruction in class.
CFLAGS += -O0 -g3
# Not -Werror: this code is kept across course years, and a new compiler
# warning should not stop the build.

# Pass the quantum to the C code: -DQUANTUM=5 defines QUANTUM as 5,
# as if kernel/task.h had `#define QUANTUM 5`.
CFLAGS += -DQUANTUM=$(QUANTUM)

# Use our linker script instead of the toolchain's default.
LDFLAGS := -T scripts/mps2_m3.ld
# Also write a map file: it shows which symbol landed at which address.
LDFLAGS += -Xlinker -Map=$(BUILD_DIR)/output.map
# -nostdlib: do not link a C library or its startup code.
# -lgcc: still link libgcc, the small compiler helper library (for
# example software integer division, which the compiler may use alone).
LDFLAGS += -nostdlib -lgcc

# The command line that starts QEMU: the MPS2-AN385 board, no monitor
# console, no window, UART0 connected to this terminal, our image as
# the program. There is no semihosting: all output goes through the UART.
QEMU := qemu-system-arm -machine mps2-an385 -monitor null -nographic -serial stdio -kernel $(BIN)
QEMU += -device loader,file=$(IMAGE),addr=$(IMAGE_ADDR),force-raw=on

# These targets are command names, not files on disk.
.PHONY: all clean image run debug run-trace test

# Default goal: build the image.
all: $(BIN)

# Compile one .c file to a .o file, creating its build folder first.
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble one .s file (hand-written assembly) to a .o file. We still
# call $(CC): GCC hands a ".s" file straight to the assembler, and the
# same CPU flags make the assembly match what the C files use.
$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Link every object file into the final image. The linker script is a
# prerequisite too, so editing it makes make link again.
$(BIN): $(OBJECTS) scripts/mps2_m3.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o $(BIN)

# Temporary valid raw image for QEMU's generic loader. The project packer
# will replace this recipe once the image format contract is implemented.
$(IMAGE):
	@mkdir -p $(dir $@)
	@printf 'SCRATCH-OS-PROJECT-IMAGE\n' > $@

# The host tool is student work. Once it exists, this target rebuilds the
# image from every program in programs/. The separate $(IMAGE) rule above
# remains the bootable placeholder used by an untouched project base.
image: $(BIN)
	@test -f $(IMAGE_TOOL) || { printf '%s\n' 'make image: implement tools/mkimage.py first'; exit 2; }
	@test -d $(PROGRAM_DIR) || { printf '%s\n' 'make image: programs/ is missing'; exit 2; }
	python3 $(IMAGE_TOOL) --output $(IMAGE) $(PROGRAM_DIR)

# Build, load the image at its fixed FLASH address, then run in QEMU.
run: $(BIN) $(IMAGE)
	$(QEMU)

# Same as `run`, but paused for a debugger:
#   -S             - freeze the emulated CPU at reset
#   -gdb tcp::1234 - open a GDB server on TCP port 1234 and wait there
# From a second terminal, connect with:
#   arm-none-eabi-gdb build/scratch-os.axf   (on Linux: gdb-multiarch)
# (for a demo build, e.g. DEMO=rogue: build/demo-rogue/scratch-os.axf;
#  for QUANTUM=2: build/quantum-2/scratch-os.axf)
# and inside gdb:
#   target remote :1234
debug: $(BIN) $(IMAGE)
	$(QEMU) -S -gdb tcp::1234

# Same as `run`, plus `-d unimp`: QEMU prints a message whenever the
# program touches a register or device that QEMU does not implement.
run-trace: $(BIN) $(IMAGE)
	$(QEMU) -d unimp

# Task 5.2 will publish the common test kit. Do not report a false pass
# while that suite is intentionally absent.
test:
	@printf '%s\n' 'make test: unavailable until the common project test kit is implemented (OpenSpec task 5.2).'
	@exit 2

# Remove every generated file, demo and quantum builds included.
clean:
	rm -rf build
