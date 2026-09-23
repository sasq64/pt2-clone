# Builds pt2clone_libretro, the ProTracker 2 clone as a libretro core.
#
# The tracker's own sources are compiled with no edits: the SDL2 they are
# linked against is sdl_shim.c here, and pt2_compat.h is force included to keep
# main(), chdir() and the crash handler out of the way.
#
# SDL2 is only needed for its headers; nothing links against the library.

cmake_minimum_required(VERSION 3.16)
project(pt2clone_libretro C)

set(PT2_CLONE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.."
    CACHE PATH "pt2-clone checkout to build the core out of")

if(NOT EXISTS "${PT2_CLONE_DIR}/src/pt2_main.c")
    message(FATAL_ERROR "no pt2-clone sources in ${PT2_CLONE_DIR}")
endif()

file(GLOB PT2_SOURCES
    "${PT2_CLONE_DIR}/src/*.c"
    "${PT2_CLONE_DIR}/src/modloaders/*.c"
    "${PT2_CLONE_DIR}/src/smploaders/*.c"
    "${PT2_CLONE_DIR}/src/gfx/*.c")

add_library(pt2clone_libretro MODULE
    ${PT2_SOURCES}
    pt2_libretro.c
    sdl_shim.c)

set_target_properties(pt2clone_libretro PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden)

target_include_directories(pt2clone_libretro PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PT2_CLONE_DIR}/src")

# `#include <SDL2/SDL.h>`, so the directory holding SDL2/ is what has to be on
# the include path -- not the one pkg-config points at.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
foreach(dir ${SDL2_INCLUDE_DIRS})
    get_filename_component(parent "${dir}" DIRECTORY)
    target_include_directories(pt2clone_libretro SYSTEM PRIVATE "${dir}" "${parent}")
endforeach()

target_compile_options(pt2clone_libretro PRIVATE -O3 -ffast-math)
target_compile_definitions(pt2clone_libretro PRIVATE _GNU_SOURCE)

if(NOT WIN32)
    target_compile_options(pt2clone_libretro PRIVATE -fPIC)
endif()

# _DEBUG is pt2-clone's own switch for the three things it does on Windows that
# a core may not: a process-wide low-level keyboard hook, an unhandled
# exception filter, and a chdir to the .exe's directory. NDEBUG beside it keeps
# the tracker's ASSERT() from turning into a real assert() in the bargain.
if(WIN32)
    target_compile_definitions(pt2clone_libretro PRIVATE _DEBUG NDEBUG)
endif()

# -include applies to the tracker's sources as well as to ours; both want
# SDL_MAIN_HANDLED, and nothing here defines a main() for the macro to rename.
target_compile_options(pt2clone_libretro PRIVATE
    "-include${CMAKE_CURRENT_SOURCE_DIR}/pt2_compat.h")

find_package(Threads REQUIRED)
target_link_libraries(pt2clone_libretro PRIVATE m Threads::Threads)

# Keeping the SDL2 replacement to ourselves, three ways of saying the same
# thing. Only ELF can have our calls resolved against a frontend's real libSDL2,
# so only ELF needs the version script; ld64 and PE both bind a library's calls
# to its own definitions, and there the hidden visibility the target is built
# with -- plus the default visibility libretro.h puts back on the entry points
# -- is the whole of it.
if(APPLE)
    set_target_properties(pt2clone_libretro PROPERTIES SUFFIX ".dylib")
elseif(WIN32)
    target_link_options(pt2clone_libretro PRIVATE -Wl,--exclude-all-symbols)
else()
    target_link_options(pt2clone_libretro PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T" -Wl,-Bsymbolic)
endif()
