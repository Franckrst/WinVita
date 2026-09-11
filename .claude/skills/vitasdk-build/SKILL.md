---
name: vitasdk-build
description: Use when building homebrew for PS Vita — CMake + VitaSDK toolchain, producing a .vpk, or when the elf→velf→self→eboot→vpk pipeline fails with errors like C1-2613-2, undefined SceXxx symbols, or "invalid ELF".
---

# vitasdk-build

## Overview

Building for the Vita means compiling with `arm-vita-eabi-*` (GCC/binutils via
VitaSDK), producing a Vita ELF, converting it to VELF, wrapping it as a
self-signed FSELF, then packing it with a `param.sfo` into a `.vpk`. The CMake
toolchain file automates most of this via `vita_create_self` / `vita_create_vpk`.

!!! warning "VitaSDK is usually not on the PATH"
    Export it explicitly (`/usr/local/vitasdk` is the common location):
    `export VITASDK=/usr/local/vitasdk PATH="$VITASDK/bin:$PATH"`. Without it
    `objdump`/`nm`/`gcc` silently resolve to the host toolchain, and you lose
    exactly the proof-by-disassembly you were reaching for.

## When to use

- Setting up CMake for a new Vita homebrew project
- Adding a Vita target to an existing desktop C/C++ codebase
- Diagnosing a build that produces an ELF but no `.vpk`
- Errors: `C1-2613-2`, `unresolved reference to Sce…`,
  `vita-elf-create: … invalid`, `param.sfo not found`
- Wondering why a binary loads in an emulator but crashes on hardware

## The pipeline

```
main.cpp ──gcc──▶ prog.elf ──vita-elf-create──▶ prog.velf ──vita-make-fself UNSAFE──▶ eboot.bin
                                                                                        │
                                                        vita-mksfoex ──▶ param.sfo ─────┤
                                                                                        ▼
                                                                              vita-pack-vpk ▶ prog.vpk
```

`vita-elf-create` is the step people forget — its absence causes the infamous
**C1-2613-2** error on the device.

## Minimal CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)

if(DEFINED ENV{VITASDK})
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
else()
  message(FATAL_ERROR "VITASDK env var not set")
endif()

project(myport C CXX)

set(VITA_APP_NAME  "MyPort")
set(VITA_TITLEID   "MYPORT001")   # 9 chars, [A-Z0-9]
set(VITA_VERSION   "01.00")

add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME}
  vitaGL vitashark SceLibKernel_stub SceDisplay_stub
  SceGxm_stub SceCommonDialog_stub SceCtrl_stub ScePgf_stub
  m z)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME    ${VITA_APP_NAME}
  FILE    sce_sys/icon0.png     sce_sys/icon0.png
  FILE    sce_sys/livearea/contents/bg.png       sce_sys/livearea/contents/bg.png
  FILE    sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
```

!!! note "The CMake macros are the reference, not necessarily the shipping path"
    A port that compiles a large runtime plus a vendored dynarec often drives the
    build from shell scripts instead, keeping CMake for host-side tools only.
    Both are fine — what matters is that the elf→velf→self step is not skipped.

## Quick reference

| Symptom | Cause | Fix |
|---|---|---|
| `C1-2613-2` on boot | ELF not converted to VELF | Use `vita_create_self` (it calls `vita-elf-create`); never ship a raw `.elf` |
| `undefined reference to sceXxx` | Missing `*_stub` library | Add the matching `SceXxx_stub` to `target_link_libraries` |
| Boots in the emulator, crashes on HW | Alignment / uninitialised memory the emulator tolerates | `-Wall -Wextra`, and run with `-fsanitize=undefined` on desktop first |
| VPK installs but LiveArea empty | Missing `sce_sys/livearea/contents/template.xml` | Include the LiveArea assets in `vita_create_vpk` |
| `param.sfo` errors | Missing/bad TITLEID | 9 chars, `[A-Z0-9]`, unique per app |
| Links every rebuild | Not incremental | `-j$(nproc)`, and do not wipe the build dir between changes |

## Desktop parity

Keep the same sources compiling for desktop — it is what makes the
desktop-first debugging rule possible at all:

```cmake
if(NOT VITA)
  add_executable(${PROJECT_NAME}_desktop src/main.cpp)
  target_link_libraries(${PROJECT_NAME}_desktop SDL2 GL m z)
endif()
```

Gate Vita-specific code with `#ifdef __vita__` (defined by the toolchain).

## Common mistakes

- **Shipping the `.elf`** as `eboot.bin` — always run it through
  `vita-elf-create` (the toolchain macros do this for you).
- **Hardcoding a VitaSDK path** — always read `$VITASDK`; machines vary.
- **Skipping `UNSAFE`** in `vita_create_self` for homebrew that uses
  non-whitelisted syscalls — you get silent boot failures.
- **Editing a source while it is compiling** — you get a stale `.o` with a
  recent mtime, and the *next* build skips the recompile. `touch` the source
  before rebuilding, and verify the binary actually changed (`strings`, or the
  compile line in the log) before deploying.
- **Fresh checkout, VitaSDK not installed** — check `arm-vita-eabi-gcc --version`
  before diagnosing anything weirder.
- **Testing exclusively in an emulator** — it is more permissive than hardware.
