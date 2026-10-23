#
# Makefile.app -- links the emulator kernel image.
#
# Circle's Rules.mk supplies the compiler, the flags, the architecture checks and
# the library paths; this file lists what goes into the image and links it.  It
# is invoked by scripts/build.ps1 with an output directory, for example:
#
#   make -f Makefile.app \
#        OUT=$PWD/build/kernel64 CIRCLEHOME=$PWD/external/circle \
#        CORE_LIB=$PWD/build/lib/libatari800.a \
#        KERNEL_IMAGE_NAME=kernel8.img \
#        AARCH=64 RASPPI=3 PREFIX=aarch64-none-elf-
#
# The Atari800 emulation core is not listed here: Makefile.core builds it into a
# static archive, which is linked in as one of the libraries.
#

ifndef OUT
$(error OUT must be set to the directory the image is written to)
endif

ifndef KERNEL_IMAGE_NAME
$(error KERNEL_IMAGE_NAME must be set, e.g. kernel8.img)
endif

# The emulation core archive, built by Makefile.core.
ifndef CORE_LIB
$(error CORE_LIB must be set to the path of build/lib/libatari800.a)
endif

# This makefile is run with OUT as the working directory, so OUT is "." and the
# paths in it are relative.  Everything that lives outside OUT is absolute, so
# the working directory only decides where the build products land.
OUT          := $(abspath $(OUT))
MAKEFILE_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

CIRCLEHOME ?= $(MAKEFILE_ROOT)/external/circle
CIRCLEHOME := $(abspath $(CIRCLEHOME))

# Circle's build rules.
#
# They must be included before TARGET is assigned: Rules.mk does `TARGET ?=` and
# defines its own recipe for $(TARGET).img.  Assigning TARGET after the include
# means Rules.mk's recipe is built for Circle's default name, and the recipe at
# the bottom of this file -- for a name of our choosing -- is a different target
# and so does not collide with it.
#
# The name matters.  If TARGET were, say, "kernel8", both recipes would match
# kernel8.img, and make would use whichever came first in the makefile: Circle's,
# which knows nothing about this project's object files and would link without
# compiling them.
include $(CIRCLEHOME)/Rules.mk

TARGET := atari-emulator

# ---------------------------------------------------------------------------
# This project's sources
#
# Objects go to obj/ under OUT, so a build for one architecture never reuses the
# objects of another; the two need different flags and produce incompatible
# objects.
# ---------------------------------------------------------------------------
OBJDIR := obj

APP_SRCS := \
	src/main.cpp \
	src/kernel.cpp \
	src/bootlog.cpp

MODULE_SRCS := \
	src/port/vfs.c \
	src/config/settings.cpp \
	src/video/video_out.cpp \
	src/storage/storage.cpp \
	src/input/usb_keyboard.cpp \
	src/input/usb_keymap.cpp \
	src/input/gpio_keyboard.cpp \
	src/input/joystick.cpp \
	src/input/usb_joystick.cpp \
	src/input/gpio_joystick.cpp \
	src/audio/audio_out.cpp \
	src/osd/osd_menu.cpp \
	src/osd/atari_font.cpp

OBJS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(patsubst %.c,$(OBJDIR)/%.o,$(APP_SRCS) $(MODULE_SRCS)))

# ---------------------------------------------------------------------------
# Diagnostic build: make PROBE=1
#
# Replaces this project's entry point and application class with a minimal
# program that prints a series of markers and nothing else.  Everything else is
# identical -- the same libraries in the same order, the same compiler flags, the
# same linker script, and the emulation core still linked in -- so it tests the
# *linkage* rather than the emulator, and the image has the same size and memory
# layout as the real one.
#
# This exists because the emulator starts and dies with a black screen while
# Circle's own sample runs on the same card.  The one structural difference
# between the two is that this project links newlib's libc and libm in front of
# Circle's own C library.  If the probe prints, the linkage is sound and the
# fault is in the emulator's start-up; if it does not, the fault is in the
# linkage.
#
#   make -f Makefile.app ... PROBE=1 KERNEL_IMAGE_NAME=kernel8-probe.img
#
# It must come after both source lists, because it replaces them.
# ---------------------------------------------------------------------------
ifdef PROBE
MODULE_SRCS :=
APP_SRCS    := src/main_probe.cpp
OBJS        := $(OBJDIR)/src/main_probe.o

