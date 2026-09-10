Include(FetchContent)

if(NOT DEFINED ORBITER_BINARY_ROOT_DIR)
    message(FATAL_ERROR "ORBITER_BINARY_ROOT_DIR is not set")
endif(NOT DEFINED ORBITER_BINARY_ROOT_DIR)

add_subdirectory(Lua)
add_subdirectory(zlib)
add_subdirectory(imgui)

# VulkanSceneGraph backs the Linux graphics client. Windows has D3D9Client and
# needs none of this, and fetching it there would add a large dependency to a
# build that cannot use it.
if(NOT WIN32 AND ORBITER_BUILD_VULKANCLIENT)
	add_subdirectory(vsg)
endif()

# stb is only needed by the Linux imaging layer, which reimplements the Windows
# Imaging Component over it. The Windows build uses WIC itself and has no use
# for this, so it is not fetched there.
if(NOT WIN32)
	add_subdirectory(stb)
endif()

## LFS
add_library(lfs SHARED luafilesystem/src/lfs.c)
install(TARGETS lfs	DESTINATION ${ORBITER_BINARY_ROOT_DIR})
install(TARGETS lfs DESTINATION ${ORBITER_INSTALL_SDK_DIR}/Utils/)
target_include_directories(lfs PUBLIC Lua)
target_link_libraries(lfs lua)
# The .def is MSVC's export list and `/DEF:` is an MSVC linker flag. Handing
# it to GNU ld makes it try to OPEN the path as an object file:
#
#   cannot find /DEF:/home/.../luafilesystem/src/lfs.def: No such file or directory
#
# so liblfsd.so never linked, and because ninja stops at the first failure it
# also silently prevented everything after it in the build order from being
# rebuilt -- which is how a header change can appear to have no effect.
#
# Nothing is lost by dropping it here: lfs.def exports exactly one symbol,
# `luaopen_lfs`, and lfs.c already declares it LFS_EXPORT, which is default
# visibility in ELF. The .def has no ELF equivalent to translate to.
if(WIN32)
	SET_TARGET_PROPERTIES(lfs PROPERTIES LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/luafilesystem/src/lfs.def\"")
endif()

install(TARGETS lfs	DESTINATION ${ORBITER_BINARY_ROOT_DIR})

set_target_properties(lfs PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
)

add_custom_target(CopyLDoc ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/ldoc/ ${ORBITER_BINARY_ROOT_DIR}/packages/ldoc
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/Penlight/lua/ ${ORBITER_BINARY_ROOT_DIR}/
)
