# Copyright (c) Martin Schweiger
# Licensed under the MIT License

set(CELBODY "Saturn")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Modules/Celbody)


# See the note in Src/Plugin/CMakeLists.txt: on ELF a shared object is a
# LIBRARY artifact, so the runtime directory alone does not place it, and the
# lib prefix must be cleared because Orbiter names modules bare.
if(NOT WIN32)
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Modules/Celbody)
	set(CMAKE_SHARED_LIBRARY_PREFIX "")
	set(CMAKE_SHARED_MODULE_PREFIX "")
endif()

add_library(${CELBODY} SHARED
	${CELBODY}.cpp
)

add_dependencies(${CELBODY}
	${OrbiterTgt}
	Orbitersdk
	Vsop87
	Satsat
)

target_include_directories(${CELBODY}
	PUBLIC ${CMAKE_SOURCE_DIR}/Orbitersdk/include
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(${CELBODY}
	${VSOP87_LIB}
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

# The Saturnian moon ephemeris. SATSAT_LIB is set in Src/Celbody/Satsat, a
# SIBLING directory, so it does not reach this scope -- hence the target
# expression written out here rather than the variable.
#
# $<TARGET_LINKER_FILE:Satsat> is right on Windows (the import library). On ELF
# it is a full path, which CMake reduces to -L<dir> -lSatsat on a first
# configure, when it cannot read an SONAME from a file that does not exist yet;
# libSatsat.so is then not found, because the module is deliberately Satsat.so
# with no prefix. Naming the target instead gives NEEDED [Satsat.so]. Full
# reasoning in Src/Celbody/Vsop87/CMakeLists.txt.
if(WIN32)
	target_link_libraries(${CELBODY} $<TARGET_LINKER_FILE:Satsat>)
else()
	target_link_libraries(${CELBODY} Satsat)
endif()

add_custom_command(
	TARGET ${CELBODY} PRE_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/Config/ ${CMAKE_BINARY_DIR}/Config	
)

add_custom_command(
	TARGET ${CELBODY} PRE_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Config/${CELBODY}/Data/
	COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/../Data/Vsop87B_sat.dat ${CMAKE_BINARY_DIR}/Config/${CELBODY}/Data/Vsop87B.dat
)

set_target_properties(${CELBODY}
	PROPERTIES
	FOLDER Celbody/Vsop87
)

#Installation
install(TARGETS
	${CELBODY}
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_CELBODY_DIR}
)
install(DIRECTORY Config
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}
)
install(FILES ../Data/Vsop87B_sat.dat
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Config/${CELBODY}/Data
	RENAME Vsop87B.dat
)
