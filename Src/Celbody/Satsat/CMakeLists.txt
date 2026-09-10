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

add_library(Satsat SHARED
	Satsat.cpp
	Tass17.cpp
)

set_target_properties(Satsat
	PROPERTIES
	FOLDER Celbody/Satsat
)

add_dependencies(Satsat
	${OrbiterTgt}
	Orbitersdk
)

target_include_directories(Satsat
	PUBLIC ${CMAKE_SOURCE_DIR}/Orbitersdk/include
)

target_link_libraries(Satsat
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

add_custom_command(
	TARGET Satsat PRE_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Config/Saturn/Data
	COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/tass17.dat ${CMAKE_BINARY_DIR}/Config/Saturn/Data/
)

set(SATSAT_LIB $<TARGET_LINKER_FILE:Satsat>)


# Linked by the moon modules that take their ephemeris from this one. On ELF
# link the TARGET, not a path to its output file -- see the long note in
# Src/Celbody/Vsop87/CMakeLists.txt. In short: on a FIRST configure the file
# does not exist yet, CMake cannot read a SONAME from it, and it reduces the
# full path to -L<dir> -lSatsat, which searches for libSatsat.so; the module is
# deliberately Satsat.so with no prefix because Orbiter names modules bare.
# The target form uses the target's SONAME and records NEEDED [Satsat.so].
# The rpath lets the loader find it -- a moon lives in Modules/Celbody and
# Satsat.so in Modules, one directory up.
if(NOT WIN32)
	set(SATSAT_LIB Satsat)
	set(CMAKE_INSTALL_RPATH "$ORIGIN/..;$ORIGIN")
	set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
endif()

add_subdirectory(Mimas)
add_subdirectory(Enceladus)
add_subdirectory(Tethys)
add_subdirectory(Dione)
add_subdirectory(Rhea)
add_subdirectory(Titan)
add_subdirectory(Hyperion)
add_subdirectory(Iapetus)

# Installation
install(TARGETS
	Satsat
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_MODULE_DIR}
)
install(FILES tass17.dat
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Config/Saturn/Data
)
