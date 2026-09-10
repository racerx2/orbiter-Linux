# Copyright (c) Martin Schweiger
# Licensed under the MIT License

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Modules)


# See the note in Src/Plugin/CMakeLists.txt: on ELF a shared object is a
# LIBRARY artifact, so the runtime directory alone does not place it, and the
# lib prefix must be cleared because Orbiter names modules bare.
if(NOT WIN32)
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Modules)
	set(CMAKE_SHARED_LIBRARY_PREFIX "")
	set(CMAKE_SHARED_MODULE_PREFIX "")
endif()

add_library(Galsat SHARED
	Galsat.cpp
	Lieske.cpp
)

set_target_properties(Galsat
	PROPERTIES
	FOLDER Celbody/Galsat
)

add_dependencies(Galsat
	${OrbiterTgt}
	Orbitersdk
)

target_include_directories(Galsat
	PUBLIC ${CMAKE_SOURCE_DIR}/Orbitersdk/include
)

target_link_libraries(Galsat
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

add_custom_command(
	TARGET Galsat PRE_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Config/Jupiter/Data
	COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/ephem_e15.dat ${CMAKE_BINARY_DIR}/Config/Jupiter/Data/
)

set(GALSAT_LIB $<TARGET_LINKER_FILE:Galsat>)


# Linked by the moon modules that take their ephemeris from this one. On ELF
# link the TARGET, not a path to its output file -- see the long note in
# Src/Celbody/Vsop87/CMakeLists.txt. In short: on a FIRST configure the file
# does not exist yet, CMake cannot read a SONAME from it, and it reduces the
# full path to -L<dir> -lGalsat, which searches for libGalsat.so; the module is
# deliberately Galsat.so with no prefix because Orbiter names modules bare.
# The target form uses the target's SONAME and records NEEDED [Galsat.so].
# The rpath lets the loader find it -- a moon lives in Modules/Celbody and
# Galsat.so in Modules, one directory up.
if(NOT WIN32)
	set(GALSAT_LIB Galsat)
	set(CMAKE_INSTALL_RPATH "$ORIGIN/..;$ORIGIN")
	set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
endif()

add_subdirectory(Io)
add_subdirectory(Europa)
add_subdirectory(Ganymede)
add_subdirectory(Callisto)

# Installation
install(TARGETS
	Galsat
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_MODULE_DIR}
)
install(FILES ephem_e15.dat
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Config/Jupiter/Data
)
