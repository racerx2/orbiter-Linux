# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Core library to go to the Orbiter main directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR})

# On ELF a shared object is a LIBRARY artifact, not a RUNTIME one, so
# CMAKE_RUNTIME_OUTPUT_DIRECTORY alone does not place it -- the module would be
# left in its build subdirectory and Orbiter's scans of Modules/ would find
# nothing. Mirroring the directory here puts .so files where .dll files go on
# Windows.
#
# The prefix is cleared for the same reason: Orbiter refers to a module by bare
# name ("Sun", "ScnEditor"), so it must be Sun.so rather than libSun.so.
if(NOT WIN32)
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR})
	set(CMAKE_SHARED_LIBRARY_PREFIX "")
	set(CMAKE_SHARED_MODULE_PREFIX "")
endif()

set(BUILD_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/out)

add_library(LuaInterpreter SHARED
        Interpreter.cpp
        lua_vessel_mtd.cpp
)

if(ORBITER_BUILD_XRSOUND)
  target_sources(LuaInterpreter PRIVATE lua_xrsound.cpp)
elseif(NOT WIN32)
  # Interpreter.cpp references Interpreter::xrsound_create_instance
  # unconditionally, but lua_xrsound.cpp -- which defines it -- was only
  # compiled when XRSound was on. With XRSound off that leaves
  # LuaInterpreter.so with an undefined symbol, and every module that links it
  # fails to dlopen:
  #
  #   LuaConsole.so / LuaMFD.so / ScriptMFD.so
  #     -> LuaInterpreter.so: undefined symbol:
  #        _ZN11Interpreter23xrsound_create_instanceEP9lua_State
  #
  # which ModuleTab::RefreshLists does not report -- it checks `if (hMod)` and
  # otherwise leaves its category buffer holding the previous entry's value.
  # That is why LuaMFD and ScriptMFD landed in Miscellaneous and LuaConsole
  # inherited AscentMFD's category.
  #
  # XRSound.h is the client-side API header: it declares the interface and
  # loads the engine at runtime, so it pulls in no irrKlang. Compiling this
  # translation unit therefore needs no XRSound build and no irrKlang SDK --
  # it just supplies the definition the interpreter already calls.
  target_sources(LuaInterpreter PRIVATE lua_xrsound.cpp)
  target_include_directories(LuaInterpreter
    PRIVATE ${ORBITER_SOURCE_ROOT_DIR}/Sound/XRSound/src)
endif()

target_include_directories(LuaInterpreter
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	PUBLIC ${ORBITER_BINARY_SDK_DIR}/include
	PUBLIC ${ORBITER_BINARY_SDK_DIR}/XRSound/
)

target_link_libraries(LuaInterpreter
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
	lua::lib
)

add_dependencies(LuaInterpreter
	${OrbiterTgt}
	Orbitersdk
)

# The library the Lua plugins link against is LUAINTERPRETER_LIB, and it is set
# at the TOP LEVEL -- see the note beside it in the root CMakeLists.txt. It used
# to be set here as a CACHE INTERNAL entry holding the full path
# ${ORBITER_BINARY_ROOT_DIR}/LuaInterpreter.so, which was wrong twice over: the
# cache entry was shadowed in every consumer by the normal variable the top
# level had already set, and the full path is exactly the form CMake reduces to
# -lLuaInterpreter on a first configure. Nothing sets it here now, so there is
# one authority for it rather than two that can drift apart.
#
# The rpath lets the loader find it at runtime -- the plugins live in
# Modules/Plugin and this one in the root.
if(NOT WIN32)
	set(CMAKE_INSTALL_RPATH "$ORIGIN/../..;$ORIGIN/..;$ORIGIN")
	set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
endif()

if(ORBITER_BUILD_D3D9CLIENT)
  add_dependencies(LuaInterpreter	D3D9Client D3D9Client_Interface)
endif()

if(ORBITER_BUILD_XRSOUND)
  add_dependencies(LuaInterpreter	XRSound_lib	XRSound_assets)
  target_link_libraries(LuaInterpreter XRSound_lib)
  target_compile_definitions(LuaInterpreter PUBLIC XRSOUND)
endif()

set_target_properties(LuaInterpreter
	PROPERTIES
	FOLDER Modules/Lua
)

# Copy library to its Orbitersdk/lib location so that external project integrated into the build can find it
add_custom_command(TARGET LuaInterpreter
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_SDK_DIR}/include
	COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_SDK_DIR}/lib/Lua
	COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/Interpreter.h ${ORBITER_BINARY_SDK_DIR}/include/
	# Not ${LUAINTERPRETER_LIB}: that is now a link item (a target name on ELF),
	# and cmake -E copy needs a file. This is the same expression the install()
	# rule at the bottom of this file uses for the same artifact -- the import
	# library on Windows, LuaInterpreter.so on ELF.
	COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_LINKER_FILE:LuaInterpreter> ${ORBITER_BINARY_SDK_DIR}/lib/Lua/
)

if(ORBITER_MAKE_DOC)
file(GLOB lua_in *.h *.cpp *.ld *.md *.lua)
add_custom_command(
	POST_BUILD
	DEPENDS ${lua_in} lfs CopyLDoc
	OUTPUT ${BUILD_OUT_DIR}/index.html
	COMMAND ${CMAKE_COMMAND} -E copy ${lua_in} ${CMAKE_CURRENT_BINARY_DIR}
	COMMAND ${ldoc} .
	COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/interpreter.hhp ${BUILD_OUT_DIR}
	JOB_POOL htmlhelp
)

add_custom_command(
	POST_BUILD
	OUTPUT ${ORBITER_BINARY_SDK_DIR}/doc/orbiter_lua.chm
	DEPENDS ${BUILD_OUT_DIR}/index.html
	WORKING_DIRECTORY ${BUILD_OUT_DIR}
	COMMAND ${HHC_CMD} interpreter.hhp
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ./interpreter.chm ${ORBITER_BINARY_SDK_DIR}/doc/orbiter_lua.chm
	JOB_POOL htmlhelp
)

add_custom_target(orbiter_lua
	DEPENDS ${ORBITER_BINARY_SDK_DIR}/doc/orbiter_lua.chm
)

add_dependencies(${OrbiterTgt}
	orbiter_lua
)
endif()

if(ORBITER_MAKE_DOC)
install(FILES ${ORBITER_BINARY_SDK_DIR}/doc/orbiter_lua.chm
	DESTINATION ${ORBITER_INSTALL_SDK_DIR}/doc
)
endif()

# Installation
install(TARGETS LuaInterpreter
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}
)
install(FILES $<TARGET_LINKER_FILE:LuaInterpreter>
	DESTINATION ${ORBITER_INSTALL_SDK_DIR}/lib/Lua
)
