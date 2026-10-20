# Circle on Raspberry Pi Zero 2 W / Pi 3 / Pi 4 / Pi 5 — Implementation Guide for a Bare-Metal Atari 8-bit Emulator

**Analyzed tree:** `external/circle` — rsta2/circle, commit `7a81e1b480fe7d19fc1fc695b89bff0a0848f44b` (`CIRCLEVER = 510101`, i.e. release 51.1.1 — `Rules.mk:21`).

**Method:** read-only inspection of `README.md`, `boot/`, `configure`, `Rules.mk`, all of `doc/`, `include/circle/**`, `lib/**` (incl. `lib/sound`, `lib/usb`, `lib/fs`, `lib/sched`), `addon/fatfs`, `addon/SDCard`, and the relevant `sample/` directories. Every claim below cites a file and line. Anything not provable from this tree is called out explicitly in §10.8 "Unconfirmed / must be verified on hardware".

Path convention: paths are relative to the Circle root (`external/circle/`).

---

## 1. Supported models & the 32/64-bit split

### 1.1 Supported models (README.md:175–207)

`README.md:178` — *"This table lists the support status for the different Raspberry Pi models. Not listed models are not supported."*

| Model | Status | Remarks | README line |
|---|---|---|---|
| Pi Model A / B R1 | Should work | | 182–183 |
| Pi Model B R2, A+, B+ | Tested | | 184–186 |
| **Pi Zero / Zero W** | Tested | | 188–189 |
| **Pi Zero 2 W** | Tested | *"WLAN unknown for new revision"* | 190 |
| **Pi 2 Model B, Pi 3 B, 3 A+, 3 B+** | Tested | | 192–195 |
| **Pi 4 Model B, Pi 400** | Tested | | 196–197 |
| **Pi 5** | Tested | *"With BCM2712 C1 and D0 steppings"* | 198 |
| Pi 500 | **Unknown** | not confirmed | 199 |
| CM1/CM3/CM3+/CM4/CM5/CM0 | Reported to work / Should work | | 201–207 |

`CMachineInfo::GetMachineModel()` reports `MachineModelZero2W` explicitly (`include/circle/machineinfo.h:39`), and `TSoCType` has `SoCTypeBCM2837` for Zero 2 W / Pi 3 and `SoCTypeBCM2711` / `SoCTypeBCM2712` for Pi 4 / Pi 5 (`machineinfo.h:59–67`). `TSoCStepping` distinguishes Pi 5 C1 vs D0 (`machineinfo.h:69–74`).

### 1.2 The 32/64-bit split — the key matrix

`README.md:97` — feature table: *"C++ build environment | AArch32 and AArch64 support | **AArch64 only**"* (third column = Raspberry Pi 5).
`README.md:214` — *"**Circle does not support 32-bit applications on the Raspberry Pi 5.**"*

The build system enforces this — `Rules.mk:79–102` handles `AARCH=32` and ends with:

```
99  else
100     $(error RASPPI must be set to 1, 2, 3 or 4)
101 endif
```

so `AARCH=32 RASPPI=5` is a hard build error. `AARCH=64` requires `RASPPI` in {3,4,5} (`Rules.mk:103–121`, error text at `Rules.mk:120`).

**RASPPI → target file → models (README.md:225–234):**

| RASPPI | kernel file | Models | Optimized for | Rules.mk |
|---|---|---|---|---|
| 1 | `kernel.img` | A, B, A+, B+, Zero, (CM) | ARM1176JZF-S | `Rules.mk:83–86` |
| 2 | `kernel7.img` | 2, 3, **Zero 2**, (CM3) | Cortex-A7 | `Rules.mk:87–90` |
| 3 | `kernel8-32.img` | 3, **Zero 2**, (CM3) | Cortex-A53 | `Rules.mk:91–94` |
| 4 | `kernel7l.img` | 4B, 400, CM4 | Cortex-A72 | `Rules.mk:95–98` |
| 3 (`AARCH=64`) | `kernel8.img` | 3, Zero 2 (BCM2837) | Cortex-A53 | `Rules.mk:107–110` |
| 4 (`AARCH=64`) | `kernel8-rpi4.img` | 4B, 400, CM4 | Cortex-A72 | `Rules.mk:111–114` |
| 5 (`AARCH=64`) | `kernel_2712.img` | 5, 500, CM5 | Cortex-A76 | `Rules.mk:115–118` |

**Answers to the direct questions:**

* **Is AArch64 *required* for Pi 4 / Pi 5?** For **Pi 5: yes** — AArch32 is impossible (`README.md:214`, `Rules.mk:100`; `boot/config32.txt` only ever selects `kernel7l.img` for `[pi4]`, never anything for `[pi5]`). For **Pi 4: no** — 32-bit is fully supported (`RASPPI=4` → `kernel7l.img`, `Rules.mk:95–98`; `boot/config32.txt:15–18` selects `kernel=kernel7l.img` under `[pi4]` and also sets `armstub=armstub7-rpi4.bin`).
* **Is Pi Zero 2 W supported by the 32-bit kernel?** **Yes.** `README.md:230–231` lists *"2, 3, Zero 2, (CM3)"* for `RASPPI=2` (`kernel7.img`) and *"3, Zero 2, (CM3)"* for `RASPPI=3` (`kernel8-32.img`). Both are AArch32. It is also supported in 64-bit via `RASPPI=3`/`AARCH=64` → `kernel8.img` (`README.md:254`: *"There are also Raspberry Pi 2 versions and the Raspberry Pi Zero 2, which are based on the BCM2837 SoC. These Raspberry Pi versions can be used too (with `RASPPI = 3`)."*).
  *Practical caveat:* with `arm_64bit=0` the firmware's default 32-bit kernel filename for a BCM2837 board is `kernel7.img`. So a `RASPPI=3` AArch32 build produces `kernel8-32.img`, which the firmware will **not** load unless it is renamed to `kernel7.img` or `kernel=kernel8-32.img` is added to `config.txt`. `boot/config32.txt` contains no `kernel=` override for `[pi02]`/`[pi3]`.
* **One image for all four targets?** No. `RASPPI` selects both `-mcpu` and the compiled-in driver set (`#if RASPPI` throughout `lib/`), and the firmware selects a *different filename* per model (`boot/config64.txt:19–42`). A single card that boots Zero 2 W + Pi 3 + Pi 4 + Pi 5 needs at minimum three builds (see §2.6).

### 1.3 Exact boot-firmware requirements

`README.md:279–286` (Installation):

> Copy the Raspberry Pi firmware (from boot/ directory, do *make* there to get them) files along with the *kernel\*.img* (from sample/ subdirectory) to a SD(HC) card with FAT file system.
> It is now always recommended to copy the file *config32.txt* (for 32-bit mode) or *config64.txt* (for 64-bit mode) from the boot/ directory to the SD(HC) card and to rename it to *config.txt* there. These files are especially required to enable FIQ use on the Raspberry Pi 4. Furthermore the additional file *armstub7-rpi4.bin* (for 32-bit mode) or *armstub8-rpi4.bin* (for 64-bit mode) is required on the SD card then.

`boot/README:6–8`:

> It is now always recommended to copy the file config32.txt (for 32-bit operation, AArch32) or config64.txt (for 64-bit operation, AArch64) from this directory to the SD card and to rename it to config.txt there.

`boot/README:46–50`:

> In any case you have to copy these files onto the first FAT partition on a SD card along with the kernel\*.img file of your Circle application.
> The device tree overlay bcm2712d0.dtbo must be copied into the directory overlays/ on the SD card.

`boot/README:52–65` (FIQ on Pi 4/400/CM4):

> FIQ (fast interrupt request) support on the Raspberry Pi 4, 400 and CM4 requires an additional Circle-specific ARM stub file armstub7-rpi4.bin (AArch32) or armstub8-rpi4.bin (AArch64). These files will be built when you enter the following commands … `make armstub` / `make armstub64` … You have to copy the created file armstub?-rpi4.bin to the SD card together with the other files.

`boot/README:67–74` (Raspberry Pi 5):

> The Raspberry Pi 5 has an EEPROM, which contains the firmware. Therefore only these additional files are required on the SD card (beside kernel\*.img): `bcm2712-rpi-5-b.dtb`, `overlays/bcm2712d0.dtbo`, `config.txt` (copied from config64.txt)

#### What Circle ships vs. what the user must supply

| Artifact | Shipped in repo? | Evidence |
|---|---|---|
| `boot/config32.txt`, `boot/config64.txt` | **Yes** (text files) | present in tree; `boot/README:6–8` |
| `boot/armstub/armstub7.S`, `armstub8.S`, `Makefile` | **Yes (sources)** — binaries must be built | `boot/armstub/Makefile:7–22`; `*.bin` is git-ignored (`boot/.gitignore:1`) |
| `armstub7-rpi4.bin`, `armstub8-rpi4.bin` | **No — build locally** via `make armstub` / `make armstub64` | `boot/Makefile:62–69`, `boot/armstub/Makefile` |
| `start.elf`, `start4.elf`, `fixup.dat`, `fixup4.dat`, `bootcode.bin` | **No — download** | `boot/Makefile:19–37`, `boot/.gitignore:1–7` |
| `start_cd.elf`, `fixup_cd.dat`, `start4cd.elf`, `fixup4cd.dat` (cut-down, only used with `gpu_mem=16`) | **No — download** | `boot/Makefile:21`, `boot/README:36–44` |
| `bcm2711-rpi-4-b.dtb`, `bcm2711-rpi-400.dtb`, `bcm2711-rpi-cm4.dtb`, `bcm2712-rpi-5-b.dtb`, `bcm2712d0-rpi-5-b.dtb`, `bcm2710-rpi-zero-2-w.dtb`, `bcm2712-rpi-500.dtb`, `bcm2712-rpi-cm5-*.dtb`, `bcm2710-rpi-cm0.dtb` | **No — download** | `boot/Makefile:22–25` |
| `overlays/bcm2712d0.dtbo` | **No — download** into `overlays/` | `boot/Makefile:27,38–42`; `boot/README:49–50` |
| `LICENCE.broadcom`, `COPYING.linux` | **No — download** | `boot/Makefile:19` |
| `kernel*.img` | **No — you build it** | `Rules.mk:295–319` |

The download is a single pinned upstream revision, driven by `wget`:

```
boot/Makefile:15   FIRMWARE ?= 0641c5bf25d185d5bf25d8dcafd1da6dc8fdbf68
boot/Makefile:17   BASEURL = https://github.com/raspberrypi/firmware/raw/$(FIRMWARE)/boot
```

`make install32` / `install64` / `install` copy everything to `$(SDCARD)` and rename the config file for you (`boot/Makefile:89–111`), and `boot/Makefile:87` warns: *"You must rename config32.txt or config64.txt to config.txt on the SD card!"*

#### Exact contents of the two config files (verbatim semantics)

`boot/config32.txt`:
```
12  arm_64bit=0
13  initial_turbo=0
15  [pi4]
16  kernel=kernel7l.img
17  armstub=armstub7-rpi4.bin
18  max_framebuffers=2
20  [cm4]
21  otg_mode=1
```

`boot/config64.txt`:
```
12  arm_64bit=1
13  kernel_address=0x80000
14  initial_turbo=0
16  # Enable this for JTAG / SWD debugging!
17  #enable_jtag_gpio=1
19  [pi02]
20  kernel=kernel8.img
22  [pi2]
23  kernel=kernel8.img
25  [pi3]
26  kernel=kernel8.img
28  [pi3+]
29  kernel=kernel8.img
31  [pi4]
32  armstub=armstub8-rpi4.bin
33  kernel=kernel8-rpi4.img
34  max_framebuffers=2
36  [cm4]
37  otg_mode=1
39  [pi5]
40  kernel=kernel_2712.img
41  #framebuffer_depth=32
42  #framebuffer_ignore_alpha=1
```

Notes that matter:
* `kernel_address=0x80000` matches `Rules.mk:123` `LOADADDR = 0x80000` for AArch64; the AArch32 load address is `0x8000` (`Rules.mk:102`).
* `max_framebuffers=2` is needed for the two-display support of Circle on Pi 4 (`README.md:284`; `CBcmFrameBuffer::GetNumDisplays()` uses `PROPTAG_GET_NUM_DISPLAYS`, `lib/bcmframebuffer.cpp:441–463`).
* `[pi5] #framebuffer_depth=32` / `#framebuffer_ignore_alpha=1` are **commented out** in the shipped file — a hint that 32 bpp on Pi 5 may need explicit firmware configuration. Reason is not documented in this tree; treat as a hardware-verification item (§10.8).
* There is **no `[pi5]` entry in `config32.txt`** — consistent with AArch32 being impossible on Pi 5.

---

## 2. Build system

### 2.1 Environment (README.md:209–249, doc/windows-build.txt)

Circle is a GNU-make + GCC cross-toolchain build driven from a bash-like shell. The documented Windows paths are (a) native MSYS/MinGW Git Bash (recommended for Circle itself) or (b) WSL. `doc/windows-build.txt` gives both.

Native Windows toolchain (Git Bash), `doc/windows-build.txt:13–39`:

```
* Git for Windows (Complete package, with bash support)   from: https://gitforwindows.org
* GNU make for Windows (Complete package, except sources, Setup)
                                                          from: http://gnuwin32.sourceforge.net/packages/make.htm
* Toolchain  from: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads/
    Windows (mingw-w64-i686) hosted cross toolchains .exe installer for
        AArch32 bare-metal target (arm-none-eabi)
        AArch64 bare-metal target (aarch64-none-elf)
```

then `~/.bash_profile` (same file, lines 25–28):

```
export "PATH=$PATH:/c/Program Files (x86)/GnuWin32/bin"
export "PATH=$PATH:/c/Program Files (x86)/path/to/toolchain/bin"
```

and (lines 30–39) `cd ~/Documents; git clone …; cd circle; echo "RASPPI = 3" > Config.mk; ./makeall -j; cd sample/21-webserver; make`.

WSL: `doc/windows-build.txt:49–98`. **WSL 1 is required for serial-port work** — *"If you want to use a serial port to communicate with the Pi, you must use WSL v1 as serial ports currently aren't supported with WSL v2"* (`doc/windows-build.txt:66–68`). With WSL 2 you can still flash via the Windows `tools/flashy` node script (`doc/windows-build.txt:100–129`).

Also note `doc/windows-build.txt:45–46`: the `cFlashy` flash tool runs natively on Windows; `SERIALPORT` must be `COM3`-style but the serial *device* mapping in `Rules.mk:353–361` is WSL-aware (`/dev/ttyS<n>` ⇄ `COM<n>`).

**Toolchain assumption:** GCC only. `Rules.mk:60–67` derives `CC/CPP/AS/LD/AR/OBJCOPY/OBJDUMP/CPPFILT` from `$(PREFIX)` by simple concatenation — there is no autodetection and no fallback. `configure:89–95` refuses to write a config if `$(PREFIX)gcc` is not executable.

`README.md:216` / `257`: the recommended toolchains are **arm-gnu-toolchain-15.2.rel1** — `arm-none-eabi` for AArch32 and `aarch64-none-elf` for AArch64. Clang is experimental and tested with Clang 15 on Linux only (`doc/clang-support.txt:8`, `doc/clang-support.txt:30`).

### 2.2 Roles of `configure`, `Config.mk`, `Config2.mk`, `Rules.mk`

* **`Rules.mk`** is the master build include. It is included by the root `makeall`-driven library builds and by every sample/app via `sample/Rules.mk:29` (`include $(CIRCLEHOME)/Rules.mk`). It sets defaults and does the arch/model dispatch. `Rules.mk:25–26`:
  ```
  -include $(CIRCLEHOME)/Config.mk
  -include $(CIRCLEHOME)/Config2.mk	# is not overwritten by "configure"
  ```
  Both are optional (`-include`), so you can build with nothing but `Rules.mk` defaults.
* **`Config.mk`** is the machine-generated (or hand-written) configuration overlay. It is git-ignored (`.gitignore:84`) and overwritten by `configure` unless `-f` is given (`configure:118–126`). Documented hand-written form (`README.md:220–223`):
  ```
  RASPPI = 1
  PREFIX = arm-none-eabi-
  ```
  AArch64 form (`README.md:262–266`):
  ```
  AARCH = 64
  RASPPI = 3
  PREFIX64 = aarch64-none-elf-
  ```
* **`Config2.mk`** is a second, user-owned overlay never written by `configure` (`Rules.mk:26`; `.gitignore:85`). Useful for `SDCARD=`, `SERIALPORT=`, `USEFLASHY=` etc. `boot/Makefile:8–9` also includes both.
* **`configure`** writes `Config.mk` from command-line options and, importantly, **derives `AARCH` from the toolchain**, not from a flag:
  ```
  configure:33   echo "Note: The architecture is determined from the toolchain."
  configure:97–107
      AARCH=32
      if [ "$($CC -v 2>&1 | grep '^Target:.*64')" != "" ]; then
          AARCH=64
          if [ $RASPPI -lt 3 ]; then
              echo "Error: AArch64 requires Raspberry Pi 3 or 4!" >&2   # message is stale: 5 is also allowed
  ```
  and it pre-selects the AArch64 toolchain prefix for `-r 5` (`configure:81–87`). It also always writes `PREFIX` **or** `PREFIX64` depending on the detected arch (`configure:133–138`) plus `AARCH` and `RASPPI` (`configure:139–140`).

  Full option list (`configure:13–31`):
  | Option | Effect |
  |---|---|
  | `-r/--raspberrypi <1..5>` | `RASPPI=n` (default 1) |
  | `-p/--prefix <str>` | `PREFIX`/`PREFIX64` (default `arm-none-eabi-`, `aarch64-none-elf-` with `-r 5`) |
  | `--multicore` | `DEFINE += -DARM_ALLOW_MULTI_CORE` (`configure:141–144`) |
  | `--realtime` | `DEFINE += -DREALTIME` (`configure:145–148`) |
  | `--keymap <DE\|DV\|ES\|FR\|IT\|UK\|US>` | `DEFINE += -DDEFAULT_KEYMAP="…"` (`configure:149–152`) |
  | `--qemu` | `NO_SDHOST`, `NO_SCREEN_DMA_BURST_LENGTH`, and (32-bit) `NO_PHYSICAL_COUNTER` (`configure:153–161`) |
  | `--kernel-max-size <MB>` | `DEFINE += -DKERNEL_MAX_SIZE=0x…` (default 2 MB) (`configure:22–23, 114, 162–165`) |
  | `--c++14` / `--c++17` / `--c++20` | `STANDARD = …` (default C++17) (`configure:170–181`) |
  | `--clang` | `CLANG = 1` (`configure:129–132`) |
  | `--kasan` | `KASAN_ENABLED = 1` + shadow offset (`configure:182–189`) |
  | `-d/--define <X>` | `DEFINE += -DX` |
  | `-f/--force` | overwrite existing `Config.mk` |

### 2.3 The variables that matter

| Variable | Default | Meaning | Cite |
|---|---|---|---|
| `RASPPI` | `1` | target model 1..5; selects kernel filename, `-mcpu`, and driver set | `Rules.mk:32`, `79–121` |
| `AARCH` | `32` | `32` or `64` | `Rules.mk:31` |
| `PREFIX` | `arm-none-eabi-` | AArch32 toolchain prefix | `Rules.mk:34` |
| `PREFIX64` | `aarch64-none-elf-` | AArch64 toolchain prefix; **`PREFIX` is overwritten with `PREFIX64` when `AARCH=64`** | `Rules.mk:35`, `122` |
| `TARGET` | derived | output basename (`kernel`, `kernel7`, `kernel8-32`, `kernel7l`, `kernel8`, `kernel8-rpi4`, `kernel_2712`) | `Rules.mk:86,90,94,98,110,114,118` |
| `LOADADDR` | `0x8000` / `0x80000` | link/load address | `Rules.mk:102,123` |
| `STDLIB_SUPPORT` | `1` (`0` with `CLANG=1`) | standard-library level 0..4 | `Rules.mk:38–42`, `doc/stdlib-support.txt:8–12` |
| `FLOAT_ABI` | `hard` | `-mfloat-abi=$(FLOAT_ABI)` | `Rules.mk:48`, `84/88/92/96` |
| `OPTIMIZE` | `-O2` | | `Rules.mk:238` |
| `STANDARD` | `-std=c++17` | C++ dialect | `Rules.mk:239` |
| `C_STANDARD` | `-std=gnu99` | C dialect | `Rules.mk:240` |
| `WARNINGS` | `-Wall` | | `Rules.mk:241` |
| `CIRCLEHOME` | `..` | root of the Circle tree | `Rules.mk:23` |
| `CHECK_DEPS` | `1` | generate `.d` dependency files | `Rules.mk:45`, `270–272` |
| `GC_SECTIONS` | `0` | `-ffunction-sections -fdata-sections` + `--gc-sections` | `Rules.mk:51`, `233–236` |
| `GZIP_KERNEL` | `0` | gzip the image (AArch64 only) | `Rules.mk:54`, `312–319` |
| `KASAN_ENABLED` | `0` | KASan | `Rules.mk:57`, `128–170` |

Exact `-march`/`-mcpu` settings (`Rules.mk:83–118`):

| AARCH/RASPPI | `ARCHCPU` |
|---|---|
| 32 / 1 | `-mcpu=arm1176jzf-s -marm -mfpu=vfp -mfloat-abi=hard` |
| 32 / 2 | `-mcpu=cortex-a7 -marm -mfpu=neon-vfpv4 -mfloat-abi=hard` |
| 32 / 3 | `-mcpu=cortex-a53 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard` |
| 32 / 4 | `-mcpu=cortex-a72 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard` |
| 64 / 3 | `-mcpu=cortex-a53 -mlittle-endian` |
| 64 / 4 | `-mcpu=cortex-a72 -mlittle-endian` |
| 64 / 5 | `-mcpu=cortex-a76 -mlittle-endian` |

**There is no `-march=armv8-a` in Circle's own `Rules.mk`** for 64-bit — it uses `-mcpu=cortex-a5x/a7x/a76`. (`-march=armv8-a` does appear in this workspace's own `Makefile.core` for the emulator core; that is a separate file, outside the Circle tree.)

`lib/Makefile:82–86` additionally forces `-mno-unaligned-access` (32-bit) / `-mstrict-align` (64-bit) for the core library.

Exact `libgcc`/`libm` handling (`Rules.mk:194–231`):
* `STDLIB_SUPPORT=0` → `CFLAGS += -nostdinc` (`Rules.mk:195`).
* `STDLIB_SUPPORT=1` → links `libgcc.a` (`Rules.mk:221–222`) **and** `libm.a` if present (`Rules.mk:226–231`).
* `STDLIB_SUPPORT∈{3,4}` → `libstdc++.a` (`Rules.mk:172–176`), `libgcc_eh.a` (`Rules.mk:177–180`), and `crtbegin.o`/`crtend.o` for AArch64/GCC (`Rules.mk:181–186`).
* Otherwise (0/1/2) → **`CPPFLAGS += -fno-exceptions -fno-rtti`** and `-nostdinc++` (`Rules.mk:188–191`).
* `STDLIB_SUPPORT ∈ {0,1}` → `CFLAGS += -ffreestanding` (`Rules.mk:257–259`).

