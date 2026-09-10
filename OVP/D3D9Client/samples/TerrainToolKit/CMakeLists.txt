# Licensed under the MIT License

add_library(TerrainToolKit MODULE
	gcPropertyTree.h
	gcTableView.cpp
	QTree.h
	QTree.cpp	
	ToolKit.h
	ToolKit.cpp
	ToolKit.rc
	ExportImport.cpp
	Basics.cpp
	Layer.cpp
	resource.h
)

# D3D9Client is a build-ordering dependency only; this module links
# ${OrbiterTgt} and Orbitersdk, not the client. The target does not
# exist when the client is not built.
if(TARGET D3D9Client)
	add_dependencies(TerrainToolKit D3D9Client)
endif()

target_link_libraries(TerrainToolKit
	${OrbiterTgt}
	Orbitersdk
)

# Msimg32 provides TransparentBlt and Comctl32 the common controls. Both are
# Windows import libraries; on Linux those entry points come from the shim in
# Src/Orbiter/Linux, which is linked into the Orbiter executable and bound at
# dlopen time. Naming them here makes the link fail with -lMsimg32.lib.
if(WIN32)
	target_link_libraries(TerrainToolKit
		Msimg32.lib
		Comctl32.lib
	)
endif()

target_include_directories(TerrainToolKit
	PUBLIC ${INCLUDE_TARGET_DIR}
)

# ToolKit.rc is listed above as a source: rc.exe compiles it into the module on
# Windows, and Linux ignores it, so every dialog it declares had no template
# and CreateDialogParam returned NULL without a word. See
# orbiter_module_resources in the top-level CMakeLists and
# Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(TerrainToolKit ${CMAKE_CURRENT_SOURCE_DIR}/ToolKit.rc)

set_target_properties(TerrainToolKit
	PROPERTIES
	LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Modules/Plugin
	LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Modules/Plugin
	FOLDER OVP
)

install(TARGETS TerrainToolKit
	LIBRARY	
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
) 
