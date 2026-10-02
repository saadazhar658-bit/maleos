# ---------------------------------------------------------------------------
# Maleos build system
#
#   make          Build build/kernel.elf
#   make iso      Package a bootable ISO (build/maleos.iso)
#   make run      Boot the ISO in QEMU
#   make debug    Boot paused with a GDB server on localhost:1234
#   make test     Headless boot test (used by CI)
#   make format   Format C sources with clang-format
#   make clean    Remove build artifacts
#   make help     Show this message
#
# Toolchain: uses x86_64-elf-* if installed (see scripts/build-toolchain.sh),
# otherwise falls back to the host GCC/binutils in freestanding mode.
# Override any tool on the command line, e.g.  make CC=clang
# ---------------------------------------------------------------------------

ARCH       := x86_64
BUILD_DIR  := build
ISO_DIR    := $(BUILD_DIR)/isodir
KERNEL     := $(BUILD_DIR)/kernel.elf
ISO        := $(BUILD_DIR)/maleos.iso

# Prefer the cross toolchain, fall back to host tools.
ifneq ($(shell command -v x86_64-elf-gcc 2>/dev/null),)
CROSS ?= x86_64-elf-
else
CROSS ?=
endif

# make predefines CC=cc, so only override it when the user did not set it.
ifeq ($(origin CC),default)
CC      := $(CROSS)gcc
endif
LD      := $(CROSS)ld
NASM    ?= nasm
QEMU    ?= qemu-system-x86_64
GRUB_MKRESCUE ?= grub-mkrescue

CFLAGS  := -std=c11 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
           -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
           -fno-tree-loop-distribute-patterns \
           -Wall -Wextra -Werror -O2 -g -Isrc/include
ASFLAGS := -f elf64 -g -F dwarf
LDFLAGS := -n -nostdlib -z noexecstack -z max-page-size=0x1000 -T linker.ld

C_SRCS   := $(shell find src -name '*.c' 2>/dev/null)
ASM_SRCS := $(shell find src -name '*.asm' 2>/dev/null)
OBJS     := $(patsubst src/%.c,$(BUILD_DIR)/%.c.o,$(C_SRCS)) \
            $(patsubst src/%.asm,$(BUILD_DIR)/%.asm.o,$(ASM_SRCS))

QEMU_FLAGS := -cdrom $(ISO) -m 256M -serial stdio -no-reboot

.PHONY: all iso run debug test format clean help
all: $(KERNEL)

$(BUILD_DIR)/%.c.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.asm.o: src/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

iso: $(ISO)

$(ISO): $(KERNEL) iso/boot/grub/grub.cfg
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	cp iso/boot/grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR) 2>/dev/null

run: $(ISO)
	$(QEMU) $(QEMU_FLAGS)

debug: $(ISO)
	@echo "QEMU paused. Attach with:"
	@echo '  gdb -ex "target remote localhost:1234" -ex "symbol-file $(KERNEL)"'
	$(QEMU) $(QEMU_FLAGS) -s -S

test: $(ISO)
	./scripts/boot-test.sh $(ISO)

format:
	@command -v clang-format >/dev/null || { echo "clang-format not installed"; exit 1; }
	@find src -name '*.c' -o -name '*.h' | xargs -r clang-format -i

clean:
	rm -rf $(BUILD_DIR)

help:
	@sed -n '3,13p' Makefile | sed 's/^# \{0,1\}//'
