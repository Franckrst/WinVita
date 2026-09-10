cmake_minimum_required(VERSION 3.16)

project(winx86
  VERSION 0.1.0
  DESCRIPTION "Generic Windows PE32/Win32 x86->ARMv7 execution engine for PS Vita"
  LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# NOTE: this builds only the host-portable slice (PE32 loader + import
# bridge), for desktop compile checks and IDE support. It does NOT build
# libwinx86.a itself: that needs the ARM/Vita cross toolchains and the
# Box86-derived dynarec's ARM-specific internals, and is built by build.sh
# (qemu-arm/Linux target) or TARGET=vita build.sh (real PS Vita target). See
# README.md.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra)

add_library(winx86_core STATIC
  src/runtime/pe_image.cpp
  src/runtime/bridge.cpp
  src/runtime/win32_shims_shell32.cpp
  src/runtime/win32_shims_advapi32.cpp
  src/runtime/win32_shims_user32.cpp
  src/runtime/win32_shims_misc.cpp)
target_include_directories(winx86_core PUBLIC src)

# -- Host-side PE inspection tool (standalone, project-agnostic) --------------
add_executable(pe_analyze tools/pe_analyze.cpp)