### 2.4 Build commands

```
./configure -r <n> [options]      # writes Config.mk  (or hand-write Config.mk)
./makeall clean
./makeall                         # builds the Circle libraries only
cd sample/<x> && make             # builds one application
```

`README.md:240–247`, `254–277`. `makeall` (`makeall:17–58`) descends into, in order: `tools`, `lib`, `lib/usb`, `lib/usb/gadget`, `lib/input`, `lib/fs`, `lib/fs/fat`, `lib/sched`, `lib/net`, `lib/sound`. With `--sample` it also runs `sample/makelatest` (`makeall:60–64`).

**`addon/` is NOT built by `makeall`** — `README.md:294`: *"addon: Contains contributed libraries and samples (**has to be build manually**)."* So `libsound.a` **is** built by `makeall` (it is `lib/sound`, `makeall:54–56`), but `addon/fatfs/libfatfs.a` and `addon/SDCard/libsdcard.a` are **not**. You must run `make` in `addon/fatfs` (and `addon/SDCard` if you need it) yourself.

`doc/issues.txt:5–9` — **always `./makeall clean` and `make clean` after any `Config.mk` change**; stale objects after an arch switch produce bogus link errors.

### 2.5 Linux-only build steps

| Step | Windows status | Cite |
|---|---|---|
| `./configure` (bash script) | needs Git Bash / MSYS2 / Cygwin / WSL | `configure:1`, `doc/windows-build.txt:15` |
| `./makeall` (bash script) | same | `makeall:1` |
| `boot/Makefile` `firmware` target | uses `wget`, `for` shell loops → Git Bash/WSL | `boot/Makefile:32–42` |
| `boot/Makefile` `install*` targets | `cp`, `mkdir`, `sync` → Git Bash/WSL | `boot/Makefile:71–112` |
| `chmod`-dependent script execution | needs `bash configure` on Windows if the exec bit is lost | — |
| `make flash` (python3 + pyserial) | works on Windows but COM port naming; `doc/windows-build.txt:44–46` | `Rules.mk:366–370` |
| `make flash` with Flashy (node) | works natively, `USEFLASHY=1` | `Rules.mk:374–390`, `doc/bootloader.txt:97–139` |
| **`cFlashy`** (`USEFLASHY=0`) | native executable for Linux/Windows/macOS | `doc/bootloader.txt:142–156` |
| `make monitor` (putty) | Windows-aware | `Rules.mk:353–361`, `394–396` |
| `make cat` (`stty`, `tail -f`) | **Linux only** — explicitly labelled | `Rules.mk:398–401` |

If you only want a kernel image (no serial bootloader), nothing besides `configure`/`makeall`/`make` and the `boot/` file copying is needed, all of which work under Git Bash.

### 2.6 Exactly what to set

**Pi Zero 2 W, 32-bit (`kernel7.img`):**
```
RASPPI = 2
AARCH  = 32
PREFIX = arm-none-eabi-
```
→ `TARGET = kernel7` (`Rules.mk:90`), `-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard` (`Rules.mk:88`). Equivalent: `./configure -r 2 -p arm-none-eabi-`. On the SD card: `config32.txt` → `config.txt`, `kernel7.img`, plus the shared firmware files.

*Alternative (Cortex-A53-optimized 32-bit):* `RASPPI = 3` → `kernel8-32.img`, which must be renamed to `kernel7.img` or selected with `kernel=kernel8-32.img`, because `config32.txt` has no `[pi02]`/`[pi3]` `kernel=` override.

**Pi 4, 64-bit (`kernel8-rpi4.img`):**
```
AARCH    = 64
RASPPI   = 4
PREFIX64 = aarch64-none-elf-
```
→ `TARGET = kernel8-rpi4` (`Rules.mk:114`), `-mcpu=cortex-a72 -mlittle-endian` (`Rules.mk:112`). Equivalent: `./configure -r 4 -p aarch64-none-elf-` (AARCH auto-detected as 64, `configure:97–107`). SD card: `config64.txt` → `config.txt`, `kernel8-rpi4.img`, `armstub8-rpi4.bin`, `bcm2711-rpi-4-b.dtb`, firmware `.elf`/`.dat`.

**Pi 5, 64-bit (`kernel_2712.img`):**
```
AARCH    = 64
RASPPI   = 5
PREFIX64 = aarch64-none-elf-
```
→ `TARGET = kernel_2712` (`Rules.mk:118`), `-mcpu=cortex-a76 -mlittle-endian` (`Rules.mk:116`). Equivalent: `./configure -r 5` (prefix auto-set to `aarch64-none-elf-`, `configure:81–87`). SD card (firmware is in EEPROM): `config64.txt` → `config.txt`, `kernel_2712.img`, `bcm2712-rpi-5-b.dtb`, `overlays/bcm2712d0.dtbo` (`boot/README:69–74`).

**Pi 3/Zero 2 W, 64-bit** (`kernel8.img`): `AARCH=64 RASPPI=3 PREFIX64=aarch64-none-elf-` (`Rules.mk:110`).

**Multi-target SD card (recommended layout):** build these three and copy all outputs to the same FAT partition (see `README.md:234` for the multi-build advice):
1. `RASPPI=2 AARCH=32` → `kernel7.img` (Zero 2 W, Pi 2, Pi 3 in 32-bit)
2. `RASPPI=4 AARCH=64` → `kernel8-rpi4.img` + `armstub8-rpi4.bin` (Pi 4/400/CM4)
3. `RASPPI=5 AARCH=64` → `kernel_2712.img` (Pi 5/500)
plus `AARCH=64 RASPPI=3` → `kernel8.img` if you want 64-bit on Zero 2 W/Pi 3, and `RASPPI=1 AARCH=32` → `kernel.img` for Pi 1/Zero.
Then use `config64.txt` as `config.txt` **only if** every target boots 64-bit — otherwise you must merge the `[pi4]`/`[cm4]` stanzas of `config32.txt` into `config64.txt` and add a `kernel7.img` selection for `[pi02]`/`[pi3]`, because `arm_64bit` is a *global* setting in both shipped files (`boot/config32.txt:12`, `boot/config64.txt:12`). This merge is **not** provided by Circle and is your responsibility.

---

## 3. Application structure — the exact starter pattern

### 3.1 The real entry point is `main()`, not a library `CKernel`

`CKernel` is **not** a Circle library class. There is no `CKernel` in `lib/` or `include/`; every sample defines its own class named `CKernel` in its own `kernel.h`/`kernel.cpp`, and `main()` constructs it on the stack.

The library entry sequence is `lib/sysinit.cpp`:

```
startup.S / startup64.S  →  sysinit ()
lib/sysinit.cpp:310   void sysinit (void)
  :331–339   clear .bss and .tbss
  :341–347   halt() silently if the kernel image is larger than KERNEL_MAX_SIZE
  :349–351   CMemorySystem Memory;  CMachineInfo MachineInfo;
  :353–355   Memory.SetupHighMem ()            (RASPPI >= 4)
  :379–383   CInterruptSystem InterruptSystem; Initialize()
  :385–391   CSouthbridge Southbridge; Initialize()   (RASPPI >= 5)
  :393–399   call all static constructors (.init_array)
  :401–402   extern int MAINPROC (void);  int nResult = MAINPROC ();
  :403–416   EXIT_REBOOT → chain boot or reboot()
  :418–422   EXIT_POWER_OFF → poweroff()      (RASPPI >= 5)
  :424       halt ()
```

`MAINPROC` defaults to `main` (`include/circle/sysconfig.h:496–498`). So **`int main(void)` is the application entry point** and it must never return meaningfully (the samples call `halt()`).

### 3.2 Canonical `main.cpp` (verbatim from `sample/05-usbsimple/main.cpp:20–47`)

```cpp
#include "kernel.h"
#include <circle/startup.h>

int main (void)
{
	// cannot return here because some destructors used in CKernel are not implemented

	CKernel Kernel;
	if (!Kernel.Initialize ())
	{
		halt ();
		return EXIT_HALT;
	}

	TShutdownMode ShutdownMode = Kernel.Run ();

	switch (ShutdownMode)
	{
	case ShutdownReboot:
		reboot ();
		return EXIT_REBOOT;

	case ShutdownHalt:
	default:
		halt ();
		return EXIT_HALT;
	}
}
```

This file is byte-identical across essentially all samples (`sample/01-gpiosimple/main.cpp`, `sample/34-sounddevices/main.cpp`, `addon/fatfs/sample/main.cpp`, `app/template/main.cpp`).

### 3.3 The `CKernel` class skeleton

`sample/05-usbsimple/kernel.h:35–64`:

```cpp
enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CUSBHCIDevice		m_USBHCI;
};
```

**Member declaration order is load-bearing** — the comment *"do not change this order"* is real: the constructor initialiser list wires pointers between these members (`kernel.cpp:27–34`), and several singletons self-register in their constructors (`CDeviceNameService`, `CInterruptSystem`, `CTimer::s_pThis`, `CMachineInfo`). Construct `CActLED` and `CKernelOptions` first, `CDeviceNameService` before anything that registers a device, `CInterruptSystem` before `CTimer`, and `CLogger` after the log target exists.

Constructor (`sample/05-usbsimple/kernel.cpp:27–34`):

```cpp
CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer)
{
	m_ActLED.Blink (5);	// show we are alive
}
```

Note the idioms:
* `CScreenDevice m_Screen (m_Options.GetWidth (), m_Options.GetHeight ())` — `0, 0` means "let the firmware pick the optimal mode" (`lib/koptions.cpp:30–31`, `lib/bcmframebuffer.cpp:70–97`).
* `CTimer m_Timer (&m_Interrupt)` and `CLogger m_Logger (logLevel, &m_Timer)` — both take the interrupt system / timer by pointer.
* `CUSBHCIDevice m_USBHCI (&m_Interrupt, &m_Timer [, bPlugAndPlay])` — one spelling works on every model because `CUSBHCIDevice` is a macro alias (§6.1).

`Initialize()` — the canonical ordered sequence (`sample/05-usbsimple/kernel.cpp:40–81`), essentially identical in every sample:

```cpp
boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK) bOK = m_Screen.Initialize ();          // 1. framebuffer / console
	if (bOK) bOK = m_Serial.Initialize (115200);    // 2. UART

	if (bOK)                                        // 3. logger target
	{
		CDevice *pTarget = m_DeviceNameService.GetDevice (m_Options.GetLogDevice (), FALSE);
		if (pTarget == 0)
		{
			pTarget = &m_Screen;
		}

		bOK = m_Logger.Initialize (pTarget);
	}

	if (bOK) bOK = m_Interrupt.Initialize ();       // 4. IRQ/FIQ system
	if (bOK) bOK = m_Timer.Initialize ();           // 5. system timer / kernel timers
	if (bOK) bOK = m_USBHCI.Initialize ();          // 6. USB host controller

	return bOK;
}
```

The order is mandatory: `CDeviceNameService::GetDevice ("tty1", FALSE)` only resolves after `m_Screen.Initialize()` registered it (`lib/screen.cpp:99–102`).

`Run()` is where you put the emulator. `sample/05-usbsimple/kernel.cpp:83–116` shows a minimal one; `sample/34-sounddevices/kernel.cpp:136–213` is the closest structural model for an emulator (device selection → queue setup → `Start()` → paced loop).

### 3.4 `ActLED`, `CScheduler`, and the other optional components

* **`CActLED`** (`include/circle/actled.h:27–50`): `On()`, `Off()`, `Blink (unsigned nCount, unsigned nTimeOnMs = 200, unsigned nTimeOffMs = 500)`, static `Get()`. On `RASPPI >= 5` there is no `CGPIOPin` member — it goes through `CVirtualGPIOPin` (`lib/actled.cpp:30–31, 39–46, 72, 88`), i.e. the Pi 5 power-LED is driven by the VideoCore, not by a raw GPIO. `error_halt()` in the library uses `ActLED.Blink (errnum, 100, 300)` to signal fatal errors (`lib/sysinit.cpp:223–233`).
* **`CScheduler`** (`include/circle/sched/scheduler.h:41–140`): *"Cooperative non-preemptive scheduler"* — `Yield()`, `Sleep()`, `MsSleep()`, `usSleep()`, `GetCurrentTask()`, `GetTask(name)`, `RegisterTaskSwitchHandler()`, `RegisterTaskTerminationHandler()`, `SuspendNewTasks()`/`ResumeNewTasks()`, `EnumerateTasks()`, `ListTasks()`, `static CScheduler *Get()`, `static boolean IsActive()`. **The scheduler only exists if you declare a `CScheduler` member in `CKernel`** — see `sample/19-tasks/kernel.h:68` `CScheduler m_Scheduler;`. `CTask` self-registers in its constructor (`lib/sched/task.cpp:53` `CScheduler::Get()->AddTask (this)`), so `new CMyTask(...)` before constructing `CScheduler` would assert.
* **`CTask`** (`include/circle/sched/task.h:43–134`): derive and override `virtual void Run (void)`. Ctor `CTask (unsigned nStackSize = TASK_STACK_SIZE, boolean bCreateSuspended = FALSE)` (`task.h:48`). Also `Start()`, `Suspend()`, `Resume()`, `IsSuspended()`, `Terminate()`, `WaitForTermination()`, `SetName()`, `GetName()`, `SetUserData()/GetUserData()` with `TASK_USER_DATA_USER` free for the app (`task.h:79–92`), `GetStack()`.
* **`CExceptionHandler`** — declare `CExceptionHandler m_ExceptionHandler;` and it installs itself (`sample/05-usbsimple/kernel.h:59`). `sample/04-timer/kernel.cpp:100–110` deliberately faults to demonstrate the stack trace.
* **`CMultiCoreSupport`** (`include/circle/multicore.h:40–88`) — only compiled when `ARM_ALLOW_MULTI_CORE` is defined (`multicore.h:25`; `sysconfig.h:153–158`; `configure --multicore`). Derive, implement `virtual void Run (unsigned nCore)`, call `Initialize()` **last** in `CKernel::Initialize()` (`doc/multicore.txt:21–23`).

### 3.5 Minimal compiling skeleton for this project

`app/` is the documented place for your own application (`README.md:295`). A working minimal skeleton (every API here is verified in the tree):

**`app/atari/main.cpp`** — identical to `sample/05-usbsimple/main.cpp:20–47`.

**`app/atari/kernel.h`**

```cpp
#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/types.h>

enum TShutdownMode { ShutdownNone, ShutdownHalt, ShutdownReboot };

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);
	TShutdownMode Run (void);

private:
	static void KeyStatusHandlerRaw (unsigned char ucModifiers,
					 const unsigned char RawKeys[6]);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CUSBHCIDevice		m_USBHCI;
	CScheduler		m_Scheduler;
	CSoundBaseDevice	*m_pSound;
	CUSBKeyboardDevice	*m_pKeyboard;
};

#endif
```

**`app/atari/kernel.cpp`**

```cpp
#include "kernel.h"
#include <circle/sound/pwmsoundbasedevice.h>	// -> ...-rp1.h automatically on RASPPI >= 5
#include <circle/util.h>
#include <assert.h>

static const char FromKernel[] = "kernel";

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),	// TRUE: USB plug-and-play
	m_pSound (nullptr),
	m_pKeyboard (nullptr)
{
	m_ActLED.Blink (5);
}

CKernel::~CKernel (void) {}

boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK) bOK = m_Screen.Initialize ();
	if (bOK) bOK = m_Serial.Initialize (115200);

	if (bOK)
	{
		CDevice *pTarget = m_DeviceNameService.GetDevice (m_Options.GetLogDevice (), FALSE);
		if (pTarget == 0) pTarget = &m_Screen;
		bOK = m_Logger.Initialize (pTarget);
	}

	if (bOK) bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Timer.Initialize ();
	if (bOK) bOK = m_USBHCI.Initialize ();

	return bOK;
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "atari: build " __DATE__ " " __TIME__);

	// --- audio (see section 5) ---
	m_pSound = new CPWMSoundBaseDevice (&m_Interrupt, 48000, 2048);
	if (!m_pSound->AllocateQueue (100))			// 100 ms ring buffer
		m_Logger.Write (FromKernel, LogPanic, "Cannot allocate sound queue");
	m_pSound->SetWriteFormat (SoundFormatSigned16, 2);
	if (!m_pSound->Start ())
		m_Logger.Write (FromKernel, LogPanic, "Cannot start sound device");

	// --- PAL frame pacing: 50 Hz at a 1 MHz clock ---
	const unsigned nFrameTicks = CLOCKHZ / 50;		// 20000
	unsigned nNext = CTimer::GetClockTicks () + nFrameTicks;

	for (;;)
	{
		// USB hot-plug housekeeping must run from TASK_LEVEL
		if (m_USBHCI.UpdatePlugAndPlay () && m_pKeyboard == 0)
		{
			m_pKeyboard = (CUSBKeyboardDevice *) m_DeviceNameService.GetDevice ("ukbd1", FALSE);
			if (m_pKeyboard != 0)
				m_pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandlerRaw);
		}

		// emulator: one PAL frame
		// EmulateFrame ();

		// top up the audio queue with everything that fits
		// unsigned nFree = m_pSound->GetQueueSizeFrames () - m_pSound->GetQueueFramesAvail ();
		// ... render nFree frames, then m_pSound->Write (buf, nBytes);

		while ((int) (CTimer::GetClockTicks () - nNext) < 0)
		{
			// busy-wait to the frame boundary (add CCPUThrottle if you need full clock)
		}
		nNext += nFrameTicks;
	}

	return ShutdownHalt;
}

void CKernel::KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	// ucModifiers: LCTRL/LSHIFT/ALT/LWIN/RCTRL/RSHIFT/ALTGR/RWIN (include/circle/usb/usbhid.h:56-63)
	// RawKeys[0..5]: raw HID usage codes, 0 = empty slot
}
```

**`app/atari/Makefile`**

```make
CIRCLEHOME = ../..

OBJS	= main.o kernel.o

LIBS	= $(CIRCLEHOME)/lib/sched/libsched.a \
	  $(CIRCLEHOME)/lib/sound/libsound.a \
	  $(CIRCLEHOME)/lib/usb/libusb.a \
	  $(CIRCLEHOME)/lib/input/libinput.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a \
	  $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/sample/Rules.mk

-include $(DEPS)
```

(Library list and link order taken from `sample/34-sounddevices/Makefile:19–24`; note GNU `ld` resolves archives left-to-right, and `Rules.mk:300` wraps them in `--start-group … --end-group` for the GCC path, so order is less critical than with Clang — `doc/clang-support.txt:65–66`.)

---

## 4. Video / framebuffer

### 4.1 `CBcmFrameBuffer` — exact constructor

`include/circle/bcmframebuffer.h:44–46`:

```cpp
CBcmFrameBuffer (unsigned nWidth, unsigned nHeight, unsigned nDepth,
                 unsigned nVirtualWidth = 0, unsigned nVirtualHeight = 0,
                 unsigned nDisplay = 0, boolean bDoubleBuffered = FALSE);
```

| Param | Meaning | Cite |
|---|---|---|
| `nWidth`, `nHeight` | **physical** display size in pixels. Pass `0,0` to have the firmware's preferred mode detected (`PROPTAG_GET_DISPLAY_DIMENSIONS`). If the detected value is outside `640..4096 × 480..2160` it is replaced by `640×480`. | `bcmframebuffer.cpp:70–97` |
| `nDepth` | bits per pixel: `8`, `16` or `32` (`CDisplay::I8`, `RGB565`, `ARGB8888`). Selected by the base class ctor: `CDisplay (nDepth == 8 ? I8 : (nDepth == 16 ? RGB565 : ARGB8888))`. | `bcmframebuffer.cpp:49`, `display.h:62–71` |
| `nVirtualWidth`, `nVirtualHeight` | size of the **virtual** buffer (the scrollable/pannable area). If either is 0 it defaults to `nWidth, nHeight*(1+bDoubleBuffered)`. | `bcmframebuffer.cpp:99–104` |
| `nDisplay` | zero-based display index (Pi 4/5 dual HDMI). `GetNumDisplays()` returns 1 on `RASPPI <= 3`. | `bcmframebuffer.h:46`, `bcmframebuffer.cpp:441–463` |
| `bDoubleBuffered` | if `TRUE` and virtual size not given, virtual height becomes `2 * nHeight`. | `bcmframebuffer.cpp:103` |

`Initialize()` issues all the tags in one mailbox transaction (`TBcmFrameBufferInitTags`, `bcmframebuffer.h:31–39` = `PROPTAG_SET_PHYS_WIDTH_HEIGHT`, `SET_VIRT_WIDTH_HEIGHT`, `SET_DEPTH`, `SET_VIRTUAL_OFFSET`, `ALLOCATE_BUFFER`, `GET_PITCH` — `bcmpropertytags.h:56–69`) and returns `FALSE` if any of width/height/depth/base address came back zero (`bcmframebuffer.cpp:151–197`).

### 4.2 Getting the buffer pointer and pitch

```cpp
u32 GetBuffer (void) const;   // ARM physical base address of the framebuffer
u32 GetPitch  (void) const;   // bytes per scanline
u32 GetSize   (void) const;   // total allocated size in bytes
u32 GetWidth  (void) const;   // physical width
u32 GetHeight (void) const;   // physical height
u32 GetVirtWidth  (void) const;
u32 GetVirtHeight (void) const;
u32 GetDepth  (void) const;
```
Declared `bcmframebuffer.h:60–67`, implemented `bcmframebuffer.cpp:320–358`.

Important detail: the base address is **masked** — `m_nBufferPtr = m_nBufferBaseAddress & 0x3FFFFFFF` (`bcmframebuffer.cpp:176`). So `GetBuffer()` returns a ≤30-bit ARM physical address. (This is a correctness hazard if the firmware ever allocates the framebuffer above 1 GB — see §10.8.)

`GetPitch()` is in **bytes**, not pixels: `SetPixel()` computes `nPosY*m_nPitch` and then indexes a `u16*`/`u32*` (`bcmframebuffer.cpp:199–218`, via the `PTR_ADD` macro at `bcmframebuffer.cpp:26`). To build a pixel pointer yourself:

```cpp
u8 *pFb   = (u8 *) (uintptr) m_FrameBuffer.GetBuffer ();
u32 nPitch = m_FrameBuffer.GetPitch ();
u16 *pRow  = (u16 *) (pFb + nY * nPitch);   // 16 bpp
```
(`u32`/`u16`/`u8` and `uintptr` come from `include/circle/types.h`.)

### 4.3 Supported resolutions and depths

Circle does **not** enumerate modes. The firmware decides, driven by the display's EDID and by `config.txt`; Circle only asks for a size and gets back what it got. The only in-code constraint is the sanity clamp in the constructor:

```
bcmframebuffer.cpp:83–90
    if (   m_nWidth  < 640
        || m_nWidth  > 4096
        || m_nHeight < 480
        || m_nHeight > 2160)
    {
        m_nWidth  = 640;
        m_nHeight = 480;
    }
```

