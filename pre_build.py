"""
Pre-build script: patches .esphome/build/.../src/CMakeLists.txt to include
Duke3D engine sources. ESPHome only copies flat files from component dirs,
not subdirectories, so the engine/ subtree is added here via absolute path.

Registered in esphome.yaml → platformio_options → extra_scripts.
"""
import os

Import("env")  # PlatformIO SCons env

# $PROJECT_DIR is the platformio.ini location.
# When run via esphome.yaml extra_scripts, that is .esphome/build/<name>/ (the build dir).
# The script may also be invoked from the project root in some configurations.
# Detect which case we're in by checking for src/CMakeLists.txt existence.
_pio_dir = env.subst("$PROJECT_DIR")


if os.path.exists(os.path.join(_pio_dir, "src", "CMakeLists.txt")):
    # Running from ESPHome build dir (.esphome/build/duke3d-matrix/)
    cmake_path = os.path.join(_pio_dir, "src", "CMakeLists.txt")
    real_root = os.path.normpath(os.path.join(_pio_dir, "..", "..", ".."))
else:
    # Fallback: assume $PROJECT_DIR is the project root
    cmake_path = os.path.join(
        _pio_dir, ".esphome", "build", "duke3d-matrix", "src", "CMakeLists.txt"
    )
    real_root = _pio_dir

engine_base = os.path.join(
    real_root, "components", "duke3d", "engine", "components"
).replace("\\", "/")

MARKER = "Duke3D engine sources"

PATCH = (
    "\n# Duke3D engine sources — referenced by absolute path since ESPHome only copies\n"
    "# flat files from the component dir, not subdirectories.\n"
    'set(DUKE3D_ENGINE_BASE "' + engine_base + '")\n'
    "file(GLOB_RECURSE engine_srcs\n"
    '    "${DUKE3D_ENGINE_BASE}/Engine/*.c"\n'
    '    "${DUKE3D_ENGINE_BASE}/Game/*.c"\n'
    '    "${DUKE3D_ENGINE_BASE}/SDL/*.c"\n'
    '    "${DUKE3D_ENGINE_BASE}/audiolib/*.c"\n'
    ")\n"
    "# dummy_multi.c conflicts with mmulti.c (both define numplayers/syncstate/etc.).\n"
    "# spi_lcd.c is replaced by esp32_hal.cpp (renders to HUB75 instead of SPI LCD).\n"
    'list(FILTER engine_srcs EXCLUDE REGEX ".*/dummy_multi\\\\.c$")\n'
    'list(FILTER engine_srcs EXCLUDE REGEX ".*/spi_lcd\\\\.c$")\n'
    'list(FILTER engine_srcs EXCLUDE REGEX ".*/midi/")\n'
    "# SDL_audio.c uses IDF 4 DAC-mode I2S APIs removed in IDF 5; replaced by stubs in sdl_stubs.c.\n"
    'list(FILTER engine_srcs EXCLUDE REGEX ".*/SDL_audio\\\\.c$")\n'
    "# SDL_event.c targets ODROID-GO GPIO hardware; replaced by stubs in sdl_stubs.c.\n"
    "# Demo playback mode (/dDEMO1.DMO) needs no real input events.\n"
    'list(FILTER engine_srcs EXCLUDE REGEX ".*/SDL_event\\\\.c$")\n'
    "# Audiolib: exclude DOS ISA hardware drivers, OPL2/MIDI music, and stubs that\n"
    "# include music.h -> duke3d.h (re-defines large globals, causes dup symbols).\n"
    'foreach(_excl adlibfx al_midi awe32 blaster debugio dma dpmi\n'
    '        gus gusmidi guswave irq leetimbr midi mpu401 music\n'
    '        nomusic pas16 sndscape sndsrc task_man gmtimbre myprint usrhooks)\n'
    '    list(FILTER engine_srcs EXCLUDE REGEX ".*/${_excl}\\\\.c$")\n'
    'endforeach()\n'
    "\n"
)