# PROBE_SRC=circle builds Circle's own 03-screentext sample sources instead of
# this project's probe.
#
# This is the control for the experiment.  That sample is the program already
# known to boot on the card in question, so building it unchanged through this
# makefile separates the two remaining possibilities cleanly:
#
#   Circle's sample via this makefile   boots?  then the build is sound and the
#                                              fault is in our own probe source
#   Circle's own makefile (baseline)    boots   (established)
#
# Without this control, a black screen from our probe leaves it ambiguous
# whether the source or the build is at fault.
ifeq ($(strip $(PROBE_SRC)),circle)
CIRCLE_SAMPLE := $(CIRCLEHOME)/sample/03-screentext
APP_SRCS := $(CIRCLE_SAMPLE)/main.cpp $(CIRCLE_SAMPLE)/kernel.cpp
OBJS     := $(OBJDIR)/sample/main.o $(OBJDIR)/sample/kernel.o

$(OBJDIR)/sample/%.o: $(CIRCLE_SAMPLE)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CPP   $<"
	@$(CPP) $(CPPFLAGS) -I$(CIRCLE_SAMPLE) $(DEPFLAGS) -c -o $@ $<
endif

# PROBE_LINK=circle links *only* Circle's own library, exactly as Circle's
# sample programs do and as the diagnostic kernel that is known to boot does.
#
# This turns the probe into a controlled experiment.  PROBE=1 alone links the
# emulation core and newlib as well, so:
#
#   probe, core + newlib  ->  boots?   these two together are the problem
#   probe, Circle only    ->  boots?   the probe code itself is fine, and the
#                                      difference is purely the linkage
#
# Without the second variant a black screen would leave it ambiguous whether the
# probe program or the way it is linked is at fault.
ifeq ($(strip $(PROBE_LINK)),circle)
PROBE_CIRCLE_ONLY := 1
endif
endif

# ---------------------------------------------------------------------------
# Libraries
#
# Order matters in two separate ways here, and the second one is not obvious.
#
# FIRST, anything that calls into a library must come before it, so the emulation
# core is first: the application calls it, and it calls into nothing else here.
# CORE_LIB is supplied by the caller because Makefile.core builds it separately.
#
# SECOND -- and this one cost real time to find -- CIRCLE MUST COME BEFORE
# NEWLIB.
#
# Circle ships its own small C library inside libcircle.a: memset, memcpy,
# memmove, strlen, strcmp, strtoul and their relatives.  newlib's libc.a defines
# every one of those names as well.  With a single --start-group the linker
# resolves a name from whichever archive it reaches first, so this order decides
# which implementation the *entire program* gets -- including Circle's own code,
# which was compiled against its own.
#
# Getting it backwards is not a link error.  It builds cleanly, and the kernel
# then produces a black screen with no output at all, because Circle's sysinit()
# clears .bss with:
#
#     memset (&__bss_start, 0, &__bss_end - &__bss_start);
#
# and that call is resolved at link time like any other.  Point it at another
# implementation and .bss is not cleared the way Circle expects; every global
# Circle and this project depend on then holds whatever was in RAM at power-on,
# and the kernel dies before it can print anything.
#
# This was found by building one probe kernel two ways, differing only in which
# libraries were linked:
#
#     Circle only                   -> boots, prints its markers
#     Circle + newlib + the core    -> black screen, no LED, no output
#
# So Circle comes first and newlib after it.  newlib still supplies everything
# Circle does not have -- snprintf, vsnprintf, strtod, and the formatted
# conversion the emulation core leans on heavily -- because the linker only takes
# from an archive what is still undefined at that point.
#
# The library paths MUST be resolved with the architecture flags.  This toolchain
# is a multilib build, and without them the query returns the default soft-float
# libc; linking that against Circle's hard-float objects fails with "uses VFP
# register arguments, libc.a does not".  ARCHCPU is the flag set Circle chose for
# this target.
#
# libnosys supplies the system-call stubs (_write, _sbrk, _fstat, ...) that
# newlib assumes a target provides.  They are never reached -- this project's own
# VFS handles all file access and Circle owns the heap -- but newlib references
# them, so they have to exist.  It must come after libc, which is what needs
# them.
# ---------------------------------------------------------------------------
NEWLIB_LIBC   := $(shell $(PREFIX)gcc $(ARCHCPU) -print-file-name=libc.a)
NEWLIB_LIBM   := $(shell $(PREFIX)gcc $(ARCHCPU) -print-file-name=libm.a)
NEWLIB_NOSYS  := $(shell $(PREFIX)gcc $(ARCHCPU) -print-file-name=libnosys.a)

