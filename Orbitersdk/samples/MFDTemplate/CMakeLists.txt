# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# The target directory for plugin DLLs
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_PLUGIN_DIR})

add_library(MFDTemplate SHARED
	MFDTemplate.cpp
	MFDTemplate.rc
)

target_include_directories(MFDTemplate
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
)

# MFDTemplate.rc is listed above as a source: rc.exe compiles it into the
# module on Windows, and Linux ignores it, so every dialog it declares had no
# template and CreateDialogParam returned NULL without a word. See
# orbiter_module_resources in the top-level CMakeLists and
# Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(MFDTemplate ${CMAKE_CURRENT_SOURCE_DIR}/MFDTemplate.rc)

target_link_libraries(MFDTemplate
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

add_dependencies(MFDTemplate
	${OrbiterTgt}
	Orbitersdk
)

set_target_properties(MFDTemplate
	PROPERTIES
	FOLDER SDKSamples
)
