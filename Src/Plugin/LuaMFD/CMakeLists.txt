# Copyright (c) Martin Schweiger
# Licensed under the MIT License

add_library(LuaMFD SHARED
	LuaMFD.cpp
	LuaMFD.rc
	MfdInterpreter.cpp
)

target_include_directories(LuaMFD
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	PUBLIC ${MODULE_COMMON_DIR}
	PUBLIC ${ORBITER_SOURCE_ROOT_DIR}/Src/Module/LuaScript/LuaInterpreter
)

# LuaMFD.rc is listed above as a source: rc.exe compiles it into the module on
# Windows, and Linux ignores it, so every dialog it declares had no template
# and CreateDialogParam returned NULL without a word. See
# orbiter_module_resources in the top-level CMakeLists and
# Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(LuaMFD ${CMAKE_CURRENT_SOURCE_DIR}/LuaMFD.rc)

target_link_libraries(LuaMFD
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
	lua::lib
	${LUAINTERPRETER_LIB}
)

add_dependencies(LuaMFD
	${OrbiterTgt}
	Orbitersdk
	LuaInterpreter
)

set_target_properties(LuaMFD
	PROPERTIES
	FOLDER Modules
)

# Installation
install(TARGETS LuaMFD
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)
