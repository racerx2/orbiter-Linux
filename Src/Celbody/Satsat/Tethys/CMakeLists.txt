# Copyright (c) Martin Schweiger
# Licensed under the MIT License

set(CELBODY "Tethys")

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
	Satsat
	${OrbiterTgt}
	Orbitersdk
)

target_include_directories(${CELBODY}
	PUBLIC ${CMAKE_SOURCE_DIR}/Orbitersdk/include
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(${CELBODY}
	${SATSAT_LIB}
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

add_custom_command(
	TARGET ${CELBODY} PRE_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/Config/ ${CMAKE_BINARY_DIR}/Config	
)

set_target_properties(${CELBODY}
	PROPERTIES
	FOLDER Celbody/Satsat
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
