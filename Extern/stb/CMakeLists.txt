# stb — single-header image decode and encode.
#
# Used by Src/Orbiter/Linux/WinCodec.cpp, which reimplements the Windows
# Imaging Component over it. WIC is COM and Windows-only, but the work it does
# is ordinary image decode/encode, and stb covers exactly that in two headers
# with no build of its own.
#
# Fetched rather than vendored, matching how imgui, lua and zlib are handled in
# this tree. Pinned to a commit rather than a branch so a build is reproducible
# -- stb has no releases, so a tag is not available to pin to instead.
#
# INTERFACE target: there is nothing to compile. The implementation is emitted
# into WinCodec.cpp by its STB_IMAGE_IMPLEMENTATION define, so only the include
# path is published here.

project(stb)

Include(FetchContent)

FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31
)
FetchContent_MakeAvailable(stb)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

add_library(stb::stb ALIAS stb)