# ---------------------------------------------------------------------------
# KERNEL_MAX_SIZE is deliberately left at Circle's default
#
# Circle's sysconfig.h defines it as 2 MiB:
#
#     #ifndef KERNEL_MAX_SIZE
#     #define KERNEL_MAX_SIZE  (2 * MEGABYTE)
#     #endif
#
# It is tempting to raise it, because it looks like a build setting -- and doing
# so compiles and links perfectly well.  It is not a build setting.  From
# include/circle/memorymap.h it derives MEM_KERNEL_END, MEM_KERNEL_STACK,
# MEM_COHERENT_REGION and MEM_HEAP_START, so raising it moves the kernel stacks,
# the coherent DMA region and the start of the heap.
#
# The Raspberry Pi firmware reserves the kernel's memory using its own copy of
# this figure, and is never told when the build changes.  Raising it therefore
# makes Circle place its own memory in a region the firmware still owns.  That
# is exactly what happened here: with 4 MiB, the USB stack's assertion
#
#     assert ((uintptr) pBuffer > MEM_KERNEL_END);
#
# fired on the first control transfer, after the kernel had booted and printed
# its start-up messages.
#
# This project's kernel is about 1.14 MB including .bss, so the 2 MiB default has
# room to spare.  scripts/build.ps1 checks the finished image against it rather
# than the limit being raised.  If the kernel ever outgrows it, the answer is to
# make the kernel smaller, not to move the boundary.  See
# external/circle/Config2.mk.
# ---------------------------------------------------------------------------