NEW_REGISTER = (
    "idf_component_register(SRCS ${app_sources} ${engine_srcs}\n"
    "    INCLUDE_DIRS\n"
    '        "${DUKE3D_ENGINE_BASE}/Engine"\n'
    '        "${DUKE3D_ENGINE_BASE}/Game"\n'
    '        "${DUKE3D_ENGINE_BASE}/SDL"\n'
    '        "${DUKE3D_ENGINE_BASE}/audiolib"\n'
    ")\n"
    "\n"
    "# network.c provides sendpacket/flushpackets/sendlogoff (routing to mmulti.c).\n"
    "# But it also defines callcommit, conflicting with mmulti.c STUB_NETWORKING stub.\n"
    "# Rename network.c's callcommit — mmulti.c's empty stub is used for single-player.\n"
    'set_source_files_properties(\n'
    '    "${DUKE3D_ENGINE_BASE}/Engine/network.c"\n'
    '    PROPERTIES COMPILE_FLAGS "-Dcallcommit=_network_callcommit_unused"\n'
    ")\n"
    "\n"
    "# Old 1990s C engine code triggers many warnings that ESP-IDF promotes to errors\n"
    "# via -Werror=xxx. target_compile_options is applied AFTER IDF flags, so these\n"
    "# -Wno-* flags win and prevent build failure on ancient code patterns.\n"
    "target_compile_options(${COMPONENT_LIB} PRIVATE\n"
    "    # Old C headers define variables (not extern-declare) — requires GCC's tentative\n"
    "    # definition merging behavior (default before GCC 10; must be explicit with GCC 13).\n"
    "    -fcommon\n"
    "    -Wno-error\n"
    "    -Wno-error=all\n"
    "    -Wno-cpp\n"
    "    -Wno-format\n"
    "    -Wno-misleading-indentation\n"
    "    -Wno-implicit-fallthrough\n"
    "    -Wno-uninitialized\n"
    "    -Wno-maybe-uninitialized\n"
    "    -Wno-int-conversion\n"
    "    -Wno-incompatible-pointer-types\n"
    "    -Wno-discarded-qualifiers\n"
    "    -Wno-overflow\n"
    "    -fno-strict-aliasing\n"
    "    # Make int32_t = int (same as engine's int32 typedef) to prevent\n"
    "    # \"conflicting types\" errors between int and long int (both 32-bit on Xtensa).\n"
    "    -D__INT32_TYPE__=int\n"
    "    -D__UINT32_TYPE__=unsigned\n"
    "    -Wno-builtin-macro-redefined\n"
    "    -Wno-array-parameter\n"
    "    -Wno-pointer-sign\n"
    "    -Wno-stringop-overflow\n"
    "    -Wno-char-subscripts\n"
    "    -Wno-address\n"
    "    -Wno-restrict\n"
    "    -Wno-stringop-truncation\n"
    "    -Wno-sizeof-array-div\n"
    "    -Wno-type-limits\n"
    "    -Wno-return-type\n"
    "    -Wno-shift-negative-value\n"
    "    -Wno-shift-overflow\n"
    "    -Wno-tautological-compare\n"
    "    -Wno-logical-op\n"
    "    -Wno-aggressive-loop-optimizations\n"
    ")"
)

ORIG_REGISTER = "idf_component_register(SRCS ${app_sources})"

if not os.path.exists(cmake_path):
    print("[duke3d pre_build] WARNING: CMakeLists.txt not found, skipping patch")
else:
    with open(cmake_path, "r") as f:
        content = f.read()
    if MARKER not in content:
        new_content = content.replace(ORIG_REGISTER, PATCH + NEW_REGISTER)
        if new_content != content:
            with open(cmake_path, "w") as f:
                f.write(new_content)
            print("[duke3d pre_build] Patched src/CMakeLists.txt with engine sources")
        else:
            print("[duke3d pre_build] WARNING: Could not find marker to patch CMakeLists.txt")
    else:
        print("[duke3d pre_build] CMakeLists.txt already patched")

# Patch build.ninja to remove --warn-common.  GNU ld 2.41 exits(1) when
# --warn-common is set and -fcommon is used for the old C engine code.
# ESP-IDF CMakeLists.txt adds --warn-common unconditionally; we strip it here.
# This runs before ninja so the patched flags are used for linking.
# When _pio_dir is the ESPHome build dir, ninja is at _pio_dir/.pioenvs/.../build.ninja.
# When _pio_dir is the project root (fallback), it is under .esphome/build/....
if os.path.isdir(os.path.join(_pio_dir, ".pioenvs")):
    ninja_path = os.path.join(_pio_dir, ".pioenvs", "duke3d-matrix", "build.ninja")
else:
    ninja_path = os.path.join(
        _pio_dir, ".esphome", "build", "duke3d-matrix",
        ".pioenvs", "duke3d-matrix", "build.ninja"
    )
if os.path.exists(ninja_path):
    with open(ninja_path, "r") as f:
        ninja_content = f.read()
    if "--warn-common" in ninja_content:
        with open(ninja_path, "w") as f:
            f.write(ninja_content.replace("-Wl,--warn-common", ""))
        print("[duke3d pre_build] Removed --warn-common from build.ninja")
    else:
        print("[duke3d pre_build] build.ninja already clean (no --warn-common)")
