# Copyright (c) Martin Schweiger
# Licensed under the MIT License

add_library(FlightData SHARED
	FlightData.cpp
)

target_include_directories(FlightData
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	PUBLIC ${MODULE_COMMON_DIR}
)

target_link_libraries(FlightData
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
	imgui
)

add_dependencies(FlightData
	${OrbiterTgt}
	Orbitersdk
)

set_target_properties(FlightData
	PROPERTIES
	FOLDER Modules
)

# Installation
install(TARGETS FlightData
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)

install(FILES FlightData.png
	DESTINATION ${ORBITER_INSTALL_TEXTURES_DIR}/MenuInfoBar
)

# AND THE SAME FILE INTO THE BUILD TREE, because that is where this port is run
# from and install() does not touch it.
#
# oapiRegisterCustomMenuCmd asks for "MenuInfoBar/FlightData.png" and
# DynamicMenuBar::MenuItem raises an OAPINOTIF_ERROR notification when
# oapiLoadTexture cannot find it. The icon lives in this plugin's own source
# directory, not under Textures/, so CopyData -- which copies Textures/ wholesale
# -- never sees it, and only `ninja install` puts it where the running program
# looks. Upstream has the same gap: the base commit's Textures/MenuInfoBar holds
# the twelve core icons and none of the plugin ones, so a Windows build-tree run
# is missing them too.
#
# It went unnoticed here for a different reason: nothing drew the notifications.
# Every ImGuiDialog, the notification renderer included, was being built into a
# discarded ImGui frame until orbiter_DrawImGuiDialogs was wired into UIHost's
# pump. See the porting notes.
add_custom_command(TARGET FlightData POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_TEXTURES_DIR}/MenuInfoBar
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_CURRENT_SOURCE_DIR}/FlightData.png
		${ORBITER_BINARY_TEXTURES_DIR}/MenuInfoBar/
)
