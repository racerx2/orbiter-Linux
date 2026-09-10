# Copyright (c) Martin Schweiger
# Licensed under the MIT License

add_library(TrackIR SHARED
	TrackIR.cpp
	TrackIR.rc
	TrackIRconfig.cpp
	NPClientWraps.cpp
)

target_include_directories(TrackIR
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	PUBLIC ${MODULE_COMMON_DIR}
)

# TrackIR.rc is listed above as a source: rc.exe compiles it into the module on
# Windows, and Linux ignores it, so all five of this module's dialogs -- the
# whole of TrackIRconfig.cpp's Launchpad page -- had no template and
# CreateDialogParam returned NULL without a word. See orbiter_module_resources
# in the top-level CMakeLists and Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(TrackIR ${CMAKE_CURRENT_SOURCE_DIR}/TrackIR.rc)

target_link_libraries(TrackIR
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

# UxTheme is the Windows visual-styles library. Its entry points are provided
# by the shim in Src/Orbiter/Linux and resolved from the executable at load
# time, so there is nothing to link against here.
if(WIN32)
	target_link_libraries(TrackIR UxTheme.lib)
endif()

add_dependencies(TrackIR
	${OrbiterTgt}
	Orbitersdk
)

set_target_properties(TrackIR
	PROPERTIES
	FOLDER Modules
)

# Installation
install(TARGETS TrackIR
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)