So:
* **1280×720 @16/32 bpp — yes, supported** (within `640..4096 × 480..2160`).
* **1920×1080 @16/32 bpp — yes, supported** (same range). Note the depth is requested via `PROPTAG_SET_DEPTH` and its return value is not re-checked against the request, so a firmware that refuses 32 bpp leaves you with a mismatched stride; `CScreenDevice` guards this only trivially (`lib/screen.cpp:83–86` compares `GetDepth()` — the locally stored value — against `DEPTH`).
* **4K — conditionally.** `doc/issues.txt:21–24`: *"According to tests with the current firmware, 4K display resolutions on a Raspberry Pi 4 with Circle are only possible, if the option `hdmi_enable_4kp60=1` is enabled in the file config.txt and are not possible with the cut-down firmware (start_cd.elf)."*
* **8 bpp** is also legal (`I8` + `SetPalette`/`SetPalette32`/`UpdatePalette`, `bcmframebuffer.h:49–50,69`, `bcmframebuffer.cpp:129–149, 360–384`) — not useful for an Atari emulator.

**Requesting a specific HDMI mode:** there is **no** mode-setting API in Circle. You control it via the firmware:
1. `config.txt` — standard Raspberry Pi `hdmi_group` / `hdmi_mode` / `hdmi_enable_4kp60` settings (**not** shipped by Circle; the shipped `config32.txt`/`config64.txt` contain none of them).
2. `cmdline.txt` — `doc/cmdline.txt:7`:
   > `width=640 height=480` — *"Changes screen size (default is maximum size)"*
   parsed into `CKernelOptions::GetWidth()/GetHeight()` (`lib/koptions.cpp:71–86, 233–238`).
3. Constructing `CBcmFrameBuffer`/`CScreenDevice` with explicit `nWidth`/`nHeight`.

The de-facto pattern used by every sample is to pass `m_Options.GetWidth(), m_Options.GetHeight()` (default `0,0` → optimal mode) and only override when `width=`/`height=` is present in `cmdline.txt`.

### 4.4 Double buffering and frame presentation

The complete presentation API is:

```cpp
void    SetPixel (unsigned nPosX, unsigned nPosY, TRawColor nColor);
void    SetArea  (const TArea &rArea, const void *pPixels,
                  TAreaCompletionRoutine *pRoutine = nullptr, void *pParam = nullptr);
boolean SetVirtualOffset (u32 nOffsetX, u32 nOffsetY);
boolean WaitForVerticalSync (void);
```
`bcmframebuffer.h:54–73`; `CDisplay::TArea { unsigned x1, x2, y1, y2; }` is inclusive on both ends (`display.h:75–78`).

* `SetArea()` blits a rectangle. If `SCREEN_DMA_BURST_LENGTH` is defined (it defaults to 2, `sysconfig.h:252–256`) it uses `CDMAChannel::SetupMemCopy2D` + DMA, and the completion routine is invoked from the DMA IRQ; otherwise it is a row-by-row `memcpy` and the completion routine is called inline (`bcmframebuffer.cpp:220–304`). Note the DMA path uses a busy spin on `m_nDMAInUse` (`bcmframebuffer.cpp:224–227`).
* `SetVirtualOffset(x, y)` pans the visible window inside the virtual buffer (`bcmframebuffer.cpp:386–402`) — this is the flip primitive.
* `WaitForVerticalSync()` is a firmware mailbox round-trip (`PROPTAG_WAIT_FOR_VSYNC`) (`bcmframebuffer.cpp:404–411`).

**Important:** `CBcmFrameBuffer` has **no `SetPhysicalSize`/`SetVirtualSize` method** in this revision. The double-buffer geometry is fixed at construction time via `nVirtualWidth/nVirtualHeight` (or `bDoubleBuffered=TRUE`); after that only the offset can change.

The reference double-buffering implementation is `C2DGraphics::UpdateDisplay()`, `lib/2dgraphics.cpp:540–567`:

```cpp
void C2DGraphics::UpdateDisplay (void)
{
#if RASPPI <= 4
	if (m_bVSync)
	{
		unsigned nBaseHeight = m_bBufferSwapped ? m_nHeight : 0;
		CDisplay::TArea Area {0, m_nWidth-1, nBaseHeight, nBaseHeight + m_nHeight-1};

		m_pFrameBuffer->WaitForVerticalSync();
		m_pFrameBuffer->SetArea (Area, m_pBuffer8);
		m_pFrameBuffer->SetVirtualOffset(0, m_bBufferSwapped ? m_nHeight : 0);
		m_bBufferSwapped = !m_bBufferSwapped;
	}
	else
#endif
	{ /* single-buffered SetArea */ }
}
```

Note the `#if RASPPI <= 4` — on **Pi 5 this path is compiled out** and `C2DGraphics` degradation to single-buffered `SetArea` is used. If you want tear-free double buffering on Pi 5 you must implement it yourself with `SetArea` + `SetVirtualOffset` and be prepared to verify `WaitForVerticalSync()` behaviour on that SoC.

If you implement it yourself, the pattern is:
```cpp
// construct with virtual height = 2 * height
CBcmFrameBuffer Fb (1920, 1080, 16, 1920, 2160);
// or CBcmFrameBuffer Fb (1920, 1080, 16, 0, 0, 0, TRUE);

// draw into (frame & 1) half of GetBuffer(), then flip:
CDisplay::TArea Area {0, 1919, (m_bFront ? 1080u : 0u), (m_bFront ? 2159u : 1079u)};
Fb.SetArea (Area, pMyBackBuffer);
Fb.SetVirtualOffset (0, m_bFront ? 1080 : 0);
m_bFront = !m_bFront;
```
(`Area` semantics from `bcmframebuffer.cpp:545–550`; `TArea` field order from `display.h:75–78`.)

A cheaper approach that many emulators use: get the raw pointer/pitch once and write pixels directly, then call `WaitForVerticalSync()` at the frame boundary. There is no Circle API that flips a raw pointer, so you either write directly into the visible half (tearing possible) or use the `SetArea`/`SetVirtualOffset` scheme above.

`C2DGraphics` itself is a ready-made software 2D layer on top of `CBcmFrameBuffer` — `C2DGraphics (unsigned nWidth, unsigned nHeight, boolean bVSync = TRUE, unsigned nDisplay = 0)` with `Initialize()`, `ClearScreen()`, `DrawRect()`, `DrawRectOutline()`, `DrawLine()`, `DrawCircle()`, `DrawCircleOutline()`, `DrawImage()`, `DrawImageTransparent()`, `DrawImageRect()`, `DrawImageRectTransparent()`, `DrawText()`, `UpdateDisplay()`, `GetWidth()/GetHeight()`, `GetBuffer()`, `GetDisplay()`, `Resize()` (`include/circle/2dgraphics.h:78–221`, `lib/2dgraphics.cpp:530–567`). It also has a `C2DGraphics (CDisplay *pDisplay)` ctor with *"no VSync support"* (`2dgraphics.h:79–81`).

### 4.5 `CScreenDevice` — the character console

`include/circle/screen.h:139–141`:

```cpp
CScreenDevice (unsigned nWidth, unsigned nHeight, const TFont &rFont = DEFAULT_FONT,
               CCharGenerator::TFontFlags FontFlags = CCharGenerator::FontFlagsNone,
               unsigned nDisplay = 0);
```

Differences from `CBcmFrameBuffer`, concretely:

| Aspect | `CBcmFrameBuffer` | `CScreenDevice` |
|---|---|---|
| Purpose | raw pixels | character console **plus** pixel access |
| Base class | `CDisplay` (`display.h:34`) | `CDevice` (`screen.h:131`), so `Write()`/`Read()` device semantics |
| Depth | caller-chosen `nDepth` | fixed at compile time by the `DEPTH` macro — `#ifndef DEPTH / #define DEPTH 16` (`screen.h:30–32`); must be 8, 16 or 32 (`screen.h:50–127`) |
| Internals | `SetArea` blits to the visible buffer | owns a `CBcmFrameBuffer` **and** a `CTerminalDevice` (`lib/screen.cpp:52–53, 88`) |
| Resize | recreate the object | `boolean Resize (unsigned nWidth, unsigned nHeight)` (`screen.h:154`, `lib/screen.cpp:107–119`) |
| Escape sequences | none | full ANSI subset — cursor, colour, bold/reverse, scroll region, autopage (`doc/screen.txt:1–32`) |
| Extra | palette, `WaitForVerticalSync`, `SetVirtualOffset`, `SetBacklightBrightness` | `Write()`, `GetPixel()`, `Rotor()`, `SetCursorBlock()`, `Update()` |
| `GetFrameBuffer()` | n/a (is one) | returns the underlying `CBcmFrameBuffer *` (`screen.h:166–169`, `lib/screen.cpp:141–144`) — **only if `SCREEN_HEADLESS` is not defined** |

Overriding the screen depth for the whole build: pass `-DDEPTH=32` (e.g. `CFLAGS += -DDEPTH=32` in your app Makefile) because `screen.h:30` guards with `#ifndef DEPTH`.

Character geometry: the font is `DEFAULT_FONT` = `Font8x16` unless overridden (`include/circle/font.h:43–44`; alternatives `Font8x8/8x10/8x12/8x14/12x22`, `font.h:36–40`). Columns/rows are `GetColumns()`/`GetRows()` (`screen.h:162–164`).

**Headless mode is mandatory on Pi 4/5 without a display** — `doc/issues.txt:46–49`:

> The class CScreenDevice does not initialize successfully on the Raspberry Pi 4, when a display is not attached. This is different from earlier models. To run most of the sample programs headless, you have to enable the system option SCREEN_HEADLESS in the file include/circle/sysconfig.h.

`sysconfig.h:364–375` adds Pi 5 to that statement: *"especially on the Raspberry Pi 4 and 5 this is not the case any more … In your own headless applications you should just not use the CScreenDevice class instead and direct the logger output to CSerialDevice or CNullDevice."* With `SCREEN_HEADLESS` defined, `CScreenDevice` becomes a stub that returns `TRUE` from `Initialize()` and discards everything (`lib/screen.cpp:196–289`), and `GetFrameBuffer()` does not exist.

`CBcmFrameBuffer::SetBacklightBrightness()` (`bcmframebuffer.h:75–79`, `bcmframebuffer.cpp:413–421`) is documented as *"Tested with the Official Touchscreen only (level can be about 0..180)"*; it is also driven by `cmdline.txt` `backlight=` via `CKernelOptions::GetBacklight()` (`bcmframebuffer.cpp:183–194`, `doc/cmdline.txt:83–84`).

---

## 5. Audio

### 5.1 The class hierarchy

All output devices derive from `CSoundBaseDevice` (`include/circle/sound/soundbasedevice.h:67`). `lib/sound/Makefile:27–38` determines what is actually compiled:

```
27  OBJS	= soundbasedevice.o pwmsounddevice.o hdmisoundbasedevice.o \
28  	  pcm512xsoundcontroller.o wm8960soundcontroller.o
30  ifneq ($(strip $(RASPPI)),5)
31  OBJS	+= dmasoundbuffers.o i2ssoundbasedevice.o pwmsoundbasedevice.o
32  else
33  OBJS	+= i2ssoundbasedevice-rp1.o pwmsoundbasedevice-rp1.o
34  endif
36  ifneq ($(filter 4 5,$(RASPPI)),)
37  OBJS	+= usbsoundbasedevice.o usbsoundcontroller.o
38  endif
```

So: PWM + HDMI sound exist for **all** models (including Pi 5, with RP1 implementations); I2S exists for all models; USB sound exists for **RASPPI 4 and 5 only**.

### 5.2 `CSoundBaseDevice` — the shared API

**Constructors** (`soundbasedevice.h:71–93`):
```cpp
CSoundBaseDevice (void);                       // then call Setup()
CSoundBaseDevice (TSoundFormat HWFormat, u32 nRange32, unsigned nSampleRate,
                  boolean bSwapChannels = FALSE);
void Setup (TSoundFormat HWFormat, u32 nRange32, unsigned nSampleRate,
            unsigned nHWTXChannels, unsigned nHWRXChannels, boolean bSwapChannels);
```

**Sample formats — the complete enum** (`soundbasedevice.h:42–52`), all interleaved little-endian:

```cpp
enum TSoundFormat
{
	SoundFormatUnsigned8,    // Write/Read,  HWFormat none
	SoundFormatSigned16,     // Write/Read,  HWFormat output/input
	SoundFormatSigned24,     // Write/Read,  HWFormat output/input (occupies 3 bytes)
	SoundFormatSigned24_32,  // Write/Read,  HWFormat output/input (occupies 4 bytes)
	SoundFormatUnsigned32,   // HWFormat output only
	SoundFormatIEC958,       // HWFormat output only
	SoundFormatFloat32,      // Write/Read,  HWFormat none (range [-1.0 .. 1.0])
	SoundFormatUnknown
};
```
Ranges per format are set in `lib/sound/soundbasedevice.cpp:99–136` (`Signed16` → `-32768..32767`; `Signed24`/`Signed24_32`/`IEC958` → `-(1<<23)+1 .. (1<<23)-1`; `Unsigned32` → `0 .. nRange32-1`).

**Queue-based output (recommended, no callback):**
```cpp
boolean AllocateQueue       (unsigned nSizeMsecs);    // 1..1000 ms
boolean AllocateQueueFrames (unsigned nSizeFrames);   // 1..sampleRate
void    SetWriteFormat      (TSoundFormat Format, unsigned nChannels = 2);
int     Write               (const void *pBuffer, size_t nCount);  // bytes
unsigned GetQueueSizeFrames (void);
unsigned GetQueueFramesAvail(void);
void    Flush               (void);
void    RegisterNeedDataCallback (TSoundDataCallback *pCallback, void *pParam);
```
(`soundbasedevice.h:130–166`; implementations `lib/sound/soundbasedevice.cpp:202–238` for the allocators, `:278–393` for `Write`.)

Two facts that matter for an emulator loop:
1. **`Write()` never blocks.** It fills only the free space and returns the number of bytes actually consumed (`lib/sound/soundbasedevice.cpp:295–307` returns `nBytes`, the fast path; the conversion paths stop when `GetQueueBytesFree() < m_nHWTXFrameSize`, `:312–313`, `:364–365`). A short return means samples were dropped — the sample logs *"Sound data dropped"* (`sample/34-sounddevices/kernel.cpp:229–232`).
2. Queue sizing: `m_nQueueSize = (m_nHWTXFrameSize * m_nSampleRate * nSizeMsecs + 999)/1000 + 1` (`lib/sound/soundbasedevice.cpp:208`), and `m_nNeedDataThreshold = m_nQueueSize / 2` (`:216`). `RegisterNeedDataCallback()` therefore fires when **at least half the queue is empty** (`soundbasedevice.h:162–166`).

**Callback-based output (the other supported method):**
```cpp
protected:
	virtual unsigned GetChunk (s16 *pBuffer, unsigned nChunkSize);
	virtual unsigned GetChunk (u32 *pBuffer, unsigned nChunkSize);
	u32 ConvertIEC958Sample (u32 nSample, unsigned nFrame);   // for IEC958 framing only
```
`soundbasedevice.h:213–234`. `nChunkSize` is in **words**, and *"Each sample consists of two words (Left channel, right channel)"* (`soundbasedevice.h:219–220`). It is called from **IRQ context**. *"Transfer will stop if 0 is returned"* (`soundbasedevice.h:217–218`). `AreChannelsSwapped()` tells you whether the right channel must be written first (`soundbasedevice.h:168–169`). The two methods are mutually exclusive: *"Not used, if GetChunk() is overloaded"* (`soundbasedevice.h:132, 137, 142, 148, 153`).

**Input:** `AllocateReadQueue`, `SetReadFormat`, `Read`, `GetReadQueueSizeFrames`, `GetReadQueueFramesAvail`, `RegisterHaveDataCallback`, `virtual void PutChunk (const s16*/const u32*, unsigned nChunkSize)` (`soundbasedevice.h:171–229`).

**Optional controller:** `virtual CSoundController *GetController (void)` returning `nullptr` if unsupported (`soundbasedevice.h:125–126`; `CSoundController` API in `include/circle/sound/soundcontroller.h:28–148` — jacks `JackHeadphone`/`JackHDMI`/`JackLineOut`…, controls `ControlMute`/`ControlVolume`/`ControlALC`).

**Multi-core note:** *"In a multi-core environment all methods, except if otherwise noted, have to be called or will be called (for callbacks) on core 0."* (`soundbasedevice.h:64–65`).

### 5.3 `CPWMSoundBaseDevice` (PWM, via the 3.5 mm jack / 40-pin header)

Identical public signature on all models — the header switches implementation internally (`include/circle/sound/pwmsoundbasedevice.h:23–25` → `pwmsoundbasedevice-rp1.h` for `RASPPI >= 5`):

```cpp
CPWMSoundBaseDevice (CInterruptSystem *pInterrupt,
                     unsigned nSampleRate = 44100,
                     unsigned nChunkSize  = 2048,
                     boolean  bMSMode     = TRUE);
```
(`pwmsoundbasedevice.h:42–45`; `pwmsoundbasedevice-rp1.h:38–41`.)

* Hardware range: `GetRangeMin()`/`GetRangeMax()`/`GetRange()` (`pwmsoundbasedevice.h:50–54`). Range is `CLOCK_RATE / nSampleRate` (`lib/sound/pwmsoundbasedevice.cpp:109`, `-rp1.cpp:93`).
* **Clock rate:** `250 MHz` on `RASPPI <= 3`, `125 MHz` on `RASPPI == 4` using `ARM_PWM1_BASE` / `DREQSourcePWM1` (`lib/sound/pwmsoundbasedevice.cpp:35–43`); `50 MHz` on Pi 5 (`lib/sound/pwmsoundbasedevice-rp1.cpp:33`, `StartRate (CLOCK_RATE)` at `:214`). The clock divider is `(CLOCK_RATE + nSampleRate/2) / nSampleRate`, i.e. the sample rate is **derived from the PWM clock** and is only approximately right: the effective rate is `CLOCK_RATE / round(CLOCK_RATE/nSampleRate)`. Range of usable rates is therefore bounded by `RANGE` register width; there is **no assert on `nSampleRate`** in either implementation (only `assert (m_nChunkSize > 0)` and even parity, `lib/sound/pwmsoundbasedevice.cpp:124–125`).
* Pins: `GPIOPinAudioLeft`/`GPIOPinAudioRight` (virtual pins 54/55, `gpiopin.h:31–36`) at `GPIOModeAlternateFunction0`, or GPIO18/GPIO19 at Alt5 when `USE_GPIO18_FOR_LEFT_PWM_ON_ZERO` / `USE_GPIO19_FOR_RIGHT_PWM_ON_ZERO` are defined (`lib/sound/pwmsoundbasedevice.cpp:110–119`; `sysconfig.h:118–134` for Zero/Zero 2 W, `sysconfig.h:193–203` for Pi 5). Left/right may be swapped per model — `CMachineInfo::ArePWMChannelsSwapped()` (`machineinfo.h:126–127`) is fed into the base ctor as `bSwapChannels` (`lib/sound/pwmsoundbasedevice.cpp:106`).
* `bMSMode` selects the M/S (mark/space) PWM mode and **defaults to `TRUE`**; README release notes (`README.md:73`): *"The class `CPWMSoundBaseDevice` supports the M/S PWM mode, which is also the default now. This should lead to a better audio quality and suppresses hum, when the PWM signal is delivered via the GPIO header."*
* DMA + IRQ driven: `CDMASoundBuffers m_DMABuffers` (`pwmsoundbasedevice.h:99`) with `ChunkCompletedHandler` (`pwmsoundbasedevice.h:85–86`).
* Registers itself as device `"sndpwm"` (`lib/sound/pwmsoundbasedevice.cpp:130`).
* **Model caveats:** Pi Zero/Zero 2 W have no 3.5 mm jack — PWM audio must be taken from GPIO12/13 (or 18/19) and needs an external RC/filter circuit; `sysconfig.h:118–123` warns *"Some external circuit is needed to use this"* and *"WARNING: Do not feed voltage into these GPIO pins with this option defined on a RPi Zero, because this may destroy the pins."* Pi 5 likewise has no jack — the README feature table says `PWM sound output (on headphone jack) | with adapter` (`README.md:124`); `sample/34-sounddevices/README:6` says *"PWM sound device via the 3.5mm headphone jack (with adapter on Raspberry Pi 5)"*.

### 5.4 `CI2SSoundBaseDevice` (I2S, external DAC)

Same public signature on all models; internal RP1 variant for `RASPPI >= 5` (`include/circle/sound/i2ssoundbasedevice.h:23–25`):

```cpp
CI2SSoundBaseDevice (CInterruptSystem *pInterrupt,
                     unsigned   nSampleRate = 192000,
                     unsigned   nChunkSize  = 8192,
                     bool       bSlave      = FALSE,
                     CI2CMaster *pI2CMaster = 0,
                     u8         ucI2CAddress = 0,
                     TDeviceMode DeviceMode = DeviceModeTXOnly,
                     unsigned   nHWChannels = 2);
enum TDeviceMode { DeviceModeTXOnly, DeviceModeRXOnly, DeviceModeTXRX, DeviceModeUnknown };
```
(`i2ssoundbasedevice.h:40–65`; Pi 5 variant `i2ssoundbasedevice-rp1.h:42–58` — same, but `nChunkSize` is *"ignored in IRQ mode"*.)

* Hard evidence on rate limits (`lib/sound/i2ssoundbasedevice.cpp:111–133`):
  ```
  111  assert (m_nChunkSize >= 32);
  112  assert ((m_nChunkSize & 1) == 0);
  113  assert (nHWChannels == 2);
  ...
  121  assert (8000 <= nSampleRate && nSampleRate <= 192000);
  122  assert (nClockFreq % (CHANLEN*CHANS) == 0);
  123  unsigned nDivI = nClockFreq / (CHANLEN*CHANS) / nSampleRate;
  ```
  So **8000 Hz … 192000 Hz**, derived from the PLLD GPIO clock source (`m_Clock (GPIOClockPCM, GPIOClockSourcePLLD)`, `i2ssoundbasedevice.cpp:104`) with an integer+fractional divider (MASH 1 when `nDivF > 0`, `:133`). `nHWChannels` must be `2`; the Pi 5 variant accepts `2 or 8` per its header comment (`i2ssoundbasedevice-rp1.h:58`) with 8-channel TX pins (`m_PCMDOUTPin1..3`, `i2ssoundbasedevice-rp1.h:115–117`).
