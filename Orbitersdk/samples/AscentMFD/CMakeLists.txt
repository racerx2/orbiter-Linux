# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# The target directory for plugin DLLs
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_PLUGIN_DIR})

add_library(AscentMFD MODULE
	AscentMFD.cpp
	AscentMFD.rc
)

add_dependencies(AscentMFD
	${OrbiterTgt}
	Orbitersdk
)

target_link_libraries(AscentMFD
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

target_include_directories(AscentMFD
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
)

# AscentMFD.rc is listed above as a source: rc.exe compiles it into the module
# on Windows, and Linux ignores it, so every dialog it declares had no template
# and CreateDialogParam returned NULL without a word. See
# orbiter_module_resources in the top-level CMakeLists and
# Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(AscentMFD ${CMAKE_CURRENT_SOURCE_DIR}/AscentMFD.rc)

set_target_properties(AscentMFD
	PROPERTIES
	LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Modules/Plugin
	LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Modules/Plugin
	FOLDER Modules
)

install(TARGETS AscentMFD
	LIBRARY	
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
) 
