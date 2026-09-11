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
  src/runtime/guest_atomics.cpp
  src/runtime/guest_sync.cpp
  src/runtime/guest_thread_ctx.cpp
  src/runtime/win32_shims_kernel32.cpp
  src/runtime/win32_shims_shell32.cpp
  src/runtime/win32_shims_advapi32.cpp
  src/runtime/win32_shims_user32.cpp
  src/runtime/win32_shims_misc.cpp
  src/runtime/win32_shims_psapi.cpp
  src/runtime/win32_shims_version.cpp
  src/runtime/win32_shims_gdi32.cpp
  src/runtime/win32_shims_window.cpp
  src/runtime/win32_shims_wsock32.cpp
  src/runtime/poll_gil.cpp
  src/runtime/net_nonblock.cpp
  src/runtime/guest_scratch.cpp
  src/platform/present_scale.cpp
  src/render/render_null.cpp)
target_include_directories(winx86_core PUBLIC src)

# -- Host-side PE inspection tool (standalone, project-agnostic) --------------
add_executable(pe_analyze tools/pe_analyze.cpp)

# -- Auto-controle de la mise a l'echelle de presentation ---------------------
# La couche de presentation des portages vit sous #ifdef __vita__ : l'oracle
# d'identite d'image du projet ne peut pas l'exercer hors console. Ce binaire
# compare la version generique a une transcription de la boucle d'origine,
# octet pour octet, et tourne sur bureau. Voir tools/present_scale_selftest.cpp.
add_executable(present_scale_selftest tools/present_scale_selftest.cpp)
target_link_libraries(present_scale_selftest PRIVATE winx86_core)
