# Copyright (c) Martin Schweiger
# Licensed under the MIT License

add_library(Notes SHARED
	Notes.cpp
)

target_include_directories(Notes
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	PUBLIC ${MODULE_COMMON_DIR}
)

target_link_libraries(Notes
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
	imgui
)

add_dependencies(Notes
	${OrbiterTgt}
	Orbitersdk
)

set_target_properties(Notes
	PROPERTIES
	FOLDER Modules
)

# Installation
install(TARGETS Notes
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)

install(FILES Notes.png
	DESTINATION ${ORBITER_INSTALL_TEXTURES_DIR}/MenuInfoBar
)

# The build-tree copy of the menu icon; install() alone leaves a run from the
# build directory without it, and DynamicMenuBar then raises a "Cannot find menu
# item texture" notification. See Src/Plugin/FlightData/CMakeLists.txt for the
# full note, including why upstream has the same gap and why it only became
# visible once the ImGui dialogs were actually being drawn.
add_custom_command(TARGET Notes POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_TEXTURES_DIR}/MenuInfoBar
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_CURRENT_SOURCE_DIR}/Notes.png
		${ORBITER_BINARY_TEXTURES_DIR}/MenuInfoBar/
)

