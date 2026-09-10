# Licensed under the MIT License

add_library(DrawOrbits MODULE
	Draw.cpp
	Orbit.cpp
	Orbit.h
	Reference.cpp
	Reference.h
	Tools.cpp
	Tools.h
	Orbits.rc
)

# D3D9Client is a build-ordering dependency only; this module links
# ${OrbiterTgt} and Orbitersdk, not the client. The target does not
# exist when the client is not built.
if(TARGET D3D9Client)
	add_dependencies(DrawOrbits D3D9Client)
endif()

target_link_libraries(DrawOrbits
	${OrbiterTgt}
	Orbitersdk
)

target_include_directories(DrawOrbits
	PUBLIC ${INCLUDE_TARGET_DIR}
)

# Orbits.rc is listed above as a source: rc.exe compiles it into the module on
# Windows, and Linux ignores it, so every dialog it declares had no template
# and CreateDialogParam returned NULL without a word. See
# orbiter_module_resources in the top-level CMakeLists and
# Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(DrawOrbits ${CMAKE_CURRENT_SOURCE_DIR}/Orbits.rc)

set_target_properties(DrawOrbits
	PROPERTIES
	LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Modules/Plugin
	LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Modules/Plugin
	FOLDER OVP
)

install(TARGETS DrawOrbits
	LIBRARY	
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
) 
