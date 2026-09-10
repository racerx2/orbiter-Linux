# Copyright (c) Martin Schweiger
# Licensed under the MIT License

add_library(ScriptMFD SHARED
	ScriptMFD.cpp
	ScriptMFD.rc
)

target_include_directories(ScriptMFD
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	PUBLIC ${MODULE_COMMON_DIR}
	PUBLIC ${ORBITER_SOURCE_ROOT_DIR}/Src/Module/LuaScript/LuaInterpreter
)

# ScriptMFD.rc is listed above as a source: rc.exe compiles it into the module
# on Windows, and Linux ignores it, so every dialog it declares had no template
# and CreateDialogParam returned NULL without a word. See
# orbiter_module_resources in the top-level CMakeLists and
# Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(ScriptMFD ${CMAKE_CURRENT_SOURCE_DIR}/ScriptMFD.rc)

target_link_libraries(ScriptMFD
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
	lua::lib
	${LUAINTERPRETER_LIB}
)

add_dependencies(ScriptMFD
	${OrbiterTgt}
	Orbitersdk	
	LuaInterpreter
)

set_target_properties(ScriptMFD
	PROPERTIES
	FOLDER Modules
)

# Installation
install(TARGETS ScriptMFD
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)