* HW format is `SoundFormatSigned24_32` (`i2ssoundbasedevice.cpp:97`, `i2ssoundbasedevice-rp1.cpp:183`); range `±(2^23-1)` (`i2ssoundbasedevice.cpp:152–160`).
* DMA + IRQ driven; Pi 5 uses `CDMAChannelRP1` with 2×2 ping-pong buffers (`i2ssoundbasedevice-rp1.h:130–135`) or an IRQ path if `USE_I2S_SOUND_IRQ` is enabled (`i2ssoundbasedevice-rp1.h:27`, currently commented out).
* I2C DAC initialisation is optional: pass `pI2CMaster` (and optionally `ucI2CAddress`; `0` = auto-probe `0x4C`/`0x4D` per `i2ssoundbasedevice.h:54–55`, plus WM8960 at `0x1A` per `sample/34-sounddevices/README:46–49`). Supported controllers in-tree: `CPCM512xSoundController`, `CWM8960SoundController` (`lib/sound/Makefile:28`; `include/circle/sound/pcm512xsoundcontroller.h`, `wm8960soundcontroller.h`).
* Registers as device `"sndi2s"` (`lib/sound/i2ssoundbasedevice.cpp:138`).

### 5.5 `CHDMISoundBaseDevice` (HDMI audio without VCHIQ)

```cpp
CHDMISoundBaseDevice (CInterruptSystem *pInterrupt,     // DMA mode
                      unsigned nSampleRate = 48000,
                      unsigned nChunkSize  = 384 * 10);

CHDMISoundBaseDevice (unsigned nSampleRate = 48000);    // polling mode
```
(`include/circle/sound/hdmisoundbasedevice.h:51–57`.)

* Header note: *"This driver does not support HDMI1 on Raspberry Pi 4 (HDMI0 only)."* (`hdmisoundbasedevice.h:40`).
* Two extra polling-mode methods: `boolean IsWritable (void)` and `void WriteSample (s32 nSample)` — *"Must be called twice for each frame (left/right sample)"* (`hdmisoundbasedevice.h:72–82`).
* HW format `SoundFormatIEC958` (`lib/sound/hdmisoundbasedevice.cpp:127`), so in DMA mode your `GetChunk()` **must** pass every sample through `ConvertIEC958Sample (nSample, nFrame)` (`hdmisoundbasedevice.h:90–94`).
* `nChunkSize` must be a multiple of 384 (= `IEC958_SUBFRAMES_PER_BLOCK`, `soundbasedevice.h:37–38`): `assert (m_nChunkSize % IEC958_SUBFRAMES_PER_BLOCK == 0)` (`lib/sound/hdmisoundbasedevice.cpp:141`).
* The audio clock is derived from the **pixel clock**: `rational_best_approximation (m_ulAudioClockRate, m_nSampleRate, …)` and `nCTS` from `m_ulPixelClockRate` (`lib/sound/hdmisoundbasedevice.cpp:368–392`). Because of this the rate must be one the HDMI stream can carry; `SampleRateToHWFormat()` recognises exactly (`.cpp:565–581`): `8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000` — anything else yields `MaiFormatSampleRateNotIndicated`.
* Registers as device `"sndhdmi"` (`lib/sound/hdmisoundbasedevice.cpp:121, 143`).
* **Pi 5 status is contradictory in this tree.** The driver builds for `RASPPI=5` (`lib/sound/Makefile:27`) and contains real Pi 5 code paths, including BCM2712 D0-stepping-specific DMA DREQ and MAI threshold values:
  ```
  lib/sound/hdmisoundbasedevice.cpp:265–270   if (GetSoCStepping() >= SoCSteppingD0) DREQ = DREQSourceHDMI_D0;
  lib/sound/hdmisoundbasedevice.cpp:417–425   D0-specific MaiThreshold panic/DREQ values
  ```
  but the README feature table marks HDMI sound as supported on Pi 5 (`README.md:131`), while `sample/34-sounddevices/README:8–9` says *"HDMI sound device via sound-capable HDMI monitor (without VCHIQ, HDMI0 only, **not on Raspberry Pi 5**)"*. Treat Pi 5 HDMI audio as **unverified** and test it (§10.8).
* Known link problem: `doc/issues.txt:33–34` — *"The class CHDMISoundBaseDevice will not link with STDLIB_SUPPORT = 0 and AARCH = 32."*

### 5.6 `CUSBSoundBaseDevice` (USB audio class)

```cpp
CUSBSoundBaseDevice (unsigned nSampleRate = 48000,
                     TDeviceMode DeviceMode = DeviceModeTXOnly,
                     unsigned nDevice = 0);
enum TDeviceMode { DeviceModeTXOnly, DeviceModeRXOnly, DeviceModeTXRX, DeviceModeUnknown };
```
(`include/circle/sound/usbsoundbasedevice.h:32–47`.)

* **Only compiled for `RASPPI ∈ {4, 5}`** (`lib/sound/Makefile:36–38`), consistent with `sample/34-sounddevices/README:12` (*"for Raspberry Pi 4 and 5 only"*) and `README.md:150` (which still says *"RPi 4 only"* and is stale). `sample/34-sounddevices/kernel.cpp:155–160` guards the instantiation with `#if RASPPI >= 4`.
* Registered interfaces are enumerated from `CDeviceNameService` by `GetStreamingDevice (bTX, nIndex)` (`usbsoundbasedevice.h:81–82`); `nDevice` selects the 0-based index.
* HW format `SoundFormatSigned16` (default) or 24-bit per `cmdline.txt` `soundopt=` (`doc/cmdline.txt:60–63`), and the channel counts come from `cmdline.txt` `usbsoundchannels=<out>,<in>` (default `0,0` = first found; `doc/cmdline.txt:49–52`).
* **Completion-driven, not queue-driven.** The device pulls data from your `GetChunk`:
  ```
  lib/sound/usbsoundbasedevice.cpp:330   boolean CUSBSoundBaseDevice::SendChunk (void)
  lib/sound/usbsoundbasedevice.cpp:343       nChunkSize = GetChunk ((s16 *) pBuf, nChunkSize);   // 16-bit path
  lib/sound/usbsoundbasedevice.cpp:349       nChunkSize = GetChunk ((u32 *) pBuf, nChunkSize);   // 24-bit path
  lib/sound/usbsoundbasedevice.cpp:360       m_pTXUSBDevice->SendChunk (pBuf, nChunkSize * m_nSubframeSize, …)
  lib/sound/usbsoundbasedevice.cpp:398   void CUSBSoundBaseDevice::TXCompletionRoutine (unsigned nBytesTransferred)
  lib/sound/usbsoundbasedevice.cpp:435       if (!SendChunk ()) …
  ```
  `Start()` issues two `SendChunk()` calls synchronously (`:234, :236`); every subsequent chunk is requested from the USB completion routine. So the first two callbacks are in task context and the rest are in IRQ context — your `GetChunk` must therefore be safe to call from both.
* **Restriction** (`sample/34-sounddevices/README:51–53`): *"The USB sound card must be connected before the sample program is started and must not be removed, while the program is running."*
* Optional `CSoundController` via `GetController()` (`usbsoundbasedevice.h:61–62`, `usbsoundcontroller.h`).

### 5.7 Recommended device for Pi Zero 2 W → Pi 5

**Recommendation: `CPWMSoundBaseDevice` as the single portable default, with `CI2SSoundBaseDevice` as the high-quality option.**

| Device | Zero 2 W | Pi 3 | Pi 4 | Pi 5 | Needs external hardware | API used |
|---|---|---|---|---|---|---|
| `CPWMSoundBaseDevice` | yes (GPIO12/13 + RC filter — no jack) | yes (jack) | yes (jack) | yes (`with adapter`, `README.md:124`) | Zero/Zero 2 W: RC filter + jack/amp; Pi 5: adapter/header | `AllocateQueue` + `Write` |
| `CI2SSoundBaseDevice` | yes | yes | yes | yes | I2S DAC (PCM5102A/PCM5122/WM8960) | `AllocateQueue` + `Write` |
| `CHDMISoundBaseDevice` | yes (HDMI0) | yes | yes (HDMI0 only) | **unverified** (§5.5) | sound-capable HDMI sink | `GetChunk` + `ConvertIEC958Sample` |
| `CUSBSoundBaseDevice` | **no** | **no** | yes | yes | USB audio class device | `GetChunk` |

Justification: PWM is the only device whose class exists for every one of the four targets with the *same* constructor and the *same* sample-feeding API (`AllocateQueue`/`SetWriteFormat`/`Write`), which means one binary-portable audio path. It is also fully interrupt/DMA driven, so the emulator never has to be re-entrant into the audio path. Its weakness on Zero/Zero 2 W is that it needs a small analogue filter, and its rate accuracy is quantised by the PWM clock divider (`250/125/50 MHz ÷ integer`).

**Is HDMI audio viable on all four?** No, not demonstrably: it requires a sound-capable HDMI sink (a monitor with speakers or an audio extractor) on every target, and on Pi 5 the in-tree documentation contradicts itself (§5.5). It is also restricted to HDMI0 on Pi 4 and its chunk size must be a multiple of 384 with IEC958 framing applied by hand.

**Minimal working example of the recommended device** (API-verified against `sample/34-sounddevices/kernel.cpp:142–238` and `lib/sound/soundbasedevice.cpp`):

```cpp
#include <circle/sound/pwmsoundbasedevice.h>   // pulls in the -rp1 variant on RASPPI >= 5
#include <circle/interrupt.h>

static const unsigned SAMPLE_RATE     = 48000;
static const unsigned CHUNK_SIZE      = 2048;   // words per DMA chunk (even, > 0)
static const unsigned QUEUE_SIZE_MSECS= 100;

class CAudio
{
public:
	CAudio (CInterruptSystem *pInterrupt)
	:	m_Sound (pInterrupt, SAMPLE_RATE, CHUNK_SIZE)	// bMSMode defaults to TRUE
	{
		m_Sound.AllocateQueue (QUEUE_SIZE_MSECS);	// 100 ms ring buffer
		m_Sound.SetWriteFormat (SoundFormatSigned16, 2);	// interleaved L,R int16
	}

	boolean Start (void) { return m_Sound.Start (); }
	boolean IsActive (void) const { return m_Sound.IsActive (); }

	// Feed whatever fits; returns frames accepted (Write() never blocks)
	unsigned Feed (const s16 *pFrames, unsigned nFrames)
	{
		int nBytes = m_Sound.Write (pFrames, nFrames * 2 * sizeof (s16));
		if (nBytes <= 0) return 0;
		return (unsigned) nBytes / (2 * sizeof (s16));
	}

	// How many frames can be queued right now (call from TASK_LEVEL)
	unsigned FramesFree (void)
	{
		return m_Sound.GetQueueSizeFrames () - m_Sound.GetQueueFramesAvail ();
	}

private:
	CPWMSoundBaseDevice m_Sound;
};
```

Loop usage (mirrors `sample/34-sounddevices/kernel.cpp:189–210`):

```cpp
// initial fill before Start()
//   n = m_Sound.GetQueueSizeFrames ();  render n frames;  Feed (...);  Start ();

// once per emulated frame (50 Hz PAL, 60 Hz NTSC):
//   unsigned nFree = FramesFree ();
//   render nFree frames into pBuffer (POKEY at SAMPLE_RATE)
//   Feed (pBuffer, nFree);
```

Sample-format note: because the emulator's POKEY output is naturally `int16_t`, `SoundFormatSigned16` avoids any conversion; `Write()` then takes the fast path (no bit-depth conversion, no channel swap) as long as `m_bSwapChannels == FALSE` — which is *not* guaranteed on PWM, because `CMachineInfo::ArePWMChannelsSwapped()` is passed as `bSwapChannels` for PWM only (`lib/sound/pwmsoundbasedevice.cpp:104–106`). Check `m_Sound.AreChannelsSwapped()` (`soundbasedevice.h:168–169`) if left/right come out mirrored; I2S/HDMI/USB pass `FALSE`.

---

## 6. USB keyboard / input / mass storage

### 6.1 Which controller, per model — and does USB work on Pi 5?

`include/circle/usb/usbhcidevice.h:26–35` — the whole story:

```cpp
#if RASPPI <= 3
	#include <circle/usb/dwhcidevice.h>
	#define CUSBHCIDevice	CDWHCIDevice
#elif RASPPI == 4
	#include <circle/usb/xhcidevice.h>
	#define CUSBHCIDevice	CXHCIDevice
#else
	#include <circle/usb/usbsubsystem.h>
	#define CUSBHCIDevice	CUSBSubSystem
#endif
```

Constructors:
```cpp
CDWHCIDevice  (CInterruptSystem *pInterruptSystem, CTimer *pTimer,
               boolean bPlugAndPlay = FALSE);                       // dwhcidevice.h:45-46
CXHCIDevice   (CInterruptSystem *pInterruptSystem, CTimer *pTimer,
               boolean bPlugAndPlay = FALSE, unsigned nDevice = 0,
               CXHCISharedMemAllocator *pSharedMemAllocator = 0);   // xhcidevice.h:41-43
CUSBSubSystem (CInterruptSystem *pInterruptSystem, CTimer *pTimer,
               boolean bPlugAndPlay = FALSE);                       // usbsubsystem.h:36-37
```
All three expose `boolean Initialize (boolean bScanDevices = TRUE)` (`dwhcidevice.h:49`, `xhcidevice.h:46`, `usbsubsystem.h:40`) and inherit `boolean UpdatePlugAndPlay (void)` (`usbcontroller.h:36`, `usbhostcontroller.h:77–79`: *"must be called from TASK_LEVEL, if Plug-and-Play is enabled; returns TRUE if device tree might have been updated (always TRUE on first call)"*).

**Yes, USB works on Pi 5.** `CUSBSubSystem` drives **two** `CXHCIDevice` instances over PCIe:
```
include/circle/usb/usbsubsystem.h:61   static const unsigned NumDevices = 2;
include/circle/usb/usbsubsystem.h:70   CXHCIDevice *m_pXHCIDevice[NumDevices];
```
and `README.md:142–144` marks "Host controller interface (HCI) drivers", "Standard hub driver (USB 2.0 only)" and "HID class device drivers (keyboard, mouse, gamepad)" with `x` for Pi 5. `CUSBSubSystem::IsActive()` / `::Get()` static helpers exist (`usbsubsystem.h:52–54`).

Pi-4-specific: the external xHCI (the four USB-A sockets) is the default; `USE_XHCI_INTERNAL` switches to the internal controller used by the USB-C power socket, and is *"the only possibility to use USB on the Compute Module 4 with Circle"*, requiring `otg_mode=1` in `config.txt` (`sysconfig.h:176–191`).

### 6.2 Initialisation for Zero 2 W and for Pi 4/5

Identical source, because of the alias. Declare the member and construct it exactly as in `sample/05-usbsimple/kernel.h:63` / `kernel.cpp:31`:

```cpp
CUSBHCIDevice m_USBHCI;                      // in kernel.h
// ...
m_USBHCI (&m_Interrupt, &m_Timer)            // ctor, plug-and-play off
m_USBHCI (&m_Interrupt, &m_Timer, TRUE)      // ctor, plug-and-play on
// ...
bOK = m_USBHCI.Initialize ();                // in CKernel::Initialize(), after Interrupt and Timer
```

* There is **no `USBHispeed` parameter**. The only speed control is the `cmdline.txt` option `usbspeed=full` (`doc/cmdline.txt:29–31`), read via `CKernelOptions::GetUSBFullSpeed()` and honoured only in the DWHCI driver (`lib/usb/dwhcidevice.cpp:499, 654`) — *"does work on Raspberry Pi 1-3 and Zero only"*.
* Plug-and-play (`bPlugAndPlay = TRUE`) additionally requires `USE_USB_SOF_INTR` (default on via `sysconfig.h:225–227`: *"For USB plug-and-play operation this option must be set in any case"*) and, in `Run()`, a per-iteration `m_USBHCI.UpdatePlugAndPlay ()` call from `TASK_LEVEL` (`sample/08-usbkeyboard/kernel.cpp:147–166`).
* `cmdline.txt` also has `usbpowerdelay=<ms>` (default 510) and `usbignore=<name>` (`doc/cmdline.txt:25–47`); `usbignore` is compared against the device/interface name in `CUSBDeviceFactory::GetDevice` (`lib/usb/usbdevicefactory.cpp:64–72`).
* Boot-time caution (`doc/issues.txt:41–44`): *"If there are problems to detect a specific USB device after power-on or attach, it may help to increase the USB power-on delay by setting the option usbpowerdelay= in the file cmdline.txt to 800 (ms) or higher."*
* Performance caution (`doc/multicore.txt:55–60`): *"the USB frame scheduler for interrupt transfers in Circle is very simple. If you generate heavy USB bulk traffic … this may cause problems with devices using interrupt transfers (e.g. keyboard, mouse) which will respond very slowly … continuously executing a short delay in your program flow from time to time."* Heavy USB mass-storage reads on the same bus as the keyboard are exactly that scenario.

### 6.3 `CUSBKeyboardDevice` — registration and handler signatures

Device name is `"ukbd<N>"` (added as `DevicePrefix "ukbd"` + index, non-block device — `lib/usb/usbkeyboard.cpp:91`, and samples fetch `"ukbd1"`, `sample/08-usbkeyboard/kernel.cpp:153`).

The two handler modes (`include/circle/usb/usbkeyboard.h:31–60`), verbatim:

```cpp
// The raw handler is called when the keyboard sends a status report (on status change and/or continously).
typedef void TKeyStatusHandlerRaw (unsigned char	ucModifiers,	// see usbhid.h
				   const unsigned char	RawKeys[6]);	// key code or 0 in each byte
typedef void TKeyStatusHandlerRawEx (unsigned char	ucModifiers,	// see usbhid.h
				   const unsigned char	RawKeys[6], void* arg);	// key code or 0 in each byte
...
	// cooked mode
	void RegisterKeyPressedHandler (TKeyPressedHandler *pKeyPressedHandler);
	void RegisterSelectConsoleHandler (TSelectConsoleHandler *pSelectConsoleHandler);
	void RegisterShutdownHandler (TShutdownHandler *pShutdownHandler);

	// raw mode (if bMixedMode is FALSE, the cooked handlers are ignored)
	void RegisterKeyStatusHandlerRaw (TKeyStatusHandlerRawEx *pKeyStatusHandlerRaw,
					  boolean bMixedMode, void* pArg);
	void RegisterKeyStatusHandlerRaw (TKeyStatusHandlerRaw *pKeyStatusHandlerRaw,
					  boolean bMixedMode = FALSE);
	void UnregisterKeyStatusHandlerRaw (void);

	// works in cooked and raw mode
	virtual boolean SetLEDs (u8 ucStatus);		// must not be called in interrupt context
```

Cooked-mode typedefs (`include/circle/input/keyboardbehaviour.h:53–55`):
```cpp
typedef void TKeyPressedHandler (const char *pString);
typedef void TSelectConsoleHandler (unsigned nConsole);
typedef void TShutdownHandler (void);
```

**Raw HID usage codes are available** via `RegisterKeyStatusHandlerRaw` — the sample's raw handler receives exactly the boot-protocol report (`sample/08-usbkeyboard/kernel.cpp:205–224`):

```cpp
void CKernel::KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	assert (s_pThis != 0);

	CString Message;
	Message.Format ("Key status (modifiers %02X)", (unsigned) ucModifiers);

	for (unsigned i = 0; i < 6; i++)
	{
		if (RawKeys[i] != 0)
		{
			CString KeyCode;
			KeyCode.Format (" %02X", (unsigned) RawKeys[i]);

			Message.Append (KeyCode);
		}
	}

	s_pThis->m_Logger.Write (FromKernel, LogNotice, Message);
}
```

Modifier bit definitions (`include/circle/usb/usbhid.h:56–63`): `LCTRL (1<<0)`, `LSHIFT (1<<1)`, `ALT (1<<2)`, `LWIN (1<<3)`, `RCTRL (1<<4)`, `RSHIFT (1<<5)`, `ALTGR (1<<6)`, `RWIN (1<<7)` — same bit values as `KEY_*_MASK` in `keyboardbehaviour.h:40–47`. LED status bits `LED_NUM_LOCK/CAPS_LOCK/SCROLL_LOCK` (`usbhid.h:66–68`).

**For an Atari emulator, always use raw mode.** Cooked mode goes through `CKeyboardBehaviour` + `CKeyMap` (`usbkeyboard.h:77`, `keyboardbehaviour.h:57–89`) and translates to ASCII/escape sequences (`doc/keyboard.txt:1–43`) with the layout selected by `DEFAULT_KEYMAP` / `cmdline.txt keymap=` (`sysconfig.h:298–312`, `doc/cmdline.txt:20–23`). The raw handler is called *"on status change and/or continously"* (`usbkeyboard.h:31`) which is exactly the key-matrix semantics a 6502 emulator wants.

Important operational details:
* The handler runs in **IRQ context** (USB completion). Do not call `SetLEDs()`/`UpdateLEDs()`/`Write()`-to-screen from it. `sample/08-usbkeyboard/kernel.cpp:168–173` shows the correct place: call `m_pKeyboard->UpdateLEDs ()` from the main `Run()` loop with the comment *"CUSBKeyboardDevice::UpdateLEDs() must not be called in interrupt context … This does nothing in raw mode."* The release notes for 51.1 note the same restriction for `SetLEDs` (`usbkeyboard.h:63`).
* Register a removal handler so you do not use a dangling pointer:
  ```cpp
  m_pKeyboard->RegisterRemovedHandler (KeyboardRemovedHandler);   // sample/08-usbkeyboard/kernel.cpp:156
  ```
  with `void KeyboardRemovedHandler (CDevice *pDevice, void *pContext)` (`sample/08-usbkeyboard/kernel.cpp:226–232`; typedef `TDeviceRemovedHandler` at `include/circle/device.h:29`).
* 8BitDo keyboards are routed to `CUSBKeyboard8BitDoDevice` by the factory (`lib/usb/usbdevicefactory.cpp:98–102`; `include/circle/usb/usbkeyboard8bitdo.h`).
* `#define EXCLUDE_USB_KEYB` removes the driver from the factory at compile time (`lib/usb/usbdevicefactory.cpp:92`).

### 6.4 USB gamepads

`include/circle/usb/usbgamepad.h:170–227` — `class CUSBGamePadDevice : public CUSBHIDDevice`:

```cpp
boolean Configure (void);
virtual unsigned GetProperties (void);                       // TGamePadProperty bit mask
virtual const TGamePadState *GetInitialState (void);
const TGamePadState *GetReport (void);                       // deprecated alias
virtual void RegisterStatusHandler (TGamePadStatusHandler *pStatusHandler);
virtual boolean SetLEDMode  (TGamePadLEDMode Mode);
virtual boolean SetLEDMode  (u32 nRGB, u8 uchTimeOn, u8 uchTimeOff);
virtual boolean SetRumbleMode (TGamePadRumbleMode Mode);
```

Callback signature (`usbgamepad.h:168`):
```cpp
typedef void TGamePadStatusHandler (unsigned nDeviceIndex, const TGamePadState *pGamePadState);
```

State (`usbgamepad.h:144–163`) contains axes **and** hats **and** buttons **and** sensors:
```cpp
struct TGamePadState
{
	int naxes;                       // number of available axes/analog buttons
	struct { int value; int minimum; int maximum; } axes[MAX_AXIS];   // MAX_AXIS = 16
	int nhats;  int hats[MAX_HATS];  // MAX_HATS = 6
	int nbuttons;  unsigned buttons; // digital buttons, bit mask
	int acceleration[3];
	int gyroscope[3];
};
```

