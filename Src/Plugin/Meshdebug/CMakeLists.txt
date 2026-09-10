# Copyright (c) Martin Schweiger
# Licensed under the MIT License

add_library(Meshdebug SHARED
	Meshdebug.cpp
	Meshdebug.rc
)

target_include_directories(Meshdebug
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	PUBLIC ${MODULE_COMMON_DIR}
)

# Meshdebug.rc is listed above as a source: rc.exe compiles it into the module
# on Windows, and Linux ignores it. This plugin IS a dialog -- it has no other
# interface -- so without its template it loaded and showed nothing at all. See
# orbiter_module_resources in the top-level CMakeLists and
# Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(Meshdebug ${CMAKE_CURRENT_SOURCE_DIR}/Meshdebug.rc)

target_link_libraries(Meshdebug
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

add_dependencies(Meshdebug
	${OrbiterTgt}
	Orbitersdk
)

set_target_properties(Meshdebug
	PROPERTIES
	FOLDER Modules
)

# Installation
install(TARGETS Meshdebug
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)
