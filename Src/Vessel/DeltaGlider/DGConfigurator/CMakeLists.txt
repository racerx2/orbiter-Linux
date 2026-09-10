# Copyright (c) Martin Schweiger
# Licensed under the MIT License

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_STARTUP_DIR})


# On ELF a shared object is a LIBRARY artifact, not a RUNTIME one, so
# CMAKE_RUNTIME_OUTPUT_DIRECTORY alone does not place it -- the module would be
# left in its build subdirectory and Orbiter's scans of Modules/ would find
# nothing. Mirroring the directory here puts .so files where .dll files go on
# Windows.
#
# The prefix is cleared for the same reason: Orbiter refers to a module by bare
# name ("Sun", "ScnEditor"), so it must be Sun.so rather than libSun.so.
if(NOT WIN32)
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ORBITER_BINARY_STARTUP_DIR})
	set(CMAKE_SHARED_LIBRARY_PREFIX "")
	set(CMAKE_SHARED_MODULE_PREFIX "")
endif()

add_library(DGConfigurator SHARED
	DGConfigurator.cpp
	DGConfigurator.rc
)

target_include_directories(DGConfigurator
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
)

# DGConfigurator.rc is listed above as a source: rc.exe compiles it into the
# module on Windows, and Linux ignores it. This module is a Launchpad
# configuration page and nothing else, so without its template it had no
# interface at all. See orbiter_module_resources in the top-level CMakeLists
# and Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(DGConfigurator ${CMAKE_CURRENT_SOURCE_DIR}/DGConfigurator.rc)

target_link_libraries(DGConfigurator
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

add_dependencies(DGConfigurator
	${OrbiterTgt}
	Orbitersdk
)

set_target_properties(DGConfigurator
	PROPERTIES
	FOLDER Vessels/DeltaGlider
)

# Installation
install(TARGETS DGConfigurator
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_MODULE_DIR}/Startup
)