Button bit masks and aliases are in `usbgamepad.h:44–87` (`GamePadButtonA`/`Cross`, `GamePadButtonB`/`Circle`, `GamePadButtonX`/`Square`, `GamePadButtonY`/`Triangle`, `GamePadButtonUp/Right/Down/Left`, `GamePadButtonStart`/`Options`, `GamePadButtonSelect`/`Back`/`Share`, `GamePadButtonGuide`/`Xbox`/`PS`/`Home`, `GamePadButtonLB/LT/RB/RT`, `GamePadButtonL3/R3`, `GamePadButtonPlus/Minus/Touchpad`). Axis enum `TGamePadAxis` at `usbgamepad.h:94–115`. Default axis range constants at `usbgamepad.h:165–166`.

Both styles work: you can call `RegisterStatusHandler()` (IRQ context) **or** poll `GetInitialState()`/`GetReport()` from your frame loop. `sample/27-usbgamepad` and `sample/37-showgamepad` demonstrate the polling style (`sample/README:28,38` lists `37-showgamepad` as *"Shows a stylised gamepad on screen and the state of an attached USB gamepad"*).

Available concrete drivers (`include/circle/usb/`, dispatched from `lib/usb/usbdevicefactory.cpp:140–230`):
`CUSBGamePadPS3Device`, `CUSBGamePadPS4Device`, `CUSBGamePadXbox360Device`, `CUSBGamePadXbox360PCWirelessDevice`, `CUSBGamePadXboxOneDevice`, `CUSBGamePadSwitchProDevice`, `CUSBGamePad8BitDoProDevice`, `CUSBGamePad8BitDoXInputDevice`, `CUSBGamePadStandardDevice` (`usbgamepadstandard.h`, the generic HID fallback). `#define EXCLUDE_USB_GAMEPAD` removes them all (`lib/usb/usbdevicefactory.cpp:140`).

### 6.5 USB mass storage — class, mounting, and read/write API

**The class** is `CUSBBulkOnlyMassStorageDevice` (`include/circle/usb/usbmassdevice.h:35–71`) — note the name; there is no `CUSBMassStorageDevice`.

```cpp
CUSBBulkOnlyMassStorageDevice (CUSBFunction *pFunction);   // usbmassdevice.h:38
boolean Configure (void);
int  Read  (void *pBuffer, size_t nCount);                 // usbmassdevice.h:43
int  Write (const void *pBuffer, size_t nCount);           // usbmassdevice.h:44
u64  Seek  (u64 ullOffset);
u64  GetSize (void) const;        // bytes
unsigned GetCapacity (void) const;// blocks
```
There is **no `nMaxLUN` / `bMount` / `bNoAutoMount` parameter** in this revision. The object is created by the USB device factory from the interface name `"int8-6-50"` (class 8 / subclass 6 / protocol 50 = SCSI transparent), `lib/usb/usbdevicefactory.cpp:81–85`:

```cpp
#ifndef EXCLUDE_USB_STORAGE
	else if (pName->Compare ("int8-6-50") == 0)
	{
		pResult = new CUSBBulkOnlyMassStorageDevice (pParent);
	}
#endif
```

`Configure()` builds a `CPartitionManager` and registers the names (`lib/usb/usbmassdevice.cpp:395–422`):

```cpp
CString DeviceName;
DeviceName.Format ("umsd%u", m_nDeviceNumber);        // "umsd1", "umsd2", ...
m_pPartitionManager = new CPartitionManager (this, DeviceName);
m_pPartitionManager->Initialize ();
CDeviceNameService::Get ()->AddDevice (DeviceName, this, TRUE);   // TRUE = block device
```

`CPartitionManager::Initialize()` parses the MBR and registers each *supported* partition as `"<devname>-<n>"` (`lib/fs/partitionmanager.cpp:107–128`):

```cpp
PartitionName.Format ("%s-%u", (const char *) m_DeviceName, ++nPartition);
CDeviceNameService::Get ()->AddDevice (PartitionName, m_pPartition[i], TRUE);
```
Note `-1` is the **first valid** partition, not MBR index 0; and extended (`0x05`/`0x0F`) and EFI (`0xEF`) partitions are skipped (`partitionmanager.cpp:110–118`). A non-MBR drive logs *"Drive has no MBR"* and still returns `TRUE` (`partitionmanager.cpp:100–105`).

