# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# The target directory for plugin DLLs
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_PLUGIN_DIR})

add_library(DialogTemplate SHARED
	DialogTemplate.cpp
	DialogTemplate.rc
)

target_include_directories(DialogTemplate
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
)

# DialogTemplate.rc is listed above as a source: rc.exe compiles it into the
# module on Windows, and Linux ignores it. This module is nothing BUT a dialog
# -- it is the SDK's worked example of one -- so without its template it
# demonstrated an oapiOpenDialog that returns NULL. See
# orbiter_module_resources in the top-level CMakeLists and
# Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(DialogTemplate ${CMAKE_CURRENT_SOURCE_DIR}/DialogTemplate.rc)

target_link_libraries(DialogTemplate
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

add_dependencies(DialogTemplate
	${OrbiterTgt}
	Orbitersdk
)

set_target_properties(DialogTemplate
	PROPERTIES
	FOLDER SDKSamples
)
