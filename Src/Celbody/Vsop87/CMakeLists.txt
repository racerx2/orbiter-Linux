set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Modules)


# See the note in Src/Plugin/CMakeLists.txt: on ELF a shared object is a
# LIBRARY artifact, so the runtime directory alone does not place it, and the
# lib prefix must be cleared because Orbiter names modules bare.
if(NOT WIN32)
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Modules)
	set(CMAKE_SHARED_LIBRARY_PREFIX "")
	set(CMAKE_SHARED_MODULE_PREFIX "")
endif()

add_library(Vsop87 SHARED
	Vsop87.cpp
)

set_target_properties(Vsop87
	PROPERTIES
	FOLDER Celbody/Vsop87
)

add_dependencies(Vsop87
	${OrbiterTgt}
)

target_include_directories(Vsop87
	PUBLIC ${CMAKE_SOURCE_DIR}/Orbitersdk/include
)

target_link_libraries(Vsop87
	${ORBITER_LIB}
)

set(VSOP87_LIB $<TARGET_LINKER_FILE:Vsop87>)

# The per-body modules link against this one. On Windows that is an import
# library and the loader pulls Vsop87.dll in automatically.
#
# On ELF the equivalent is a NEEDED entry naming Vsop87.so, and the way to get
# one is to link the TARGET, not a path to its output file.
#
# A FULL PATH DOES NOT WORK ON A FIRST CONFIGURE, and this is worth stating
# because the tree carried the opposite claim for a while. CMake decides
# whether it may reduce a full-path item to -L<dir> -l<name> by reading the
# file's SONAME (cmComputeLinkInformation::CheckSharedLibNoSOName). On a fresh
# build directory Modules/Vsop87.so does not exist yet at GENERATE time, the
# read fails, CMake assumes there is no SONAME and emits
#     -L<dir> -lVsop87
# which searches for libVsop87.so -- and the module is deliberately Vsop87.so
# with no prefix, because Orbiter refers to modules by bare name. So:
#     cannot find -lVsop87: No such file or directory
# Once the file exists the same generate step keeps the full path, which is why
# a second cmake run "fixes" it and why an established build directory never
# showed the problem. Measured both ways with an isolated two-target project.
#
# Naming the target instead makes CMake use the target's own SONAME property,
# so it emits the path and records NEEDED [Vsop87.so]; it also creates the
# build-order edge that add_dependencies had to supply by hand.
#
# The rpath is what lets the loader find it at runtime -- a body lives in
# Modules/Celbody and Vsop87.so in Modules, one directory up.
if(NOT WIN32)
	set(VSOP87_LIB Vsop87)
	set(CMAKE_INSTALL_RPATH "$ORIGIN/..;$ORIGIN")
	set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
endif()

add_subdirectory(Sun)
add_subdirectory(Mercury)
add_subdirectory(Venus)
add_subdirectory(Earth)
add_subdirectory(Mars)
add_subdirectory(Jupiter)
add_subdirectory(Saturn)
add_subdirectory(Uranus)
add_subdirectory(Neptune)

# Installation
install(TARGETS
	Vsop87
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_MODULE_DIR}
)