**So on USB:**
* whole device (raw LBA access / FatFs auto-scan) → `"umsd1"`
* first FAT partition (Circle's own FAT driver) → `"umsd1-1"`
* second/third stick → `"umsd2"`, `"umsd3"` … (matching `addon/fatfs/diskio.cpp:28–37`)

`sample/07-usbstorage/kernel.cpp:115–137` fetches and uses the whole device:

```cpp
CDevice *pUMSD1 = m_DeviceNameService.GetDevice ("umsd1", TRUE);
if (pUMSD1 == 0) { /* not found */ }

u64 ullOffset = 0 * UMSD_BLOCK_SIZE;             // UMSD_BLOCK_SIZE = 512
if (pUMSD1->Seek (ullOffset) != ullOffset) { /* seek error */ }

TMasterBootRecord MBR;
if (pUMSD1->Read (&MBR, sizeof MBR) != (int) sizeof MBR) { /* read error */ }
```

`CDevice::Read/Write/Seek/GetSize/IOCtl` are the generic block-device API (`include/circle/device.h:53–73`), and `#define DEVICE_IOCTL_SYNC 0x10001U` flushes pending writes (`device.h:73`).

USB floppy drives are a separate class, `CUSBFloppyDiskDevice`, device prefix `"ufd"` (`lib/usb/usbfloppydevice.cpp:191–193, 379`), for interface names `"int8-4-0"`/`"int8-4-1"` (`lib/usb/usbdevicefactory.cpp:86–90`). It is *"experimental"* (`README.md:149`).

---

## 7. Storage / filesystem

Circle offers **two** FAT implementations plus the SD/USB block drivers:

| Layer | Class / API | Location | Write? | Best for |
|---|---|---|---|---|
| Internal FAT driver | `CFATFileSystem` | `include/circle/fs/fat/fatfs.h`, `lib/fs/fat/` | **Yes** (full read *and* write API) | tiny settings files; 8.3 names only |
| FatFs (ChaN, R0.16) | `f_open`/`f_read`/`f_write`… | `addon/fatfs/` | **Yes** (`FF_FS_READONLY = 0`) | everything else: LFN, subdirs, image files |
| SD/MMC block driver | `CEMMCDevice`, `CSDHOSTDevice` | `addon/SDCard/` | n/a (block device) | registers `"emmc1"` / `"emmc1-1"` |
| Partition layer | `CPartitionManager`, `CPartition` | `lib/fs/partition.cpp`, `partitionmanager.cpp` | n/a | MBR parsing → `"<dev>-<n>"` |

`README.md:159–160` in the feature table:
> Internal FAT driver (limited function) — x
> FatFs driver (full function, by ChaN) — x

and `addon/fatfs/sample/README:15–18`: *"This sample uses the FatFs generic FAT file system module … FatFs supports long file names (LFN), subdirectories, the full set of features of the FAT file system and has a better performance than the Circle FAT file system library."*

### 7.1 `CFATFileSystem` — the internal driver

`include/circle/fs/fat/fatfs.h:47–165`. Mount and I/O API (all exact):

```cpp
int Mount (CDevice *pPartition);              // fatfs.h:59   non-zero on success
void UnMount (void);                          // fatfs.h:67
void Synchronize (void);                      // fatfs.h:75

unsigned RootFindFirst (TDirentry *pEntry, TFindCurrentEntry *pCurrentEntry);  // :85
unsigned RootFindNext  (TDirentry *pEntry, TFindCurrentEntry *pCurrentEntry);  // :95

unsigned FileOpen   (const char *pTitle);                    // :104  read-only
unsigned FileCreate (const char *pTitle);                    // :113  truncates if exists
unsigned FileClose  (unsigned hFile);                        // :122
unsigned FileRead   (unsigned hFile, void *pBuffer, unsigned nCount);          // :134
unsigned FileWrite  (unsigned hFile, const void *pBuffer, unsigned nCount);    // :145
int      FileDelete (const char *pTitle);                    // :155
```

Limitations, provable from the tree:
* **8.3 file names only.** `FS_TITLE_LEN` is `12` (`include/circle/fs/fsdef.h:27`) and the directory entry stores `FAT_DIR_NAME_LENGTH 11` bytes (`include/circle/fs/fat/fatfsdef.h:104`). No long file names.
* **Root directory only** — the only enumeration entry points are `RootFindFirst`/`RootFindNext`; there is no subdirectory API (`fatfs.h:85,95`).
* **Fixed file table:** `FAT_FILES 40`, cache `FAT_BUFFERS 100` (`fatfsdef.h:12–13`).
* Max file size `0xFFFFFFFF` (`fatfsdef.h:15`).
* Error sentinel `FS_ERROR 0xFFFFFFFF` (`fsdef.h:46`); directory entry struct `TDirentry { char chTitle[13]; unsigned nSize; unsigned nAttributes; }` with `FS_ATTRIB_SYSTEM`/`FS_ATTRIB_EXECUTABLE` (`fsdef.h:29–38`).
* It **can write** — `FileCreate` + `FileWrite` + `FileClose`, and `Synchronize()` flushes the buffer cache.

Canonical usage, `sample/15-files/kernel.cpp:88–170` (on a USB stick, partition `"umsd1-1"`, `FILENAME "circle.txt"`):

```cpp
// mount
CDevice *pPartition = m_DeviceNameService.GetDevice ("umsd1-1", TRUE);
if (pPartition == 0) m_Logger.Write (FromKernel, LogPanic, "Partition not found");
if (!m_FileSystem.Mount (pPartition)) m_Logger.Write (FromKernel, LogPanic, "Cannot mount partition");

// enumerate the root directory
TDirentry Direntry;
TFindCurrentEntry CurrentEntry;
unsigned nEntry = m_FileSystem.RootFindFirst (&Direntry, &CurrentEntry);
for (unsigned i = 0; nEntry != 0; i++)
{
	if (!(Direntry.nAttributes & FS_ATTRIB_SYSTEM)) { /* use Direntry.chTitle, Direntry.nSize */ }
	nEntry = m_FileSystem.RootFindNext (&Direntry, &CurrentEntry);
}

// create + write
unsigned hFile = m_FileSystem.FileCreate ("circle.txt");
if (m_FileSystem.FileWrite (hFile, Msg, Msg.GetLength ()) != Msg.GetLength ()) { /* error */ }
m_FileSystem.FileClose (hFile);

// open + read
hFile = m_FileSystem.FileOpen ("circle.txt");
char Buffer[100];
unsigned nResult;
while ((nResult = m_FileSystem.FileRead (hFile, Buffer, sizeof Buffer)) > 0)
{
	if (nResult == FS_ERROR) { /* error */ break; }
	/* Buffer[0..nResult-1] */
}
m_FileSystem.FileClose (hFile);
```
(`m_FileSystem` is declared as `CFATFileSystem m_FileSystem;` — `sample/15-files/kernel.h`.)

Note the loop bug-compatibility detail: `while ((nResult = FileRead (...)) > 0)` never enters the `FS_ERROR` branch, because `FS_ERROR == 0xFFFFFFFF` is returned as `unsigned` and compares `> 0`. To detect an error you must capture the result as `unsigned` and compare explicitly with `FS_ERROR`. The sample's loop as written treats a hard read failure as end-of-file.

`CFATFileSystem` needs the partition device, so it needs `CEMMCDevice` or the USB MSD to have registered `"emmc1-1"` / `"umsd1-1"`. It is used by `sample/15-files` and `addon/SDCard/sample`.

### 7.2 The FatFs addon (`addon/fatfs`) — recommended

Layout:
```
addon/fatfs/Makefile        -> libfatfs.a   (OBJS = ff.o diskio.o ffsystem.o ffunicode.o)
addon/fatfs/ff.c, ff.h, ffconf.h, ffunicode.c
addon/fatfs/diskio.cpp, diskio.h
addon/fatfs/ffsystem.cpp
addon/fatfs/sample/{main.cpp,kernel.cpp,kernel.h,Makefile,README}
```
Build it manually (it is under `addon/`, so `makeall` skips it — `README.md:294`, `makeall:17–58`):
```
cd addon/fatfs && make
```
`addon/fatfs/Makefile:14` includes `$(CIRCLEHOME)/Rules.mk`, so it picks up `AARCH`/`RASPPI`/`PREFIX` from your `Config.mk`.

**The Circle ⇄ FatFs glue** is `addon/fatfs/diskio.cpp`. Volume-name ↔ physical-drive mapping (`diskio.cpp:28–37`):

```cpp
static const char *s_pVolumeName[FF_VOLUMES] =
{
	"emmc1",
	"emmc2",
	"umsd1",
	"umsd2",
	"umsd3",
	"ufd1",
	"nvme1"
};
```
with `FF_VOLUMES 7` and `FF_VOLUME_STRS "SD","SD2","USB","USB2","USB3","FD","NVME"` (`addon/fatfs/ffconf.h:181–186`). So:

| FatFs drive string | Circle device looked up | Physical medium |
|---|---|---|
| `"SD:"` | `emmc1` (whole card) | SD card |
| `"SD2:"` | `emmc2` | second EMMC / SDIO breakout (`README.md:74`) |
| `"USB:"` | `umsd1` | first USB stick |
| `"USB2:"` / `"USB3:"` | `umsd2` / `umsd3` | further sticks |
| `"FD:"` | `ufd1` | USB floppy |
| `"NVME:"` | `nvme1` | NVMe (Pi 5) |

Lookup happens on demand in `disk_initialize()` (`diskio.cpp:83–101`):
```cpp
s_pVolume[pdrv] = CDeviceNameService::Get ()->GetDevice (s_pVolumeName[pdrv], TRUE);
s_pVolume[pdrv]->RegisterRemovedHandler (disk_removed, &s_pVolume[pdrv]);
```

**Can FatFs write? Yes, definitively.** `addon/fatfs/ffconf.h:11`:
```c
#define FF_FS_READONLY	0
/* This option switches read-only configuration. (0:Read/Write or 1:Read-only)
/  Read-only configuration removes writing API functions, f_write(), f_sync(), ... */
```
and `disk_write()` is implemented (`addon/fatfs/diskio.cpp:168–218`, guarded by `#if FF_FS_READONLY == 0`, with word-alignment handling via a bounce buffer). `disk_read()` at `:109–160`. `disk_ioctl()` supports `GET_SECTOR_COUNT`, `CTRL_SYNC` (→ `CDevice::IOCtl (DEVICE_IOCTL_SYNC, 0)`), `GET_SECTOR_SIZE`, `CTRL_EJECT` (`diskio.cpp:227–313`).

**Partition scanning is automatic.** `FF_MULTI_PARTITION 0` (`ffconf.h:199`) means each logical drive binds to the same physical drive number with *"Auto partition search"* (`ff.c:255–258`), and `find_volume()` (`addon/fatfs/ff.c:3410–3455`) does exactly that:
```c
3419  fmt = check_fs(fs, 0);   /* Load sector 0 and check if it is an FAT VBR as SFD format */
3420  if (fmt != 2 && (fmt >= 3 || part == 0)) return fmt;
...
3447  for (i = 0; i < 4; i++)  /* Load partition offset in the MBR */
3448      mbr_pt[i] = ld_32(fs->win + MBR_Table + i * SZ_PTE + PTE_StLba);
3451  do { fmt = mbr_pt[i] ? check_fs(fs, mbr_pt[i]) : 3; } while (part == 0 && fmt >= 2 && ++i < 4);
```
So `f_mount (&fs, "SD:", 1)` mounts the **first FAT partition found** on the whole SD card, and `f_mount (&fs, "USB:", 1)` the first on the USB stick — you do *not* need `"umsd1-1"`. (`FF_LBA64 0` / `FF_FS_EXFAT 0`, `ffconf.h:218, 246` — GPT scanning code at `ff.c:3424–3445` is compiled out, so GPT-partitioned sticks are not handled; MBR/SFD only.)

Other `ffconf.h` settings that matter (`addon/fatfs/ffconf.h`):
| Setting | Value | Line |
|---|---|---|
| `FF_FS_READONLY` | `0` (read/write) | 11 |
| `FF_FS_MINIMIZE` | `0` (all basic functions present, incl. `f_opendir`/`f_readdir`/`f_lseek`/`f_stat`/`f_unlink`/`f_mkdir`/`f_truncate`/`f_rename`) | 18 |
| `FF_USE_FIND` | `2` (`f_findfirst`/`f_findnext` with altname matching) | 28 |
| `FF_USE_MKFS` | `0` (**`f_mkfs`/`f_fdisk` unavailable**) | 33 |
| `FF_USE_CHMOD` | `1` (`f_chmod`/`f_utime`) | 45 |
| `FF_USE_STRFUNC` | `1` (`f_gets`/`f_putc`/`f_puts`/`f_printf`, no LF↔CRLF conversion) | 59 |
| `FF_CODE_PAGE` | `850` (Latin-1) | 87 |
| `FF_USE_LFN` | `3` (dynamic working buffer on the **heap**) | 116 |
| `FF_MAX_LFN` | `255` | 117 |
| `FF_LFN_UNICODE` | `0` (ANSI/OEM in current code page → `TCHAR = char`) | 136 |
| `FF_LFN_BUF`/`FF_SFN_BUF` | `255` / `12` | 148–149 |
| `FF_FS_RPATH` | `2` (relative paths + `f_getcwd`) | 156 |
| `FF_FS_EXFAT` | `0` | 246 |
| `FF_FS_NORTC` | `0` → timestamps enabled; **`get_fattime()` *is* provided** by `addon/fatfs/ffsystem.cpp:141–…` and reads `CTimer::Get()->GetLocalTime()` (returns 0 if the time was never set); fallback constants `FF_NORTC_MON/MDAY/YEAR = 1/1/2025` unused | 252–255 |
| `FF_FS_REENTRANT` | `1`, `FF_FS_TIMEOUT 1000` → locking via `ff_mutex_*` (**provided** in `ffsystem.cpp:31–103`, built on `CMutex`; `FF_FS_LOCK 0`) | 283–296 |

`FF_USE_LFN = 3` means **`f_open`/`f_readdir` may allocate from the heap** for every long-name operation — relevant to a latency-sensitive emulator (see §10). `FF_FS_REENTRANT = 1` means FatFs is thread-safe but **each call takes a mutex**, and `doc/multicore.txt:33` explicitly says the *"FAT file system (not from interrupt context)"*.

**Directory enumeration** with FatFs:
```cpp
DIR Directory;   FILINFO FileInfo;
FRESULT Result = f_findfirst (&Directory, &FileInfo, DRIVE "/", "*");   // wildcard filter
for (unsigned i = 0; Result == FR_OK && FileInfo.fname[0]; i++)
{
	// FileInfo.fname, FileInfo.fsize, FileInfo.fattrib, FileInfo.fdate/ftime
	Result = f_findnext (&Directory, &FileInfo);
}
```
(verbatim from `addon/fatfs/sample/kernel.cpp:104–123`; note `FileInfo.fname[0] == 0` is the end-of-directory sentinel). The lower-level pair is `f_opendir`/`f_readdir`/`f_closedir`.

**Complete working snippet — write and read on an SD card** (`addon/fatfs/sample/kernel.cpp:23–189`, condensed; every call verified):

```cpp
#include "ff.h"

static FATFS m_FileSystem;                       // one per volume
#define DRIVE    "SD:"                           // "USB:" for a USB stick

// ---- mount ----
if (f_mount (&m_FileSystem, DRIVE, 1) != FR_OK)
	m_Logger.Write (FromKernel, LogPanic, "Cannot mount drive: %s", DRIVE);

// ---- write a file (create or truncate) ----
FIL File;
FRESULT Result = f_open (&File, DRIVE "/circle.txt", FA_WRITE | FA_CREATE_ALWAYS);
if (Result != FR_OK) m_Logger.Write (FromKernel, LogPanic, "Cannot create file");

unsigned nBytesWritten;
if (f_write (&File, Msg, Msg.GetLength (), &nBytesWritten) != FR_OK
    || nBytesWritten != Msg.GetLength ())
	m_Logger.Write (FromKernel, LogError, "Write error");

if (f_close (&File) != FR_OK) m_Logger.Write (FromKernel, LogPanic, "Cannot close file");

// ---- read it back ----
Result = f_open (&File, DRIVE "/circle.txt", FA_READ | FA_OPEN_EXISTING);
char Buffer[100];
unsigned nBytesRead;
while ((Result = f_read (&File, Buffer, sizeof Buffer, &nBytesRead)) == FR_OK)
{
	if (nBytesRead > 0) { /* Buffer[0..nBytesRead-1] */ }
	if (nBytesRead < sizeof Buffer) break;        // EOF
}
f_close (&File);

// ---- unmount ----
f_mount (0, DRIVE, 0);
```

**Variant flags you will actually want:**
| Need | Flags |
|---|---|
| read existing only | `FA_READ \| FA_OPEN_EXISTING` |
| create if missing, keep contents | `FA_READ \| FA_WRITE \| FA_OPEN_ALWAYS` (then `f_lseek (&File, f_size (&File))` to append; `f_size()` needs `FF_USE_STRFUNC`/`FF_FS_MINIMIZE<=1` — it is a macro over `fptr`, always available) |
| overwrite | `FA_WRITE \| FA_CREATE_ALWAYS` |
| random access (disk images) | `FA_READ \| FA_WRITE \| FA_OPEN_EXISTING` then `f_lseek (&File, offset)` before each `f_read`/`f_write` |
| durability | `f_sync (&File)` (flush without closing) and/or `f_close()`; `f_mount(0, drv, 0)` on shutdown. `f_write` only flushes when the sector buffer is full. |

`FF_USE_FASTSEEK = 0` (`ffconf.h:37`), so random seeks on large disk images are O(chain length) — expect slow `f_lseek` into the middle of a 180 KB ATR. A linear read of the whole image into RAM at mount time is much faster.

### 7.3 `addon/SDCard` — block drivers

Contents: `emmc.cpp/h` (`CEMMCDevice`), `sdhost.cpp/h` (`CSDHOSTDevice`), `mmchost.cpp/h`, `mmc.h`, `mmcerror.h`, plus `sample/`.

`addon/SDCard/sample/kernel.cpp` / `kernel.h` show the exact usage:

```cpp
// kernel.h
CEMMCDevice m_EMMC;          // ctor in kernel.cpp:32
// kernel.cpp:32
m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED)
// kernel.cpp:78
bOK = m_EMMC.Initialize ();
// kernel.cpp:23,89
#define PARTITION "emmc1-1"
CDevice *pPartition = m_DeviceNameService.GetDevice (PARTITION, TRUE);
```

* `CEMMCDevice` registers the whole card as `"emmc1"` and its MBR partitions as `"emmc1-1"`, `"emmc1-2"`, … (`CPartitionManager`, `lib/fs/partitionmanager.cpp:126–127`). `CSDHOSTDevice` does the same for the SDHOST controller; `README.md:121–122` states EMMC is available for Pi 1–5 and SDHOST only for Pi 1–3, and `sysconfig.h:404–416` says `USE_SDHOST` is only *selected* on `RASPPI <= 3` (and not with `REALTIME`). If `USE_SDHOST` is on, `CSDHOSTDevice` is what you instantiate instead of `CEMMCDevice`.
* **Two EMMC instances on Pi 4/CM4** are possible with different device-selector constructor parameters (`README.md:74`): *"On the Raspberry Pi 4 and CM4 the class can be instantiated twice with different device selector parameter specified to the constructor."* The second one registers as `"emmc2"` / FatFs `"SD2:"` and supports external SDIO breakout boards at GPIO22-27, EMMC host only, not with `USE_SDHOST` (`README.md:74`).
* **No DMA** — `doc/dma-buffer-requirements.txt:52–53`: *"The SD card device driver CEMMCDevice does not use DMA and does not need cache-aligned DMA buffers."*
* `NO_BUSY_WAIT` makes transfers yield to the scheduler instead of spinning, but then *"transfers must not be initiated from a secondary CPU core"* and the scheduler must exist (`sysconfig.h:285–290`).

### 7.4 Recommendation

**(a) Settings persisted on the SD card next to the kernel → FatFs `"SD:"`.** It is the same filesystem the firmware and the kernel image live on, so the file is trivially editable from a PC. Use `f_open (…, FA_READ|FA_WRITE|FA_CREATE_ALWAYS)` for a tiny text/binary blob. `CFATFileSystem` would also work and is smaller, but it cannot use long names and has no subdirectories, so it is strictly worse for anything but a fixed 8.3-name config file.

**(b) Atari disk/cartridge/cassette images on the SD card *or* on a USB stick → FatFs, `"SD:"` / `"USB:"`.** Long file names (`FF_USE_LFN = 3`, `FF_MAX_LFN = 255`) are essential for real-world image files, and FatFs reads/writes sectors directly through the block device with a bounce buffer for unaligned transfers (`diskio.cpp:127–157`). For read-mostly use, open the image once at mount time and copy it into a heap buffer, because `FF_USE_FASTSEEK = 0`.

Both (`a`) and (`b`) work with a single `FATFS` object per volume; declare two (`FATFS m_SDFs; FATFS m_USBFs;`) if you want both mounted simultaneously.

---

## 8. GPIO

### 8.1 `CGPIOPin` — exact API

The class is selected by arch in `include/circle/gpiopin.h:84–86`: the classic implementation for `RASPPI <= 4`, and `include/circle/gpiopin2712.h` for `RASPPI >= 5`. **`TGPIOMode`, `TGPIOPullMode`, `TGPIOInterrupt`, `TGPIOVirtualPin` and `TGPIOInterruptHandler` are defined once, above the split (`gpiopin.h:26–82`), so they are shared.**

Shared enumerations (`gpiopin.h:26–82`), verbatim:

```cpp
#define GPIO_PINS	54
#define LOW	0
#define HIGH	1

enum TGPIOVirtualPin { GPIOPinAudioLeft = GPIO_PINS, GPIOPinAudioRight, GPIOPinUnknown };

enum TGPIOMode
{
	GPIOModeNone,
	GPIOModeInput,
	GPIOModeOutput,
	GPIOModeInputPullUp,
	GPIOModeInputPullDown,
	GPIOModeAlternateFunction0,  // ... 1, 2, 3, 4, 5
#if RASPPI >= 5
	GPIOModeAlternateFunction6, GPIOModeAlternateFunction7, GPIOModeAlternateFunction8,
#endif
	GPIOModeUnknown
};

enum TGPIOPullMode
{
	GPIOPullModeOff,
	GPIOPullModeDown,
	GPIOPullModeUp,
	GPIOPullModeUnknown
};

enum TGPIOInterrupt
{
	GPIOInterruptOnRisingEdge,
	GPIOInterruptOnFallingEdge,
	GPIOInterruptOnHighLevel,
	GPIOInterruptOnLowLevel,
	GPIOInterruptOnAsyncRisingEdge,
	GPIOInterruptOnAsyncFallingEdge,
#if RASPPI >= 5
	GPIOInterruptOnDebouncedHighLevel,
	GPIOInterruptOnDebouncedLowLevel,
#endif
	GPIOInterruptUnknown
};

typedef void TGPIOInterruptHandler (void *pParam);
```

Class API for `RASPPI <= 4` (`gpiopin.h:92–154`):

```cpp
CGPIOPin (void);
CGPIOPin (unsigned nPin, TGPIOMode Mode, CGPIOManager *pManager = 0);
~CGPIOPin (void);

void AssignPin (unsigned nPin);
void SetMode (TGPIOMode Mode, boolean bInitPin = TRUE);
void SetPullMode (TGPIOPullMode Mode);

void Write (unsigned nValue);          // LOW or HIGH
unsigned Read (void) const;
void Invert (void);

void ConnectInterrupt (TGPIOInterruptHandler *pHandler, void *pParam,
                       boolean bAutoAck = TRUE);
void DisconnectInterrupt (void);
void EnableInterrupt  (TGPIOInterrupt Interrupt);
void DisableInterrupt (void);
void EnableInterrupt2 (TGPIOInterrupt Interrupt);   // second source on the same pin
void DisableInterrupt2 (void);
void AcknowledgeInterrupt (void);

static void SetModeAll (u32 nInputMask, u32 nOutputMask);   // GPIO0..31 only
static void WriteAll (u32 nValue, u32 nMask);
static u32  ReadAll (void);
```

Notes grounded in the implementation:
* `GPIOModeInputPullUp` is sugar for `SetMode (GPIOModeInput)` **plus** `SetPullMode (GPIOPullModeUp)` — see `lib/gpiopin.cpp:135–141` (and `GPIOModeInputPullDown` → `GPIOPullModeDown` at `:139–140`). With `bInitPin = FALSE` the pull configuration is not applied (`gpiopin.cpp:83–96, 109`).
* `ConnectInterrupt` requires `m_pManager != 0`, i.e. you must pass a `CGPIOManager *` to the constructor (`lib/gpiopin.cpp:211, 227`).
* `Read()` asserts the pin is an input mode (`lib/gpiopin.cpp:172–175`), `Write()` asserts output mode (`:189`), so you cannot read back an output pin with `Read()` in this revision.
* Interrupts are only allowed on input pins (`lib/gpiopin.cpp:196–200` and following).
* `AcknowledgeInterrupt()` must be called from the handler when `bAutoAck = FALSE` (`gpiopin.h:126–128`; `lib/gpiopin.cpp:315–316` asserts `!m_bAutoAck`).
* Pass `nPin >= GPIO_PINS` and it is translated through the VideoCore (`m_nPin = CMachineInfo::Get()->GetGPIOPin ((TGPIOVirtualPin) nPin)`, `lib/gpiopin.cpp:71–73`) — that is how `GPIOPinAudioLeft`/`GPIOPinAudioRight` work.
* `CGPIOPinFIQ` (`include/circle/gpiopinfiq.h`) is the separate FIQ-driven single-pin class; `USE_GPIO_MANAGER_FIQ` makes `CGPIOManager` use FIQ instead (`README.md:68`).

### 8.2 `CGPIOManager`

`include/circle/gpiomanager.h:27–48`:
```cpp
CGPIOManager (CInterruptSystem *pInterrupt);
boolean Initialize (void);
```
It is a **per-instance** multiplexer (not a singleton), holding `CGPIOPin *m_apPin[GPIO_PINS]` and connecting/disconnecting the shared GPIO IRQ lazily when pins attach (`gpiomanager.h:36–38,47`). Declare it as a `CKernel` member and `Initialize()` it **before** any pin that uses interrupts, and after `m_Interrupt.Initialize()` (`sample/09-softserial/kernel.cpp:28, 73–76`).

### 8.3 `CGPIOClock`

Implementation split for Pi 5 (`include/circle/gpioclock.h:23–25` → `gpioclock-rp1.h`). Shared `TGPIOClock` enum (`gpioclock.h:29–38`):

```cpp
enum TGPIOClock
{
	GPIOClockCAM0 = 8,
	GPIOClockCAM1 = 9,
	GPIOClock0    = 14,   // on GPIO4 Alt0 or GPIO20 Alt5
	GPIOClock1    = 15,   // RPi 4: on GPIO5 Alt0 or GPIO21 Alt5
	GPIOClock2    = 16,   // on GPIO6 Alt0
	GPIOClockPCM  = 19,
	GPIOClockPWM  = 20
};
enum TGPIOClockSource
{                                                    // RPi 1-3:      RPi 4:
	GPIOClockSourceOscillator = 1,               // 19.2 MHz      54 MHz
	GPIOClockSourcePLLC       = 5,               // 1000 MHz      1000 MHz (may vary)
	GPIOClockSourcePLLD       = 6,               // 500 MHz       750 MHz
	GPIOClockSourceHDMI       = 7,               // 216 MHz       unused
	GPIOClockSourceUnknown    = 16
};

class CGPIOClock
{
public:
	CGPIOClock (TGPIOClock Clock, TGPIOClockSource Source = GPIOClockSourceUnknown);
	boolean Start (unsigned nDivI, unsigned nDivF = 0, unsigned nMASH = 0); // 1..4095 / 0..4095 / 0..3
	boolean StartRate (unsigned nRateHZ);        // assigns source automatically
	void Stop (void);
};
```
(`gpioclock.h:40–69`.) README release notes (`README.md:69`): *"`CGPIOClock::StartRate()` can use fractional dividers on Raspberry 1-4. Before only integer dividers were supported. Now automatically a fractional divider will be used with MASH 1, if necessary."*

You must put the target GPIO into the corresponding alternate function yourself — `sample/11-gpioclock/kernel.cpp:39–41`:
```cpp
m_Clock0 (GPIOClock0),
m_ClockPin (4, GPIOModeAlternateFunction0),
m_InputPin (GPIO_INPUT_PIN, GPIOModeInput)
// ...
if (!m_Clock0.StartRate (CLOCK_RATE))   // sample uses 125000 Hz
	m_Logger.Write (FromKernel, LogPanic, "Cannot generate %u Hz clock", CLOCK_RATE);
```

### 8.4 Pi 5 / RP1 differences

* The class is `CGPIOPin` in both cases (same name, selected by `#if RASPPI >= 5`), so **source-compatible for the shared subset**. `gpiopin.h:90` even says *"For the Raspberry Pi 5 is only a subset of methods supported."*
* Additional Pi-5-only members (`gpiopin2712.h:64–67`): `SetSchmittTrigger (boolean)`, `SetFilterConstant (unsigned)` (127 max), `SetDriveStrength (TGPIODriveStrength)` (`GPIODriveStrength2mA/4mA/8mA/12mA`, `gpiopin2712.h:31–38`), `SetSlewRate (TGPIOSlewRate)` (`Slow`/`Limited`, `Fast`/`NotLimited`, `gpiopin2712.h:40–47`), plus `static void DumpStatus (void)` under `#ifndef NDEBUG`.
* **Pin numbering:** for the first 54 pins the numbering is the same as on older models, but the register banks differ — `lib/gpiopin2712.cpp:678–698`:
  ```
  683  if (nPin < 28)        { bank 0; bankpin = nPin; }
  688  else if (nPin < 34)   { bank 1; bankpin = nPin - 28; }
  695  else                  { assert (nPin < 54); bank 2; bankpin = nPin - 34; }
  ```
  So GPIO0–27 are the 40-pin header pins (bank 0), 28–33 bank 1, 34–53 bank 2. `lib/gpiopin2712.cpp:31` carries the comment `// TODO: Support non-RP1 GPIO pins`.
* **The RP1 must be initialised first.** `CGPIOPin::SetMode` asserts `CSouthbridge::IsInitialized ()` (`lib/gpiopin2712.cpp:185`, same at `:275`). That is handled for you by `sysinit()` (`lib/sysinit.cpp:385–391`, `CSouthbridge Southbridge (&InterruptSystem); … Initialize()`, skipped only if `NO_SOUTHBRIDGE_EARLY`).
* Every `CGPIOPin` on Pi 5 holds its own `CSpinLock` (`gpiopin2712.h:119`) plus a static one (`:121`), whereas the pre-Pi-5 class has only a static lock (`gpiopin.h:177`).
* Pi-5-only interrupt modes `GPIOInterruptOnDebouncedHighLevel` / `…LowLevel` (`gpiopin.h:76–77`).
* `CGPIOClock` on Pi 5 is a different class body (`gpioclock-rp1.h`); the enum values above remain valid, but verify the RP1 clock-source semantics on hardware — this is a **hardware-verification item** (§10.8), not something the tree documents.

### 8.5 Working snippets

**Input with pull-up, and read** (pattern from `sample/11-gpioclock/kernel.cpp:41,139` and `lib/gpiopin.cpp:135–141`):
```cpp
#include <circle/gpiopin.h>

CGPIOPin MyInput (23, GPIOModeInputPullUp);   // GPIO23, internal pull-up enabled

unsigned nLevel = MyInput.Read ();            // LOW (0) or HIGH (1)
```

Explicit two-step form (required when you want to re-pull an existing pin):
```cpp
CGPIOPin MyInput (23, GPIOModeInput);
MyInput.SetPullMode (GPIOPullModeDown);       // GPIOPullModeOff | Down | Up
```

**Output, write, invert** (`sample/01-gpiosimple/kernel.cpp:39–52`):
```cpp
CGPIOPin MyOut (17, GPIOModeOutput);

MyOut.Write (HIGH);
MyOut.Invert ();
MyOut.Write (LOW);
```

**Edge interrupt** — manager + handler, pattern from `sample/09-softserial/kernel.cpp:28,73–76` combined with the `ConnectInterrupt`/`EnableInterrupt` contract:

```cpp
class CKernel
{
	// ...
	CGPIOManager m_GPIOManager;              // ctor: m_GPIOManager (&m_Interrupt)
	CGPIOPin      m_Pin;                     // ctor: m_Pin (23, GPIOModeInputPullUp, &m_GPIOManager)
	static void GPIInterruptStub (void *pParam);
};

// Initialize(): after m_Interrupt.Initialize()
bOK = m_GPIOManager.Initialize ();

// wiring (Run() or Initialize(), after the manager exists)
m_Pin.ConnectInterrupt (GPIInterruptStub, this, TRUE);       // TRUE: auto-acknowledge
m_Pin.EnableInterrupt (GPIOInterruptOnFallingEdge);

// handler — runs at IRQ_LEVEL, signature: void (*)(void *pParam)
void CKernel::GPIInterruptStub (void *pParam)
{
	CKernel *pThis = static_cast<CKernel *> (pParam);
	// keep it short: set a volatile flag / push an event, do not print or allocate
}

// teardown
m_Pin.DisableInterrupt ();
m_Pin.DisconnectInterrupt ();
```

With `bAutoAck = FALSE` you must call `m_Pin.AcknowledgeInterrupt ()` from inside the handler instead (`gpiopin.h:126–128`, `lib/gpiopin.cpp:315–316`). Use `EnableInterrupt2()` for a second edge on the same pin (`gpiopin.h:136–138`).

**Fast bulk pin access** (one register write for 32 pins) — `sample/11-gpioclock/sampler.S` uses `ReadAll()`:
```cpp
CGPIOPin::SetModeAll (0xFFFFFFFF, 0);        // all GPIO0-31 as inputs
u32 nLevels = CGPIOPin::ReadAll ();
if (nLevels & (1u << 23)) { /* GPIO23 high */ }
```

`doc/realtime.txt:32–34` gives the achievable rates: *"The GPIO block of the Raspberry Pi is able to output data with up to about 60 MHz (32-bit words) and to input data with up to about 15 MHz. The exact rates depend on the Raspberry Pi model and the system clock."*

---

## 9. Timing, threads, interrupts

### 9.1 `CTimer`

`include/circle/timer.h:47–196`. There is **no `usleep()`** and **no `Delay()`** method; the delay methods are named `usDelay`/`MsDelay` (and `nsDelay` only when `CALIBRATE_DELAY` is on):

```cpp
#define HZ		100			///< ticks per second      timer.h:30
#define MSEC2HZ(msec)	((msec) * HZ / 1000)                            // timer.h:32

typedef void TKernelTimerHandler (TKernelTimerHandle hTimer, void *pParam, void *pContext); // :36
typedef void TPeriodicTimerHandler (void);                                      // :43

CTimer (CInterruptSystem *pInterruptSystem);        // :50
boolean Initialize (void);                          // :53

static unsigned GetClockTicks (void);               // :68   1 MHz counter, may wrap
static u64      GetClockTicks64 (void);             // :71   1 MHz counter, continuous
#define CLOCKHZ	1000000                             // :72

unsigned GetTicks (void) const;                     // :75   1/HZ s since boot, may wrap
unsigned GetUptime (void) const;                    // :77   seconds since boot
unsigned GetTime (void) const;                      // :87   seconds (RTC-adjusted if set)
CString *GetTimeString (void);                      // :109

TKernelTimerHandle StartKernelTimer (unsigned nDelay, TKernelTimerHandler *pHandler,
                                     void *pParam = 0, void *pContext = 0);     // :118
void CancelKernelTimer (TKernelTimerHandle hTimer);                             // :125

void MsDelay  (unsigned nMilliSeconds);             // :129   <= 2000 ms (SimpleMsDelay)
void usDelay  (unsigned nMicroSeconds);             // :132
void nsDelay  (unsigned nNanoSeconds);              // :135   #ifdef CALIBRATE_DELAY only

static CTimer *Get (void);                          // :139
static void SimpleMsDelay  (unsigned nMilliSeconds);   // :143  usable before CTimer exists
static void SimpleusDelay  (unsigned nMicroSeconds);   // :146

void RegisterPeriodicHandler (TPeriodicTimerHandler *pHandler);   // :152  called HZ times/s
```
Up to `TIMER_MAX_PERIODIC_HANDLERS = 4` periodic handlers can be registered (`timer.h:188–190`).

Timing facts:
* `GetClockTicks()` returns **microseconds** at a fixed 1 MHz rate; `CLOCKHZ == 1000000` (`timer.h:72`).
* Implementation (`lib/timer.cpp:211–240`): with `USE_PHYSICAL_COUNTER` it reads `CNTPCT_EL0`/`CNTFRQ_EL0` and scales to 1 MHz — *"Reading this counter is much faster than reading the BCM2835 system timer counter"* (`sysconfig.h:162–168`). Otherwise it reads `ARM_SYSTIMER_CLO` under `PeripheralEntry()/PeripheralExit()`.
* `USE_PHYSICAL_COUNTER` is **on by default for `RASPPI >= 2`** (`sysconfig.h:170–172`) and **mandatory on Pi 4/5**:
  ```
  sysconfig.h:167–168   "On the Raspberry Pi 4 and 5 setting this option is required."
  lib/timer.cpp:29–30   #if RASPPI >= 4 && !defined (USE_PHYSICAL_COUNTER)
                            #error USE_PHYSICAL_COUNTER is required on Raspberry Pi 4!
  ```
* `SimpleusDelay()` / `usDelay()` / `MsDelay()` are **busy-wait loops** on `GetClockTicks()` (`lib/timer.cpp:653–677`). They are safe from IRQ level (no scheduler, no locking) but they block the core. Resolution is one 1 MHz tick plus the loop overhead.
* `MsDelay()` is documented as `<= 2000` ms (`timer.h:127–129`).
* `GetTicks()`/`GetUptime()`/`GetTime()` are only meaningful after `Initialize()` and are updated from the HZ-tick IRQ (`timer.h:173–175`).

### 9.2 `CUserTimer` — fine-grained periodic IRQ

`include/circle/usertimer.h:26–66`:
```cpp
typedef void TUserTimerHandler (CUserTimer *pUserTimer, void *pParam);

CUserTimer (CInterruptSystem *pInterruptSystem, TUserTimerHandler *pHandler,
            void *pParam = 0, boolean bUseFIQ = FALSE);       // :37
boolean Initialize (void);                                     // :45  starts with a 1 h delay
void Stop (void);                                              // :48
void Start (unsigned nDelayMicros);                            // :53  > 1, callable from the handler
#define USER_CLOCKHZ	1000000U                               // :54
```
*"Fine grained user programmable interrupt timer (based on ARM_IRQ_TIMER1)"* (`usertimer.h:30`). `Start()` *"Must be called from user timer handler to set new delay"* and *"Can be called on a running user timer with a new delay"* (`usertimer.h:51–52`) — i.e. it is a **one-shot re-armed from its own handler**, the natural way to build a sample-accurate or frame-accurate tick. The handler runs at `IRQ_LEVEL` (or `FIQ_LEVEL` with `bUseFIQ = TRUE`). `sample/36-softpwm` (*"Using the class CUserTimer to implement software PWM"*, `sample/README:37`) is the reference.

### 9.3 `CInterruptSystem`

`include/circle/interrupt.h:27–72`:
```cpp
typedef void TIRQHandler (void *pParam);          // :27

boolean Initialize (void);                        // :37
void ConnectIRQ (unsigned nIRQ, TIRQHandler *pHandler, void *pParam);   // :39
void DisconnectIRQ (unsigned nIRQ);                                     // :40
void ConnectFIQ (unsigned nFIQ, TFIQHandler *pHandler, void *pParam);   // :42
void DisconnectFIQ (void);                                              // :43
static void EnableIRQ  (unsigned nIRQ);   static void DisableIRQ  (unsigned nIRQ);   // :45-46
static void EnableFIQ  (unsigned nFIQ);   static void DisableFIQ  (void);            // :48-49
static CInterruptSystem *Get (void);                                            // :51
static void InterruptHandler (void);                                            // :53
#if RASPPI >= 4
	static void InitializeSecondary (void);                                 // :56
	static void SendIPI (unsigned nCore, unsigned nIPI);                    // :58
	static void CallSecureMonitor (u32 nFunction, u32 nParam);              // :60
	static void SecureMonitorHandler (u32 nFunction, u32 nParam);           // :61
#endif
```
Two things to note against older Circle documentation:
1. **`ConnectIRQ()` has no IRQ-level parameter** in this revision (older releases took an `nIRQLevel` third/fourth argument). The handler signature is `void (*)(void *pParam)`.
2. `CInterruptSystem` is constructed by **`sysinit()`** as a local object (`lib/sysinit.cpp:379–383`) — a `CInterruptSystem` member in `CKernel` is required for `CTimer`, drivers, etc., but note that this member is a *second* instance; `CInterruptSystem::Get()` returns the singleton set by the constructor and asserts if none exists (`interrupt.h:71`, and `Get()` is used e.g. by `CBcmFrameBuffer`'s DMA channel at `lib/bcmframebuffer.cpp:61`). Follow the samples: declare `CInterruptSystem m_Interrupt;` in `CKernel` **before** `CTimer` (`sample/05-usbsimple/kernel.h:60–61`).

Interrupt priority levels (`include/circle/synchronize.h:37–39`):
```
#define TASK_LEVEL		0		// IRQs and FIQs enabled
#define IRQ_LEVEL		1		// IRQs disabled, FIQs enabled
#define FIQ_LEVEL		2		// IRQs and FIQs disabled
```
with `void EnterCritical (unsigned nTargetLevel = IRQ_LEVEL);` / `void LeaveCritical (void);` (`synchronize.h:54–56`) — *"EnterCritical() can be nested with same or increasing nTargetLevel"*. `DMA_BUFFER(type, name, num)` is defined at `synchronize.h:156`.

`CIRQHandler` table size is `IRQ_LINES` (`interrupt.h:68–69`); `IRQ_LINES` comes from the SoC headers (`bcm2835int.h` for `RASPPI <= 3`, `bcm2711int.h` for 4, `rp1int.h` for 5).

### 9.4 `CScheduler` / `CTask`

`CScheduler` (`include/circle/sched/scheduler.h:41–108`) — *"Cooperative non-preemptive scheduler, which controls which task runs at a time"*:

```cpp
void Yield (void);                            // :49
void Sleep (unsigned nSeconds);               // :52
void MsSleep (unsigned nMilliSeconds);        // :54
void usSleep (unsigned nMicroSeconds);        // :56
CTask *GetCurrentTask (void);                 // :59
CTask *GetTask (const char *pTaskName);       // :63
boolean IsValidTask (CTask *pTask);           // :67
void RegisterTaskSwitchHandler    (TSchedulerTaskHandler *pHandler);   // :72
void RegisterTaskTerminationHandler (TSchedulerTaskHandler *pHandler);// :76
void SuspendNewTasks (void);  void ResumeNewTasks (void);              // :80,83
boolean EnumerateTasks (…);   void ListTasks (CDevice *pTarget);       // :89,98
static CScheduler *Get (void);                                         // :101
static boolean IsActive (void);                                        // :105
```
Header note: *"This scheduler uses the round-robin policy, without priorities."* (`scheduler.h:39`). `CScheduler::IsActive()` explicitly says *"The scheduler is optional in Circle"* (`scheduler.h:103–108`). It must be instantiated as a `CKernel` member (`sample/19-tasks/kernel.h:68`). `doc/multicore.txt:62–64`: *"The cooperative non-preemptive scheduler is intended to allow multiple threads of operation on a single core. It cannot be used on more than one core at a time and should always run on core 0."*

`CTask` (`include/circle/sched/task.h:43–134`):
```cpp
CTask (unsigned nStackSize = TASK_STACK_SIZE, boolean bCreateSuspended = FALSE);  // :48
virtual void Run (void);        // :53  override this
void Start (void);  void Suspend (void);  void Resume (void);  boolean IsSuspended (void) const;
void Terminate (void);  void WaitForTermination (void);
void SetName (const char *pName);  const char *GetName (void) const;
void SetUserData (void *pData, unsigned nSlot);  void *GetUserData (unsigned nSlot);
TStackInfo GetStack (void) const;                              // :95
```
`TTaskState` (`task.h:30–39`) and `TTaskFlags` (`scheduler.h:30–35`) are available for `EnumerateTasks()`. Initialisation order: the `CScheduler` member must exist before any `new CMyTask(...)` because `CTask::CTask` calls `CScheduler::Get()->AddTask (this)` (`lib/sched/task.cpp:53`).

### 9.5 `CMulticore` — the actual name is `CMultiCoreSupport`

**There is no class named `CMulticore`.** The class is `CMultiCoreSupport` (`include/circle/multicore.h:40–88`), and it is **only declared when `ARM_ALLOW_MULTI_CORE` is defined** (`multicore.h:25`):

```cpp
class CMultiCoreSupport
{
public:
	CMultiCoreSupport (CMemorySystem *pMemorySystem);
	boolean Initialize (void);
	virtual void Run (unsigned nCore) = 0;                  // :48
	virtual void IPIHandler (unsigned nCore, unsigned nIPI); // :50
	static void SendIPI (unsigned nCore, unsigned nIPI);    // :53
	static void HaltAll (void);                             // :54
	static unsigned ThisCore (void);                        // :62  0..CORES-1
	static void EntrySecondary (void);                      // :78
};
```
`ARM_ALLOW_MULTI_CORE` is set by `configure --multicore` (`configure:141–144` → `DEFINE += -DARM_ALLOW_MULTI_CORE`) or by hand in `sysconfig.h:153–158` — *"It should not be defined for single core applications, because this may slow down the system because multiple cores may compete for bus time without use."* `doc/multicore.txt:17–19` repeats that. `CORES` is 4 (`memorymap.h:55`, `memorymap64.h:36`). Multicore init must be **last** in `CKernel::Initialize()` (`doc/multicore.txt:21–23`).

Thread-safe classes for multi-core use are listed in `doc/multicore.txt:25–41`: `CScreenDevice`, `CSerialDevice`, `CLogger`, `CTimer`, USB, FAT file system (not from IRQ), `CDeviceNameService` (not from IRQ), `CBcmPropertyTags` (not from IRQ), `CBcmMailBox` (not from IRQ), `CI2CMaster` (not from IRQ), `CGPIOPin` (one core at a time per instance), `CPWMOutput`, `CBcmRandomNumberGenerator`. Everything else needs `CSpinLock` (`spinlock.h`) — *"Synchronization in single-core applications was normally done by EnterCritical(). From now on the class CSpinLock will be used."* (`doc/multicore.txt:43–48`). `DataSyncBarrier()`/`DataMemBarrier()`/`InstructionSyncBarrier()` are the primitives (`include/circle/synchronize.h`).

### 9.6 Recommended emulator timing pattern

**Frame rate arithmetic.** `CTimer::GetClockTicks()` is a 1 MHz counter:
* 50 Hz (PAL) → `CLOCKHZ / 50 = 20000` ticks per frame
* 60 Hz (NTSC) → `CLOCKHZ / 60 ≈ 16666.67` ticks; use a fractional accumulator rather than a rounded constant, or you will drift ~0.02 % (≈ 1 frame per 83 s for a 16667 rounding).
* A PAL Atari frame is 312 scan lines × 228 colour clocks ≈ 1.7897725 MHz / 312 / 228 → 1.7897725e6 / 71136 ≈ 25.17 … no — the practical number is Atari800's own `Atari800_Frame()`; the *pacing* only needs 50 Hz / 60 Hz.

**Recommended: single-threaded busy-paced loop on `GetClockTicks()`.** Rationale, grounded in the source:
* The scheduler is *cooperative* and *optional* (`scheduler.h:39, 103–108`), so `MsSleep()`/`usSleep()` only help if you create tasks anyway; and `CScheduler` adds a context switch per wakeup.
* `SimpleusDelay`/`GetClockTicks` are cheap and IRQ-safe (physical counter path, `lib/timer.cpp:229–238`).
* Frame pacing must not be disturbed by `CLogger` writes or USB bulk traffic; `doc/realtime.txt` is explicit that screen/serial writes and low/full-speed USB raise interrupt latency, and `doc/multicore.txt:55–60` warns about USB bulk vs interrupt transfers.

Pattern with a fractional accumulator (drift-free at both 50 and 60 Hz):

```cpp
// cls = 0 for PAL (50 Hz), 1 for NTSC (60 Hz)
static const unsigned s_FrameRate[2] = { 50, 60 };

unsigned nFrameCount = 0;
// nNext = CLOCKHZ * (nFrameCount + 1) / rate  is the ideal boundary
// integer form, no floating point:
const unsigned nRate = s_FrameRate[m_nVideoStandard];
unsigned nNext = CLOCKHZ / nRate;
unsigned nFrames = 0;

for (;;)
{
	// ---- 1. input ----
	if (m_USBHCI.UpdatePlugAndPlay ()) { /* (re)acquire ukbd1 / gamepad */ }

	// ---- 2. emulate exactly one video frame ----
	// EmulateFrame ();         // produces PAL/NTSC video + N POKEY samples

	// ---- 3. audio: top up whatever the queue will take ----
	// unsigned nFree = m_pSound->GetQueueSizeFrames () - m_pSound->GetQueueFramesAvail ();
	// if (nFree > 0) { RenderAudio (pBuf, nFree); m_pSound->Write (pBuf, nFree*4); }

	// ---- 4. present the frame ----
	// (C2DGraphics-style SetArea + SetVirtualOffset, or raw pointer + WaitForVerticalSync)

	// ---- 5. wait for the ideal next-frame boundary ----
	nFrames++;
	unsigned nTarget = (unsigned) (((u64) CLOCKHZ * nFrames) / nRate);
	while ((int) (CTimer::GetClockTicks () - nTarget) < 0)
	{
		// busy-wait; optionally call m_USBHCI.UpdatePlugAndPlay() here
		// or CGPIOPin::ReadAll() to poll joystick GPIO
	}
	nNext = nTarget;
}
```

Notes:
* The `(int)(a - b) < 0` idiom is wrap-safe for a 1 MHz 32-bit counter (wraps every ~71 minutes) — it is the same idiom the library uses (`lib/timer.cpp:663, 671`).
* If you want to yield CPU, insert `if (m_Scheduler.IsActive()) m_Scheduler.Yield ();` — but *only* if you declared a `CScheduler` member, and remember it is cooperative, so any task that never yields will block the frame loop.
* **Audio must be driven from the frame loop, not from a task** unless you are careful: `CSoundBaseDevice::Write()` takes a `CSpinLock` (`lib/sound/soundbasedevice.cpp:287, 390`) and is explicitly *"Can be called on any core"* (`soundbasedevice.h:149`). The alternative callback design (`GetChunk`) runs at IRQ level (`soundbasedevice.h:214–222`) — there, the emulator must be able to render audio from IRQ context without taking a lock the main loop holds.
* **Do not run the emulation loop at IRQ level.** `doc/realtime.txt:23–26` explains why screen/serial writes from `IRQ_LEVEL` are silently dropped when `REALTIME` is defined, and `sample/08-usbkeyboard`/`27-usbgamepad` *"do not work with the REALTIME option"*. Keep emulation at `TASK_LEVEL`; use IRQ level only for the sound chunk producer if you choose the `GetChunk` design.
* `doc/dma-buffer-requirements.txt:26–53` governs any buffer you hand to DMA. For sound devices you do **not** need to align application buffers, because `CSoundBaseDevice::Write()` copies through its own queue; but if you write your own DMA path you must use `DMA_BUFFER()` (`synchronize.h:156`) or heap memory (`doc/dma-buffer-requirements.txt:83–86`: *"Data buffers allocated from the heap are always cache-aligned"*).
* Cache line size (relevant for any DMA you add yourself): *"32 bytes for Raspberry Pi 1 and Zero, 64 bytes for Raspberry Pi 2-4"* (`doc/dma-buffer-requirements.txt:10–16`).

---

## 10. Risks and gotchas

Ordered roughly by how likely they are to bite this project.

### 10.1 Silent halt if the kernel image exceeds `KERNEL_MAX_SIZE` (2 MB default) — **top risk**

`include/circle/sysconfig.h:37–39`:
```c
#ifndef KERNEL_MAX_SIZE
#define KERNEL_MAX_SIZE		(2 * MEGABYTE)
#endif
```
`lib/sysinit.cpp:341–347`:
```cpp
	// halt, if KERNEL_MAX_SIZE is not properly set
	// cannot inform the user here
	extern unsigned char _end;
	if (MEM_KERNEL_END < reinterpret_cast<uintptr>(&_end))
	{
		halt ();
	}
```
An Atari800 build with embedded OS ROMs, fonts, and the `ffunicode.c` code-page tables (1.3 MB of C source, `addon/fatfs/ffunicode.c`!) can plausibly cross 2 MB. The kernel image is also limited to `max. 2 MByte` per the memory map (`doc/memorymap.txt:12, 46, 103, 145, 205, 247`). **Fix:** `./configure --kernel-max-size 4` (or `DEFINE += -DKERNEL_MAX_SIZE=0x400000`), remembering `KERNEL_MAX_SIZE` *"must be a multiple of 16 KByte"* (`sysconfig.h:33–35`). Symptom if you forget: the Pi does nothing at all after the rainbow splash, with no log output.

### 10.2 Cache / memory coherency for DMA

`doc/dma-buffer-requirements.txt:10–16`:
> This is performed using system control operations, which require, that the data buffers used for DMA transfers are aligned to the size of a data cache line in the system (32 bytes for Raspberry Pi 1 and Zero, 64 bytes for Raspberry Pi 2-4). Alignment must be applied to the base address of such buffer and to its size. If this requirement is not met, the Circle application may still work, but under certain conditions data, which is stored near to an unaligned DMA buffer, may be corrupted. Such issues may be difficult to detect.

Applies to buffers you hand to: `CDMAChannel` methods, `CSPIMasterDMA`, raw USB host controller calls (`ControlMessage`, `GetDescriptor`, `Transfer`, `SubmitBlockingRequest`, `SubmitAsyncRequest`), and your own network stack (`doc/dma-buffer-requirements.txt:33–46`). Explicitly **not** required for `CUSBBulkOnlyMassStorageDevice` (the driver detects and bounces, at a memcpy cost — `:48–51`) nor for `CEMMCDevice` (no DMA — `:52–53`).

For the **framebuffer**, the DMA path is inside `CBcmFrameBuffer::SetArea()` when `SCREEN_DMA_BURST_LENGTH` is defined (`lib/bcmframebuffer.cpp:238–239`), and the *source* buffer is `pPixels` — **your application buffer**. `C2DGraphics` passes its own heap-allocated `m_pBuffer8` (`lib/2dgraphics.cpp`), which is cache-aligned because heap allocations always are (`doc/dma-buffer-requirements.txt:83–86`). If you pass a stack or statically-allocated back buffer to `SetArea()`, **use `DMA_BUFFER (u16, BackBuffer, N)`** (`synchronize.h:156`) or expect rare corruption. Alternatively define `NO_SCREEN_DMA_BURST_LENGTH` (`sysconfig.h:252–256`) to force the pure-`memcpy` path, or `-DSCREEN_DMA_BURST_LENGTH=...` to a different burst.

There is no automatic cache flush around `SetArea()`: the driver relies on the buffer being a proper DMA buffer. `doc/dma-buffer-requirements.txt:64–99` gives the three safe definitions (stack via `DMA_BUFFER`, heap via `new`, static via `DMA_BUFFER`).

### 10.3 `new`/`malloc` — the allocator and its sharp edges

* `operator new` / `new[]` without a parameter go to `CMemorySystem::HeapAllocate (nSize, HEAP_DEFAULT_NEW)` (`lib/new.cpp:45–53`; Circle provides these only when `STDLIB_SUPPORT < 3`, `lib/new.cpp:33`). `HEAP_DEFAULT_NEW` defaults to `HEAP_LOW` (`sysconfig.h:58–60`).
* Explicit heap selection: `new (HEAP_LOW) T`, `new (HEAP_HIGH) T`, `new (HEAP_ANY) T`, `new (HEAP_DMA30) T` (`include/circle/new.h:30–32`; `memory.h:74–100`). `HEAP_HIGH` exists only for `RASPPI >= 4` (`memory.h:80–99`).
* **Out-of-memory is a system halt, not a `nullptr`** (`doc/new-operator.txt:30–33`):
  > If explicitly requesting a HEAP_HIGH memory block fails, zero is returned. If requesting any other memory type block fails, because the memory is full, the system generates an "Out of memory" panic message.
  In the code this is literal: the allocator consumes a reserve, then logs at `LogPanic` (`lib/heapallocator.cpp:113–137`, `CLogger::Get()->Write (m_pHeapName, LogPanic, "Out of memory")`), and `CLogger` **calls `halt()` for `LogPanic`** (`lib/logger.cpp:212` and `:257`). So a failed default-heap `new`/`malloc` stops the machine. `if (p == nullptr)` checks are therefore useless for `HEAP_LOW`/`HEAP_ANY`/default — but they *are* meaningful for an explicit `new (HEAP_HIGH) …` (`doc/new-operator.txt:30–31`). Only with `STDLIB_SUPPORT >= 3` does the allocator log a *warning* instead so a C++ exception can be thrown (`lib/heapallocator.cpp:130–134`). This is a real hazard for a large frame buffer or an emulator heap.
* `HEAP_BLOCK_BUCKET_SIZES` (`sysconfig.h:87–89`) is `0x40,0x400,0x1000,0x4000,0x10000,0x40000,0x80000`; *"If the requested size is greater than the largest available bucket size, the block can be allocated, but the memory space is lost, if the block will be freed later"* (`sysconfig.h:72–85`). Any allocation > 512 KB (0x80000) that you free is **leaked**. A 1920×1080×32 back buffer is 8 MB → allocate it **once** and never free it, or add a bigger bucket size.
* `malloc`/`calloc` always use `HEAP_LOW` (`doc/new-operator.txt:45–46`; `sysconfig.h:68–70` says changing that is *"not recommended, because there are device drivers, which require to allocate low memory for DMA purpose using malloc()"*). `malloc` results are 16-byte aligned (`include/circle/alloc.h:29`).
* Page-granular allocation: `palloc()`/`pfree()` (`alloc.h:36–37`) — *"returns aligned page (AArch32: 4K, AArch64: 64K)"*. Note `PAGE_SIZE` is `4096` on AArch32 (`memorymap.h:40`) but **`0x10000` (64 KB) on AArch64** (`memorymap64.h:42`).
* `KASan` shadows whatever your buffers are; not relevant unless enabled.

### 10.4 C++ dialect, exceptions, RTTI

`Rules.mk:239` — default `-std=c++17` (README.md:61: *"Circle is built with `-std=c++17` by default now. There are options for selecting C++14 (previous default) and C++20"*).

With the default `STDLIB_SUPPORT = 1` (`Rules.mk:39`), **exceptions and RTTI are off**:
```
Rules.mk:187–192
	else
		CPPFLAGS  += -fno-exceptions -fno-rtti
		ifneq ($(strip $(CLANG)),1)
			CPPFLAGS  += -nostdinc++
		endif
	endif
```
So `throw`/`try`/`catch`, `dynamic_cast`, and `typeid` are unavailable **unless** you raise `STDLIB_SUPPORT` to 3 or 4 and get libstdc++ from circle-stdlib (`Rules.mk:172–186`; `doc/stdlib-support.txt:8–25`). `README.md:59` confirms `STDLIB_SUPPORT=4` requires the external circle-stdlib project.

Also with `STDLIB_SUPPORT ∈ {0,1}`: `CFLAGS += -ffreestanding` (`Rules.mk:257–259`), and with `STDLIB_SUPPORT=0`: `CFLAGS += -nostdinc` (`Rules.mk:195`) — *"Some modules in the addon/ subdirectory will not build with STDLIB_SUPPORT = 0"* (`doc/issues.txt:31`). `libsound`'s HDMI driver is the concrete example (`doc/issues.txt:33–34`).

Other global flags (`Rules.mk:253–255`):
```
CFLAGS += $(ARCH) $(WARNINGS) -fsigned-char -g $(DEFINE) $(INCLUDE) $(EXTRAINCLUDE) $(OPTIMIZE)
CPPFLAGS += $(CFLAGS) $(STANDARD)
```
i.e. **`-g` debug info is always on and `-O2` is the default** — the `-g` is harmless for size (objcopy strips it from the `.img`) but makes object files large. `DEFINE += -D__circle__=$(CIRCLEVER) -DRASPPI=$(RASPPI) -DSTDLIB_SUPPORT=$(STDLIB_SUPPORT) -D__VCCOREVER__=0x04000000 -U__unix__ -U__linux__` (`Rules.mk:249–250`).

`-fsigned-char` matters for 8-bit emulation portability (`Rules.mk:253`), and `lib/Makefile:82–86` adds `-mno-unaligned-access`/`-mstrict-align` for the library, so any unaligned-access tricks in the emulator core must not rely on the library's flags — set them in your own app Makefile if needed.

`purecall.o` and `cxa_guard.o` are compiled only for `STDLIB_SUPPORT` not in {3,4} (`lib/Makefile:77–79`) — these are Circle's stubs for pure-virtual calls and function-local static guards. Function-local statics are safe.

### 10.5 Usable RAM

From `doc/memorymap.txt` and the headers:

| Target | Kernel load | Coherent region | Heap start | Heap ceiling | Notes |
|---|---|---|---|---|---|
| Pi 1 / Zero (AArch32) | `0x8000` | 1 MB @ 4 MB | after coherent | GPU boundary | `doc/memorymap.txt:12–33` |
| Pi 2/3 (AArch32) | `0x8000` | 1 MB @ 4 MB | after coherent | GPU boundary | `:46–91` |
| Pi 3 (AArch64) | `0x80000` | 1 MB @ 5 MB | `MEM_HEAP_START` | GPU boundary | `:103–132` |
| Pi 4 (AArch32, LPAE) | `0x8000` | 4 MB @ 4 MB | `0x00800000` | GPU boundary + high heap | `:145–190` |
| Pi 4 (AArch64) | `0x80000` | 4 MB @ 5 MB | `0x00900000` | **`MEM_HIGHMEM_END = 3 GB - 1`** | `:205–237`, `memorymap64.h:77` |
| Pi 5 (AArch64) | `0x80000` | 4 MB @ 5 MB | `0x00900000` | **`MEM_HIGHMEM_END = 8 GB - 1`** | `:247–281`, `memorymap64.h:79` |

Key constraints:
* **`MEM_HIGHMEM_END`** for Pi 4 is `3 * GIGABYTE - 1` (`memorymap64.h:77`, `memorymap.h:85`) because *"memory >= 3 GB is not safe to be DMA-able and is not used"*. On a 4 GB Pi 4 the top 1 GB is unusable.
* Pi 5 uses up to 8 GB (`memorymap64.h:79`), so on a 16 GB Pi 5 the top half is unused.
* `doc/new-operator.txt:39–43`: *"Circle can currently handle up to 2 GBytes of high memory… If your Raspberry Pi 4 has 4 GBytes SDRAM, the highest 1 GByte memory region is unused. This is necessary, because memory above the 3 GByte boundary may require a specific DMA-handling, which is not implemented so far."*
* **Low memory vs high memory matters for DMA.** `HEAP_DEFAULT_NEW = HEAP_LOW` and `HEAP_DEFAULT_MALLOC = HEAP_LOW` (`sysconfig.h:58–70`), and `HEAP_DMA30` is an alias for `HEAP_LOW` (`memory.h:78`; `doc/new-operator.txt:9, 23–28`: *"Even if on Raspberry Pi models before the Raspberry Pi 4 a memory type parameter is not required, the HEAP_DMA30 type should be specified (if needed) to ensure, that the code does work on the Raspberry Pi 4 too."*). On Pi 4/5, `HEAP_LOW` is the region below `MEM_HIGHMEM_START` = 1 GB (`memorymap.h:84`, `memorymap64.h:74`).
* `CMemorySystem::GetHeapFreeSpace (HEAP_LOW|HEAP_HIGH|HEAP_ANY)` and `GetLowMemSize()`/`GetHighMemSize()` are available (`memory.h:134–173`).
* GPU memory: `GPU_MEM_SIZE (64 * MEGABYTE) // set in config.txt` (`memorymap.h:37`, `memorymap64.h:39`), and using `gpu_mem=16` requires the cut-down firmware files (`boot/README:41–44`).
* `CMachineInfo::GetRAMSize()` returns MByte (`machineinfo.h:104`).

### 10.6 Thread/task stack sizes and task limits

* `MAX_TASKS 20` — *"maximum number of tasks in the system"* (`sysconfig.h:273–277`). Includes the main task.
* `TASK_STACK_SIZE 0x8000` = **32 KB per task** (`sysconfig.h:279–283`).
* `KERNEL_STACK_SIZE 0x20000` = **128 KB** for the main thread / core 0 (`sysconfig.h:41–46`), *"must be a multiple of 16 KByte"*.
* `EXCEPTION_STACK_SIZE 0x8000` = 32 KB per exception mode per core (`memorymap.h:42`).
* `CTask`'s stack is heap-allocated per task (`task.h:129` `u8 *m_pStack;`), `GetStack()` reports `{top, size}` (`task.h:95–105`).
* Practical consequence: an emulator task that puts a 16 KB scanline buffer on the stack is fine at 32 KB, but a task that puts a full framebuffer on the stack is not. Raise it per task: `CTask (0x20000)`. Raising `TASK_STACK_SIZE` globally also works but multiplies by `MAX_TASKS`.
* `TASK_USER_DATA_SLOTS = 4`, with `TASK_USER_DATA_USER` free for the application and the other three reserved for Linux/Plan-9/libc++ emulation (`task.h:79–83`).
* The **main task's stack is `KERNEL_STACK_SIZE` (128 KB)** and lives at `MEM_KERNEL_STACK` (`task.h:97–100`).

### 10.7 `std::` containers and `std::thread`

* With the default `STDLIB_SUPPORT = 1` there is **no C++ standard library at all**: `-nostdinc++` (`Rules.mk:190`), no `libstdc++` linked (only added for `STDLIB_SUPPORT ∈ {3,4}`, `Rules.mk:172–176`). So `std::vector`, `std::string`, `std::thread`, `std::mutex`, `std::atomic`, `<algorithm>`, `<memory>` are **unavailable**.
* `doc/stdlib-support.txt:8–12` is explicit about the levels:
  > `STDLIB_SUPPORT=0` to build without external libs (old setting) / `=1` to build with libgcc.a only (default now) / `=2` to build with C standard library too / `=3` to build with C++ standard library too / `=4` to build with external C++ standard library
  and `:14–25`: levels 2–4 *"need support from external projects"* — the [circle-stdlib](https://github.com/smuehlst/circle-stdlib) project.
* Note the important caveat: `STDLIB_SUPPORT` is a **single global setting for the whole build** (`Rules.mk:39`), read from `Config.mk`, so switching to level 3/4 means *"enter `./makeall clean` from project root and `make clean` in your application's source directory … and always after changing the configuration"* (`doc/issues.txt:5–9`).
* Substitutes inside Circle: `CString` (`include/circle/string.h`), `CPtrList`/`CPtrListFIQ`/`CPtrArray` (`ptrlist.h`, `ptrarray.h`), `CNumberPool` (`numberpool.h`), `CClassAllocator` (`classallocator.h`), plus the scheduler's `CMutex`/`CSemaphore`/`CSynchronizationEvent`/`CPipe` (`include/circle/sched/`). `CPipe` (FIFO IPC) is new in 51.x (`README.md:70–71`).
* `std::thread` has no meaning here — use `CTask`, and remember it is cooperative and single-core (`doc/multicore.txt:62–64`).

### 10.8 Unconfirmed / must be verified on hardware

These are **not** resolvable from this tree; do not assume either way:

1. **HDMI audio on Pi 5.** The driver compiles and contains BCM2712 D0-specific code (`lib/sound/hdmisoundbasedevice.cpp:265–270, 417–425`; `lib/sound/Makefile:27`), and `README.md:131` marks HDMI sound supported on Pi 5 — but `sample/34-sounddevices/README:8–9` says *"not on Raspberry Pi 5"*. Contradiction inside the tree. Test it.
2. **Pi 5 framebuffer depth / `SetArea` + `SetVirtualOffset` behaviour.** `boot/config64.txt:41–42` ships *commented-out* `framebuffer_depth=32` and `framebuffer_ignore_alpha=1` under `[pi5]` with no explanation, and `lib/2dgraphics.cpp:542` compiles the VSync double-buffer path only for `RASPPI <= 4`. Whether `PROPTAG_SET_DEPTH` + `WaitForVerticalSync` + `SetVirtualOffset` work correctly on Pi 5 is untested here.
3. **`CBcmFrameBuffer::GetBuffer()` on machines with > 1 GB RAM.** The address is masked to 30 bits (`lib/bcmframebuffer.cpp:176` `& 0x3FFFFFFF`). If the firmware ever places the framebuffer above 1 GB on Pi 4/5, `GetBuffer()` returns a wrong pointer. Pi 5 with ≥ 4 GB and `framebuffer_depth=32` at 1920×1080×2 = ~16 MB is a plausible trigger. Verify, and prefer `SetArea()` (which uses the same masked value internally) over raw-pointer access if you see corruption.
4. **`CGPIOClock` on Pi 5 / RP1.** `gpioclock-rp1.h` is a distinct implementation; the shared `TGPIOClockSource` values from `gpioclock.h:40–47` are documented for Pi 1–4 only. `StartRate()` behaviour on RP1 is undocumented in this tree.
5. **Which GPIO pins are usable on Pi 5.** `lib/gpiopin2712.cpp:678–698` maps pins 0–53 to RP1 banks, with `// TODO: Support non-RP1 GPIO pins` (`:31`). Pins 0–27 correspond to the 40-pin header, but that specific mapping is not stated in the tree — confirm against the RP1 documentation before hard-coding a pin number.
6. **USB on Pi 5 with hubs and high-bandwidth devices.** `README.md:143` says the hub driver is USB 2.0 only; `CUSBSubSystem` drives two xHCI instances (`usbsubsystem.h:61–70`). Whether a USB stick attached behind a hub on Pi 5 works reliably is untested here. `doc/issues.txt:36–39` also notes babble errors with some devices on Pi 4.
7. **Pi 5 + HDMI + WLAN known issue** (`doc/issues.txt:51–52`): *"The WLAN connection is delicate on the Raspberry Pi 5, when a HDMI display is used at the same time."* Not relevant unless you add networking.
8. **`Pi 500` support** is `Unknown` (`README.md:199`) even though the build target `kernel_2712` and DTB are provided.
9. **`RASPPI=3` AArch32 on Pi Zero 2 W.** `README.md:231` claims Zero 2 W for `RASPPI=3`, but the produced filename is `kernel8-32.img`, which nothing in `boot/config32.txt` selects. Whether the firmware will load a renamed/`kernel=`-selected `kernel8-32.img` on a Zero 2 W is not documented; the safe choice is `RASPPI=2`.
10. **`CHDMISoundBaseDevice` + `STDLIB_SUPPORT=0` + `AARCH=32` link failure** is documented (`doc/issues.txt:33–34`) but not explained; avoid that combination.
11. **`CKernelOptions` has no `GetSoundDevice()` default** — it returns an empty string unless `sounddev=` is on the command line (`lib/koptions.cpp:48, 283`), so your device-selection code must handle the `else` case (as `sample/34-sounddevices/kernel.cpp:161–169` does).

### 10.9 Miscellaneous gotchas worth a line each

* **`CScreenDevice` without a display fails on Pi 4/5** — define `SCREEN_HEADLESS` or do not use the class (`doc/issues.txt:46–49`, `sysconfig.h:364–375`).
* **Stale objects after `Config.mk` changes** produce bogus link errors — always `./makeall clean` (`doc/issues.txt:5–9`).
* **USB library has no error recovery**: *"There are known issues with the included USB library (e.g. no error recovery, limited split support, limited isochronous support). You should only attach the USB devices, which are needed for your application!"* (`doc/issues.txt:11–13`). This is directly relevant to hot-plugging USB sticks mid-emulation.
* **CPU is not at full speed by default**: *"The system clock of the ARM CPU on the Raspberry Pi in a bare metal environment is normally less than the nominal maximum system clock. It is 700 MHz on the Raspberry Pi 1 and Zero and 600 MHz on the Raspberry Pi 2 and 3"* (`doc/realtime.txt:36–40`). Use `CCPUThrottle` (`include/circle/cputhrottle.h`, `sysconfig.h`/`cmdline.txt` `fast=true`, `socmaxtemp=`, `gpiofanpin=` — `doc/cmdline.txt:65–77`) — `sample/26-cpustress` is the reference. Pi 5 fan pin must be **45** (`doc/cmdline.txt:76–77`).
* **`fast=true` / `CCPUThrottle` interacts with `CMachineInfo::GetClockRate()`** — 51.x fixed the reported value by preferring `GET_CLOCK_RATE_MEASURED` (`README.md:83`).
* **`-fsigned-char`** is forced (`Rules.mk:253`) — good for 6502 code, but any external library built without it will disagree.
* **`-g` is always passed** (`Rules.mk:253`) — your `.o` files and `.elf` will be large; strip with the provided `objcopy` step (`Rules.mk:309`) which is automatic, so the final `.img` is unaffected.
* **`SAVE_VFP_REGS_ON_IRQ`/`_ON_FIQ` are auto-enabled for GCC ≥ 12 on `RASPPI >= 2`** (`sysconfig.h:470–485`), together with `__FAST_MATH__`. IRQ handling is therefore slightly slower but floating-point in handlers is safe. If your emulator core relies on strict IEEE semantics inside IRQ handlers, re-check this.
* **`DATA_SYNC`/`PeripheralEntry` nesting**: `CBcmFrameBuffer::SetArea()` spins on `m_nDMAInUse` with a `// TODO: Yield with NO_BUSY_WAIT, when on core 0 in TASK_LEVEL` (`lib/bcmframebuffer.cpp:224–227`) — a second `SetArea()` from another task will busy-spin, which under a cooperative scheduler can deadlock if the yielding task holds the DMA.
* **`CUSBKeyboardDevice` handler runs in IRQ context** — no logging, no framebuffer writes, no allocation from it.
* **`f_mount`/FatFs with `FF_USE_LFN = 3` allocates from the heap on long-name operations** (`addon/fatfs/ffconf.h:116–133`) and `FF_FS_REENTRANT = 1` takes a mutex per call (`ffconf.h:295–309`) — do not call FatFs from an IRQ handler or from the audio chunk producer.
* **`FF_USE_MKFS = 0`** (`ffconf.h:33`) — you cannot format a card from Circle; prepare FAT partitions on a PC.
* **`FF_FS_EXFAT = 0`** and `FF_LBA64 = 0` (`ffconf.h:218, 246`) — exFAT cards and 64-bit-LBA/GPT volumes are not supported. Use FAT32. Likewise `CPartitionManager` explicitly skips extended (`0x05`/`0x0F`) and EFI (`0xEF`) partitions (`lib/fs/partitionmanager.cpp:110–118`).
* **`CFATFileSystem` root directory listing is 8.3 only** and has no subdirectory API (`fatfs.h:85,95`; `fsdef.h:27`).
* **`CKernel` member order is not arbitrary** (`sample/05-usbsimple/kernel.h:53`): *"do not change this order"*.

---

## Appendix A — Verified API quick reference

| Need | Exact symbol | Header |
|---|---|---|
| app entry | `int main (void)`, `halt()`, `reboot()`, `EXIT_HALT`, `EXIT_REBOOT`, `EXIT_POWER_OFF` | `circle/startup.h` |
| shutdown enum | `TShutdownMode { ShutdownNone, ShutdownHalt, ShutdownReboot }` | app `kernel.h` |
| Act LED | `CActLED::On/Off/Blink(n,on,off)`, `CActLED::Get()` | `circle/actled.h:35–40` |
| console | `CScreenDevice (w,h,font,flags,display)`, `Initialize`, `Resize`, `GetWidth/Height/Columns/Rows`, `GetFrameBuffer`, `Write`, `SetPixel`, `GetPixel`, `Rotor`, `SetCursorBlock`, `Update` | `circle/screen.h:139–201` |
| framebuffer | `CBcmFrameBuffer (w,h,depth,vw,vh,display,doublebuf)`, `Initialize`, `SetPixel`, `SetArea`, `SetVirtualOffset`, `WaitForVerticalSync`, `GetBuffer/GetPitch/GetSize/GetWidth/GetHeight/GetVirtWidth/GetVirtHeight/GetDepth`, `SetPalette/SetPalette32/UpdatePalette`, `SetBacklightBrightness`, `GetNumDisplays` | `circle/bcmframebuffer.h:44–81` |
| 2D layer | `C2DGraphics (w,h,bVSync,display)`, `Initialize`, `ClearScreen`, `DrawRect(Outline)`, `DrawLine`, `DrawCircle(Outline)`, `DrawImage(Transparent|Rect|RectTransparent)`, `DrawText`, `UpdateDisplay`, `GetBuffer` | `circle/2dgraphics.h:78–221` |
| sound base | `CSoundBaseDevice`, `Setup`, `AllocateQueue`, `AllocateQueueFrames`, `SetWriteFormat`, `Write`, `GetQueueSizeFrames`, `GetQueueFramesAvail`, `RegisterNeedDataCallback`, `GetChunk`, `ConvertIEC958Sample`, `Start/Cancel/IsActive/Flush`, `GetController`, `AreChannelsSwapped` | `circle/sound/soundbasedevice.h:67–234` |
| PWM sound | `CPWMSoundBaseDevice (pInterrupt, rate=44100, chunk=2048, MSMode=TRUE)` | `circle/sound/pwmsoundbasedevice.h:42` (+ `-rp1.h:38`) |
| I2S sound | `CI2SSoundBaseDevice (pInterrupt, rate=192000, chunk=8192, slave, pI2C, addr, mode, hwch)` | `circle/sound/i2ssoundbasedevice.h:58` (+ `-rp1.h:51`) |
| HDMI sound | `CHDMISoundBaseDevice (pInterrupt, rate=48000, chunk=3840)` / `(rate)`; `IsWritable`, `WriteSample` | `circle/sound/hdmisoundbasedevice.h:51–82` |
| USB sound | `CUSBSoundBaseDevice (rate=48000, mode=TXOnly, device=0)` | `circle/sound/usbsoundbasedevice.h:45` |
| USB host | `CUSBHCIDevice` (alias), `Initialize (bScanDevices=TRUE)`, `UpdatePlugAndPlay()`, `GetDescriptor`, `ControlMessage`, `Transfer` | `circle/usb/usbhcidevice.h:26–35`, `usbcontroller.h:31–36`, `usbhostcontroller.h:46–79` |
| keyboard | `CUSBKeyboardDevice::RegisterKeyStatusHandlerRaw (TKeyStatusHandlerRaw*, bMixedMode=FALSE)`, `RegisterKeyPressedHandler`, `RegisterShutdownHandler`, `SetLEDs`, `UpdateLEDs`, `GetLEDStatus`, `UnregisterKeyStatusHandlerRaw` | `circle/usb/usbkeyboard.h:31–63` |
| gamepad | `CUSBGamePadDevice::RegisterStatusHandler`, `GetInitialState`, `GetReport`, `GetProperties`, `SetLEDMode`, `SetRumbleMode`; `TGamePadState` | `circle/usb/usbgamepad.h:144–227` |
| USB storage | `CUSBBulkOnlyMassStorageDevice (pFunction)`, `Configure`, `Read`, `Write`, `Seek`, `GetSize`, `GetCapacity` | `circle/usb/usbmassdevice.h:35–49` |
| generic device | `CDevice::Read/Write/Seek/GetSize/IOCtl`, `DEVICE_IOCTL_SYNC`, `RegisterRemovedHandler` | `circle/device.h:53–90` |
| device names | `CDeviceNameService::Get()->GetDevice (name, bBlockDevice)`, `EnumerateDevices`, `ListDevices` | `circle/devicenameservice.h` / `lib/devicenameservice.cpp:137–251` |
| internal FAT | `CFATFileSystem::Mount/UnMount/Synchronize/RootFindFirst/RootFindNext/FileOpen/FileCreate/FileClose/FileRead/FileWrite/FileDelete` | `circle/fs/fat/fatfs.h:59–155` |
| FatFs | `f_mount`, `f_open`, `f_read`, `f_write`, `f_lseek`, `f_sync`, `f_close`, `f_opendir`, `f_readdir`, `f_findfirst`, `f_findnext`, `f_stat`, `f_getfree`, `f_unlink`, `f_mkdir`, `FR_OK`, `FIL`, `FILINFO`, `DIR`, `FA_READ`, `FA_WRITE`, `FA_CREATE_ALWAYS`, `FA_OPEN_ALWAYS`, `FA_OPEN_EXISTING` | `addon/fatfs/ff.h` |
| GPIO | `CGPIOPin (pin, mode, pManager)`, `AssignPin`, `SetMode (mode, bInitPin=TRUE)`, `SetPullMode`, `Write`, `Read`, `Invert`, `ConnectInterrupt (handler, param, bAutoAck=TRUE)`, `DisconnectInterrupt`, `EnableInterrupt (TGPIOInterrupt)`, `EnableInterrupt2`, `AcknowledgeInterrupt`, `SetModeAll`, `WriteAll`, `ReadAll` | `circle/gpiopin.h:96–154` (+ `gpiopin2712.h:54–89`) |
| GPIO manager | `CGPIOManager (pInterrupt)`, `Initialize()` | `circle/gpiomanager.h:30–33` |
| GPIO clock | `CGPIOClock (TGPIOClock, source)`, `Start (divI, divF, MASH)`, `StartRate (hz)`, `Stop` | `circle/gpioclock.h:52–64` |
| timer | `CTimer (pInterrupt)`, `Initialize`, `GetClockTicks`, `GetClockTicks64`, `CLOCKHZ`, `GetTicks`, `GetUptime`, `GetTime`, `StartKernelTimer`, `CancelKernelTimer`, `MsDelay`, `usDelay`, `SimpleMsDelay`, `SimpleusDelay`, `RegisterPeriodicHandler`, `CTimer::Get` | `circle/timer.h:47–152` |
| user timer | `CUserTimer (pInterrupt, handler, param, bUseFIQ)`, `Initialize`, `Start (micros)`, `Stop`, `USER_CLOCKHZ` | `circle/usertimer.h:26–54` |
| interrupts | `CInterruptSystem`, `Initialize`, `ConnectIRQ (nIRQ, TIRQHandler*, param)`, `DisconnectIRQ`, `ConnectFIQ`, `DisconnectFIQ`, `EnableIRQ/DisableIRQ/EnableFIQ/DisableFIQ`, `Get` | `circle/interrupt.h:27–53` |
| critical section | `TASK_LEVEL`/`IRQ_LEVEL`/`FIQ_LEVEL`, `EnterCritical`, `LeaveCritical`, `DMA_BUFFER` | `circle/synchronize.h:37–56,156` |
| scheduler | `CScheduler::Yield/Sleep/MsSleep/usSleep/GetCurrentTask/GetTask/RegisterTaskSwitchHandler/RegisterTaskTerminationHandler/SuspendNewTasks/ResumeNewTasks/EnumerateTasks/ListTasks/Get/IsActive` | `circle/sched/scheduler.h:41–108` |
| task | `CTask (stackSize=TASK_STACK_SIZE, suspended=FALSE)`, `Run`, `Start/Suspend/Resume/Terminate/WaitForTermination/SetName/GetName/SetUserData/GetUserData/GetStack` | `circle/sched/task.h:43–105` |
| multicore | `CMultiCoreSupport (pMemorySystem)`, `Initialize`, `Run (nCore)`, `SendIPI`, `HaltAll`, `ThisCore`, needs `ARM_ALLOW_MULTI_CORE` | `circle/multicore.h:40–88` |
| machine info | `CMachineInfo::Get()->GetMachineModel/GetMachineName/GetSoCType/GetSoCStepping/GetRAMSize/GetClockRate/GetGPIOPin/GetGPIOClockSourceRate/GetDevice/ArePWMChannelsSwapped` | `circle/machineinfo.h:96–127` |
| memory | `CMemorySystem::HeapAllocate (size, HEAP_LOW|HEAP_HIGH|HEAP_ANY|HEAP_DMA30)`, `HeapFree`, `GetHeapFreeSpace`, `PageAllocate/PageFree`, `GetLowMemSize/GetHighMemSize`; `new (HEAP_x) T`, `malloc/memalign/calloc/realloc/palloc/pfree` | `circle/memory.h:74–173`, `circle/new.h:27–32`, `circle/alloc.h:29–37` |
| options | `CKernelOptions::Get()->GetWidth/GetHeight/GetLogDevice/GetLogLevel/GetKeyMap/GetUSBPowerDelay/GetUSBFullSpeed/GetUSBBoost/GetUSBIgnore/GetUSBSoundChannels/GetSoundDevice/GetSoundOption/GetCPUSpeed/GetSoCMaxTemp/GetGPIOFanPin/GetTouchScreen/GetBacklight` + `GetAppOptionString/Decimal/SignedDecimal` | `circle/koptions.h:27–70` |

### Appendix B — Device name strings you will actually look up

| Name | Kind | Registered by |
|---|---|---|
| `"tty1"`, `"tty2"` | char device (`FALSE`) | `CScreenDevice` (`lib/screen.cpp:99–102`) |
| `"ttyS1"` … (`"ttyS11"` on Pi 5) | char device | `CSerialDevice`; see `doc/cmdline.txt:9–13` |
| `"ukbd1"` | char device | `CUSBKeyboardDevice` (`lib/usb/usbkeyboard.cpp:91`) |
| `"mouse1"` | char device | `CMouseDevice` (`lib/input/mouse.cpp:27`); note it is **not** `"umouse1"` — `CUSBMouseDevice` does not register a name itself (`lib/usb/usbmouse.cpp` has no `AddDevice`), which is why `sample/10-usbmouse/kernel.cpp:153` looks up `"mouse1"` |
| `"upad1"` | char device | `CUSBGamePadDevice` (`lib/usb/usbgamepad.cpp:69`) |
| `"uhub1"` | char device | `CUSBStandardHub` (`lib/usb/usbstandardhub.cpp:179`) |
| `"umsd1"` | **block device** | `CUSBBulkOnlyMassStorageDevice` (`lib/usb/usbmassdevice.cpp:407,420`) |
| `"umsd1-1"`, `"umsd1-2"` … | **block device** | `CPartitionManager` (`lib/fs/partitionmanager.cpp:126–127`) |
| `"ufd1"` | **block device** | `CUSBFloppyDiskDevice` (`lib/usb/usbfloppydevice.cpp:379`) |
| `"emmc1"`, `"emmc1-1"` … | **block device** | `CEMMCDevice` / `CPartitionManager` |
| `"emmc2"` | **block device** | second `CEMMCDevice` (Pi 4/CM4) |
| `"sndpwm"`, `"sndi2s"`, `"sndhdmi"` | char device | sound devices (`lib/sound/*.cpp`) |
| `"SD:"`, `"SD2:"`, `"USB:"`, `"USB2:"`, `"USB3:"`, `"FD:"`, `"NVME:"` | FatFs volume IDs | `addon/fatfs/ffconf.h:186` ↔ `diskio.cpp:28–37` |
