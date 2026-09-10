# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Core library to go to the Orbiter main directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR})


# On ELF a shared object is a LIBRARY artifact, not a RUNTIME one, so
# CMAKE_RUNTIME_OUTPUT_DIRECTORY alone does not place it -- the module would be
# left in its build subdirectory and Orbiter's scans of Modules/ would find
# nothing. Mirroring the directory here puts .so files where .dll files go on
# Windows.
#
# The prefix is cleared for the same reason: Orbiter refers to a module by bare
# name ("Sun", "ScnEditor"), so it must be Sun.so rather than libSun.so.
if(NOT WIN32)
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR})
	set(CMAKE_SHARED_LIBRARY_PREFIX "")
	set(CMAKE_SHARED_MODULE_PREFIX "")
endif()

add_library(LuaInline SHARED
	LuaInline.cpp
)

target_include_directories(LuaInline
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	PUBLIC ${ORBITER_SOURCE_ROOT_DIR}/Src/Module/LuaScript/LuaInterpreter
)

target_link_libraries(LuaInline
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
	LuaInterpreter
	lua::lib
)

add_dependencies(LuaInline
	${OrbiterTgt}
	Orbitersdk
	LuaInterpreter
)

set_target_properties(LuaInline
	PROPERTIES
	FOLDER Modules/Lua
)

# Installation
install(TARGETS LuaInline
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}
)