# Circle before newlib -- see the note above.  This order is the fix for a black
# screen with no output: it is what makes Circle's own memset the one that
# sysinit() clears .bss with.
LIBS := \
	$(CORE_LIB) \
	$(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	$(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	$(CIRCLEHOME)/lib/sound/libsound.a \
	$(CIRCLEHOME)/lib/usb/libusb.a \
	$(CIRCLEHOME)/lib/input/libinput.a \
	$(CIRCLEHOME)/lib/fs/libfs.a \
	$(CIRCLEHOME)/lib/sched/libsched.a \
	$(CIRCLEHOME)/lib/libcircle.a \
	$(NEWLIB_LIBC) \
	$(NEWLIB_NOSYS) \
	$(NEWLIB_LIBM)

# The controlled half of the probe experiment: Circle's own library and nothing
# else, which is exactly what Circle's sample -- the kernel known to boot on this
# card -- links.
#
# If this variant boots and the full one does not, then the fault is in the
# linkage and not in any code, which is the conclusion the black screen and the
# dead LED most strongly suggest.  Without it, a black probe would be ambiguous.
ifeq ($(strip $(PROBE_CIRCLE_ONLY)),1)
LIBS := $(CIRCLEHOME)/lib/libcircle.a

# Circle's library has no snprintf, and this variant must not link newlib, so
# the one probe that uses it is compiled out.  See src/main_probe.cpp.
CPPFLAGS += -DPROBE_NO_NEWLIB
endif

# newlib and Circle both implement the C string and conversion routines that
# this link needs.  Circle's are deliberately minimal -- it is built with
# STDLIB_SUPPORT=1 and ships its own small libc -- and newlib's are the complete
# ones the emulation core was compiled against.  Both are correct; taking the
# first definition found lets the link succeed without either side being
# patched.
#
# This is a relaxation, so it is worth knowing exactly what it permits: a symbol
# defined in more than one object is resolved to the first, instead of the link
# failing.  That is safe here because the overlapping set is entirely the
# standard C string and conversion functions.
#
# It is deliberately NOT applied to the Circle-only probe variant, which has no
# overlap to resolve and so should link cleanly.
ifneq ($(strip $(PROBE_CIRCLE_ONLY)),1)
LDFLAGS += --allow-multiple-definition
endif

# ---------------------------------------------------------------------------
# Include paths
#
# Circle's Rules.mk has already put its own into INCLUDE.  This project's
# directories are all listed because the modules include each other's headers by
# bare name, which keeps the includes independent of the directory layout.
# ---------------------------------------------------------------------------
INCLUDE += \
	-I $(MAKEFILE_ROOT)/src \
	-I $(MAKEFILE_ROOT)/src/port \
	-I $(MAKEFILE_ROOT)/src/config \
	-I $(MAKEFILE_ROOT)/src/input \
	-I $(MAKEFILE_ROOT)/src/storage \
	-I $(MAKEFILE_ROOT)/src/video \
	-I $(MAKEFILE_ROOT)/src/audio \
	-I $(MAKEFILE_ROOT)/src/osd

EXTRAINCLUDE += -I $(CIRCLEHOME)/addon/fatfs

.PHONY: all
all: $(TARGET).img

# Rules.mk defines its own "all" and its own default goal, so the default has to
# be set explicitly after the include.  Without this, a bare `make` would build
# Circle's default kernel name using Circle's recipe -- which knows nothing about
# this project's objects and links without compiling them.
.DEFAULT_GOAL := all

# Keep the intermediate object files.
#
# They are produced by the pattern rules below and consumed by the link, and
# make's default behaviour for such files is to delete them again.  That is
# harmless on a successful build but actively unhelpful on a failed one: the
# objects vanish, and the next run links without them and reports a list of
# missing files that says nothing about the original error.  Marking them
# secondary keeps a failed build's work for inspection.
.SECONDARY:

# ---------------------------------------------------------------------------
# Compilation
#
# Rules.mk's generic "%.o: %.cpp" rule would put the objects next to the
# sources, mixing build output into the source tree and letting the two
# architectures overwrite each other.  These rules keep everything under obj/.
#
# Header dependencies are generated rather than guessed at.  Without them make
# only knows about the .cpp or .c it compiled, so changing a header leaves every
# object that includes it stale -- and a stale object links to the wrong symbols,
# producing an "undefined reference" for a function that is plainly right there
# in the source.  That is a confusing failure to be handed, so -MMD writes a .d
# file next to each object listing every header it used, and the -include below
# reads them back on the next run.  -MP adds a phony target for each header so
# that deleting one does not stop the build with "no rule to make target".
# ---------------------------------------------------------------------------
DEPFLAGS := -MMD -MP

$(OBJDIR)/src/%.o: $(MAKEFILE_ROOT)/src/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CPP   $<"
	@$(CPP) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJDIR)/src/%.o: $(MAKEFILE_ROOT)/src/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) $(C_STANDARD) $(DEPFLAGS) -c -o $@ $<

# The dependency files, read back on the next run.
#
# -include rather than include, so that a first build with no .d files yet is
# not an error.
DEPS := $(OBJS:.o=.d)
-include $(DEPS)

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
$(TARGET).img: $(OBJS) $(LIBS) $(CIRCLEHOME)/circle.ld
	@echo "  LD    $(TARGET).elf"
	@$(LD) -o $(TARGET).elf -Map $(TARGET).map $(LDFLAGS) \
		-T $(CIRCLEHOME)/circle.ld $(CRTBEGIN) $(OBJS) \
		--start-group $(LIBS) $(EXTRALIBS) --end-group $(CRTEND)
	@echo "  DUMP  $(TARGET).lst"
	@$(OBJDUMP) -d $(TARGET).elf | $(CPPFILT) > $(TARGET).lst
	@echo "  COPY  $(KERNEL_IMAGE_NAME)"
	@$(OBJCOPY) $(TARGET).elf -O binary $(KERNEL_IMAGE_NAME)
	@echo -n "  SIZE  $(KERNEL_IMAGE_NAME) => "
	@wc -c < $(KERNEL_IMAGE_NAME)
