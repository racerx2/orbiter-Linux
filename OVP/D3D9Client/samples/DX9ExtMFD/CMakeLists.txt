# Licensed under the MIT License

add_library(DX9ExtMFD MODULE
	ExtMFD.cpp
	MFDWindow.cpp
	MFDWindow.h
	ExtMFD.rc
)

# D3D9Client is a build-ordering dependency only; this module links
# ${OrbiterTgt} and Orbitersdk, not the client. The target does not
# exist when the client is not built.
if(TARGET D3D9Client)
	add_dependencies(DX9ExtMFD D3D9Client)
endif()

target_link_libraries(DX9ExtMFD
	${OrbiterTgt}
	Orbitersdk
)

target_include_directories(DX9ExtMFD
	PUBLIC ${INCLUDE_TARGET_DIR}
)

# ExtMFD.rc is listed above as a source: rc.exe compiles it into the module on
# Windows, and Linux ignores it, so every dialog it declares had no template
# and CreateDialogParam returned NULL without a word. See
# orbiter_module_resources in the top-level CMakeLists and
# Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(DX9ExtMFD ${CMAKE_CURRENT_SOURCE_DIR}/ExtMFD.rc)

set_target_properties(DX9ExtMFD
	PROPERTIES
	LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Modules/Plugin
	LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Modules/Plugin
	FOLDER OVP
)

install(TARGETS DX9ExtMFD
	LIBRARY	
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
) 
