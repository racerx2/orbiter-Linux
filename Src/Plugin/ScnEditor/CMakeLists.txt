# Copyright (c) Martin Schweiger
# Licensed under the MIT License

add_library(ScnEditor SHARED
	ScnEditor.cpp
	Editor.cpp
	Convert.cpp
	ScnEditor.rc
)

target_include_directories(ScnEditor
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
)

# ScnEditor.rc is listed above as a source, which rc.exe acts on and Linux
# ignores -- so all thirteen of this module's dialogs had no template and
# oapiOpenDialogEx returned NULL for every one of them, silently. This converts
# them the way rc.exe would; see the function in the top-level CMakeLists and
# Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(ScnEditor ${CMAKE_CURRENT_SOURCE_DIR}/ScnEditor.rc)

target_link_libraries(ScnEditor
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
	${ORBITER_DLGCTRL_LIB}
)

# comctl32 is the Windows common-control library. Its entry points -- the
# tree view, image lists and InitCommonControlsEx -- are provided by the shim
# in Src/Orbiter/Linux and resolved from the executable at load time, so there
# is nothing to link against here.
if(WIN32)
	target_link_libraries(ScnEditor comctl32.lib)
endif()

set_target_properties(ScnEditor
	PROPERTIES
	FOLDER Modules
)

# Deploy the API header file
add_custom_command(
	OUTPUT ${ORBITER_BINARY_SDK_DIR}/include/ScnEditorAPI.h
	COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_SDK_DIR}/include
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/ScnEditorAPI.h ${ORBITER_BINARY_SDK_DIR}/include
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ScnEditorAPI.h
)
add_custom_target(deploy_api
	DEPENDS ${ORBITER_BINARY_SDK_DIR}/include/ScnEditorAPI.h
)

add_dependencies(ScnEditor
	${OrbiterTgt}
	Orbitersdk
	DlgCtrl
	deploy_api
)

# Installation
install(TARGETS ScnEditor
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)
install(FILES ScnEditorAPI.h
	DESTINATION ${ORBITER_INSTALL_SDK_DIR}/include
)

install(FILES ScnEdit.png
	DESTINATION ${ORBITER_INSTALL_TEXTURES_DIR}/MenuInfoBar
)

# The build-tree copy of the menu icon; install() alone leaves a run from the
# build directory without it, and DynamicMenuBar then raises a "Cannot find menu
# item texture" notification. See Src/Plugin/FlightData/CMakeLists.txt for the
# full note, including why upstream has the same gap and why it only became
# visible once the ImGui dialogs were actually being drawn.
add_custom_command(TARGET ScnEditor POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_TEXTURES_DIR}/MenuInfoBar
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_CURRENT_SOURCE_DIR}/ScnEdit.png
		${ORBITER_BINARY_TEXTURES_DIR}/MenuInfoBar/
)


if(ORBITER_MAKE_DOC)
	add_subdirectory(Help)
endif()