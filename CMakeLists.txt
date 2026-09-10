# Copyright (c) Martin Schweiger
# Licensed under the MIT License

cmake_minimum_required(VERSION 3.19)

# Set the project name
project (Orbiter VERSION 21.7.24)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

#Add /permissive if using C++20 or higher
#add_compile_options(/permissive)

# Some functions to simplify debugging CMake scripts
include(CMakePrintHelpers)

# Support for including runtime libraries in packages
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)

# Set the pools for compilers that can't run concurrently
set_property(GLOBAL PROPERTY JOB_POOLS latex=1 soffice=1 htmlhelp=1)

# Group projects in VS
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Locally defined cmake modules
set(CMAKE_MODULE_PATH
	${CMAKE_SOURCE_DIR}/cmake
	${CMAKE_MODULE_PATH}
)

if(MSVC AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(BUILD64 0)
	set(ARCH "x86")
  add_link_options("/LARGEADDRESSAWARE")
else()
	set(BUILD64 1)
	set(ARCH "x64")
endif()

# This is important for building using vs generator (jarmonik)
if (NOT ${CMAKE_GENERATOR} STREQUAL "Ninja")
	add_compile_options(/MP)
endif()

# Define directories and file paths for some common binaries
set(OVP_DIR "${CMAKE_SOURCE_DIR}/OVP")

set(ORBITER_SOURCE_ROOT_DIR ${CMAKE_SOURCE_DIR})
set(ORBITER_SOURCE_DIR ${ORBITER_SOURCE_ROOT_DIR}/Src/Orbiter)
set(ORBITER_SOURCE_MODULE_DIR ${ORBITER_SOURCE_ROOT_DIR}/Src/Plugin)
set(ORBITER_SOURCE_SDK_DIR ${ORBITER_SOURCE_ROOT_DIR}/Orbitersdk)
set(ORBITER_SOURCE_SDK_INCLUDE_DIR ${ORBITER_SOURCE_SDK_DIR}/include)
set(ORBITER_SOURCE_HTML_DIR ${ORBITER_SOURCE_ROOT_DIR}/Html)

set(ORBITER_BINARY_ROOT_DIR ${CMAKE_BINARY_DIR})
set(ORBITER_BINARY_CONFIG_DIR ${ORBITER_BINARY_ROOT_DIR}/Config)
set(ORBITER_BINARY_MESH_DIR ${ORBITER_BINARY_ROOT_DIR}/Meshes)
set(ORBITER_BINARY_MODULE_DIR ${ORBITER_BINARY_ROOT_DIR}/Modules)
set(ORBITER_BINARY_STARTUP_DIR ${ORBITER_BINARY_MODULE_DIR}/Startup)
set(ORBITER_BINARY_CELBODY_DIR ${ORBITER_BINARY_MODULE_DIR}/Celbody)
set(ORBITER_BINARY_PLUGIN_DIR ${ORBITER_BINARY_MODULE_DIR}/Plugin)
set(ORBITER_BINARY_VESSEL_DIR ${ORBITER_BINARY_MODULE_DIR})
set(ORBITER_BINARY_TEXTURES_DIR ${ORBITER_BINARY_ROOT_DIR}/Textures)
set(ORBITER_BINARY_TEXTURES2_DIR ${ORBITER_BINARY_ROOT_DIR}/Textures2)
set(ORBITER_BINARY_HTML_DIR ${ORBITER_BINARY_ROOT_DIR}/Html)
set(ORBITER_BINARY_DOC_DIR ${ORBITER_BINARY_ROOT_DIR}/Doc)
set(ORBITER_BINARY_SDK_DIR ${ORBITER_BINARY_ROOT_DIR}/Orbitersdk)

set(ORBITER_INSTALL_ROOT_DIR "Orbiter") # This path is relative to CMAKE_INSTALL_PREFIX
set(ORBITER_INSTALL_MESH_DIR ${ORBITER_INSTALL_ROOT_DIR}/Meshes)
set(ORBITER_INSTALL_MODULE_DIR ${ORBITER_INSTALL_ROOT_DIR}/Modules)
set(ORBITER_INSTALL_CELBODY_DIR ${ORBITER_INSTALL_MODULE_DIR}/Celbody)
set(ORBITER_INSTALL_PLUGIN_DIR ${ORBITER_INSTALL_MODULE_DIR}/Plugin)
set(ORBITER_INSTALL_TEXTURES_DIR ${ORBITER_INSTALL_ROOT_DIR}/Textures)
set(ORBITER_INSTALL_TEXTURES2_DIR ${ORBITER_INSTALL_ROOT_DIR}/Textures2)
set(ORBITER_INSTALL_HTML_DIR  ${ORBITER_INSTALL_ROOT_DIR}/Html)
set(ORBITER_INSTALL_DOC_DIR ${ORBITER_INSTALL_ROOT_DIR}/Doc)
set(ORBITER_INSTALL_UTILS_DIR ${ORBITER_INSTALL_ROOT_DIR}/Utils)
set(ORBITER_INSTALL_SDK_DIR ${ORBITER_INSTALL_ROOT_DIR}/Orbitersdk)

set(ORBITER_SDK_LIB $<TARGET_FILE:Orbitersdk>)
set(ORBITER_DLGCTRL_LIB $<TARGET_FILE:DlgCtrl>)
set(LUAINTERPRETER_LIB $<TARGET_LINKER_FILE:LuaInterpreter>)

# On ELF, name the TARGET rather than the file its build produces.
#
# $<TARGET_LINKER_FILE:LuaInterpreter> is right on Windows -- it resolves to
# the import library. On ELF it resolves to LuaInterpreter.so, a full path, and
# CMake is then free to reduce a full-path link item to -L<dir> -l<name>. It
# decides by reading the file's SONAME
# (cmComputeLinkInformation::CheckSharedLibNoSOName), and on a FIRST configure
# of a fresh build directory the file does not exist yet, so the read fails,
# CMake assumes no SONAME, and emits
#     -L<dir> -lLuaInterpreter
# which searches for libLuaInterpreter.so. The module is deliberately
# LuaInterpreter.so with no prefix, because Orbiter refers to modules by bare
# name, so the link fails:
#     cannot find -lLuaInterpreter: No such file or directory
#
# Once the file exists CMake keeps the full path, which is why a SECOND cmake
# run silently repairs the build and why no established build directory ever
# showed this. Measured both ways with an isolated two-target project; the
# target form emits the path and records NEEDED [LuaInterpreter.so].
#
# This variable is the single authority for all four consumers -- LuaConsole,
# LuaMFD, ScriptMFD and the Lua.Interpreter test. It is set HERE rather than in
# Src/Module/LuaScript/LuaInterpreter/CMakeLists.txt because a normal variable
# set at this scope is inherited by every subdirectory and would shadow a cache
# entry written later from a subdirectory.
if(NOT WIN32)
	set(LUAINTERPRETER_LIB LuaInterpreter)
endif()

set(EXTERN_DIR "${CMAKE_SOURCE_DIR}/Extern")

set(HTMLHELP_DIR "${EXTERN_DIR}/Htmlhelp")
set(HTMLHELP_INCLUDE_DIR ${HTMLHELP_DIR}/include)
set(HTMLHELP_LIB_DIR ${HTMLHELP_DIR}/lib-${ARCH})
set(HTMLHELP_LIB ${HTMLHELP_LIB_DIR}/Htmlhelp.Lib)

set(ORBITER_TOOL_MESHC "$<TARGET_FILE:meshc>")
set(ldoc $<TARGET_FILE:lua::exe> ${ORBITER_BINARY_ROOT_DIR}/packages/LDoc/ldoc.lua -v --multimodule)

find_package(OpenGL QUIET)
find_package(HTMLHelp)
find_package(LATEX)
find_package(MFC)

# We don't query Qt with find_package because we need the 64-bit version here
set(Qt5_x64_DIR
	"Qt5_x64_DIR_NOTFOUND"
	CACHE
	PATH
	"Qt5-x64 installation path."
)

if(MSVC)
  add_compile_options(/we4311 /DNOMINMAX)
  add_link_options("/NODEFAULTLIB:\"LIBCMTD\"")
else()
  set(CMAKE_BUILD_WITH_INSTALL_RPATH true)

  # Plugins live in Modules/Plugin but link libraries that sit at the root of
  # the build tree -- liblua.so above all. Without a run path reaching back up,
  # dlopen fails with
  #     liblua.so: cannot open shared object file: No such file or directory
  # and the failure is not loud: ModuleTab::RefreshLists checks `if (hMod)` and
  # otherwise leaves its category buffer untouched, so the plugin silently
  # inherits whatever the previous directory entry got. LuaConsole, LuaMFD and
  # ScriptMFD were landing in three different wrong categories because of this.
  #
  # $ORIGIN is resolved by the dynamic linker at load time, so this works from
  # the build tree and from an install without being an absolute path.
  set(CMAKE_INSTALL_RPATH "$ORIGIN:$ORIGIN/..:$ORIGIN/../..:$ORIGIN/../../..")

  # Force the Orbitersdk archive member holding DllMain into every module.
  #
  # A static archive contributes a member only when something already
  # references a symbol in it. Orbitersdk.cpp holds DllMain and the ELF
  # constructor that stands in for it, and nothing in a module references
  # either by name -- the dynamic linker finds the constructor by walking
  # .init_array, which only works if the object made it into the link at all.
  #
  # For modules that reference nothing else from that object the member was
  # dropped silently. Jupiter, Io, Europa, Ganymede, Callisto and the seven
  # Saturnian moons all take their symbols from Galsat.so / Satsat.so, so they
  # linked without DllMain, InitLib never ran for them, and they never
  # appeared in the log where Windows lists them.
  #
  # -u names a symbol the linker must resolve, which pulls in the member that
  # defines it. This cannot go into ORBITER_SDK_LIB: that variable is also
  # consumed as a plain file path by the post-build step that copies the
  # archive into Orbitersdk/lib, and a linker flag there made cmake -E copy
  # try to copy a file called "-Wl,--whole-archive".
  add_link_options(-Wl,-u,orbiter_module_attach)

  # This tree was written against MSVC, which permits constructs the C++
  # standard forbids -- e.g. Vecmat.h gives default arguments on a friend
  # declaration that is not a definition, which [dcl.fct.default]/4 prohibits
  # and GCC rejects outright.
  #
  # -fpermissive downgrades that class of error to a warning, so the sources
  # compile exactly as written and none of them need editing. The cost is real
  # and worth knowing: it relaxes conformance checking for every file in the
  # non-Windows build, so a genuine error of the same class would also be
  # demoted to a warning rather than stopping the build.
  add_compile_options(-fpermissive)

  # Every plugin links the Orbitersdk and DlgCtrl static libraries into a
  # shared object. On ELF that requires their objects to be position
  # independent, or the link fails with
  #     relocation R_X86_64_PC32 against undefined symbol `GImGui'
  #     can not be used when making a shared object; recompile with -fPIC
  # Windows has no such distinction, which is why the tree does not set it.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Copy data and binary assets from source to binary tree to provide a working Orbiter environment
add_custom_target(CopyData ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/BinAssets/ ${CMAKE_BINARY_DIR}
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Scenarios/ ${CMAKE_BINARY_DIR}/Scenarios
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Textures/ ${ORBITER_BINARY_TEXTURES_DIR}
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Meshes/ ${CMAKE_BINARY_DIR}/Meshes
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Script/ ${CMAKE_BINARY_DIR}/Script
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Config/ ${CMAKE_BINARY_DIR}/Config
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/GravityModels/ ${CMAKE_BINARY_DIR}/GravityModels
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Flights/ ${CMAKE_BINARY_DIR}/Flights
	# Module directories Orbiter scans at startup. On Windows these exist
	# because the installer lays them down; a build tree has only the ones
	# that happen to contain a built module, and several scans use a bare
	# directory_iterator which throws on a missing path rather than reporting
	# it. Creating them empty is what the Windows layout provides.
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Modules/Plugin
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Modules/Startup
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Modules/Server
	COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_SDK_DIR}/lib/Lua
)
set_target_properties(CopyData
	PROPERTIES
	FOLDER Data
)

if(NOT WIN32)
	# Case-insensitive aliases for the script tree.
	#
	# Scenarios and vessels name scripts in one case and the shipped folders
	# use another: the Delta-glider's ascent autopilot asks for "dg/aap" while
	# the directory is Script/DG. NTFS does not care; ext4 does, and the script
	# fails to load.
	#
	# The other two instances of this -- vessel .cfg files and mesh names --
	# are fixed properly, with a case-insensitive fallback at the single point
	# each lookup passes through (Vessel::OpenConfigFile, Config::MeshPath).
	# Scripts have no such point: run() builds the path inside oapi_init.lua
	# and Lua calls fopen directly, bypassing the shim entirely.
	#
	# So the aliases are made in the BUILD TREE only. The source tree is left
	# alone and the Windows build never runs this. The better fix is to give
	# the interpreter its own dofile that resolves case-insensitively, which
	# would also cover scripts named from a scenario at run time.
	add_custom_target(ScriptCaseAliases ALL
		COMMAND ${CMAKE_COMMAND}
		        -DSCRIPTDIR=${CMAKE_BINARY_DIR}/Script
		        -P ${CMAKE_SOURCE_DIR}/cmake/script_case_aliases.cmake
		DEPENDS CopyData
		COMMENT "Creating case-insensitive script directory aliases"
	)
	add_dependencies(ScriptCaseAliases CopyData)

	# CopyData IS NOT THE ONLY THING THAT POPULATES Script/, and depending on
	# it alone is a race that a fresh build directory loses.
	#
	# Two vessels deliver their own script folders from PRE_BUILD steps of
	# their own targets:
	#     Src/Vessel/DeltaGlider/CMakeLists.txt -> Script/DG
	#     Src/Vessel/ShuttleA/CMakeLists.txt    -> Script/ShuttleA
	# The alias pass globs Script/ once, so anything not there yet gets no
	# alias -- and on a first build it ran at step 586 of 1023, before either
	# vessel. It made five aliases (atlantis, challenges, demo, demos, tests)
	# and NOT dg or shuttlea, so the Delta-glider's ascent autopilot failed at
	# run time with
	#     cannot open ./Script/dg/aap.lua: No such file or directory
	# while an established build directory worked perfectly, because a
	# previous build had left the symlinks behind.
	#
	# If a future vessel copies a MIXED-CASE directory into Script/, it must
	# be named here too. The evidence is in the build output: the pass prints
	# one "script alias: <lower> -> <Name>" per link it creates, so a missing
	# line is the symptom.
	add_dependencies(ScriptCaseAliases DeltaGlider ShuttleA)
endif()

# =====================================================================================
# work around Html Help compiler bug returning error code

set(HHC_CMD ${CMAKE_CURRENT_BINARY_DIR}/cmake/hhc_fix.bat)
file(TO_NATIVE_PATH "${HTML_HELP_COMPILER}" HHC_PATH_W)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/hhc_fix.bat.in ${HHC_CMD})


# =====================================================================================
# work around pdftex returning an error because of doxygen bug

set(PDFTEX_CMD ${CMAKE_CURRENT_BINARY_DIR}/cmake/pdftex_fix.bat)
file(TO_NATIVE_PATH "${PDFLATEX_COMPILER}" PDFTEX_PATH_W)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/pdftex_fix.bat.in ${PDFTEX_CMD})


# =====================================================================================

set(OrbiterTgt Orbiter)

# The library a module links against to reach the core.
#
# On Windows, building Orbiter.exe also produces an import library
# (orbiter.lib), and every module links against it -- that is what
# TARGET_LINKER_FILE resolves to. ELF has no import libraries: linking a shared
# object against an executable fails outright with
#     invalid version 4 (max 0) / error adding symbols: bad value
# which is exactly what the celbody modules hit.
#
# The ELF model is the reverse. A module simply leaves the core's symbols
# undefined -- which is the default for a shared object -- and the dynamic
# linker binds them to the executable at dlopen time. That works because the
# Orbiter target is linked with -rdynamic, putting its symbols in the dynamic
# symbol table. So on non-Windows this is deliberately empty.
if(WIN32)
	set(ORBITER_LIB $<TARGET_LINKER_FILE:Orbiter>)
else()
	set(ORBITER_LIB "")
endif()

# =====================================================================================
# Configure Orbiter.cfg for externally installed planet textures

set(ORBITER_PLANET_TEXTURE_INSTALL_DIR
	${ORBITER_BINARY_ROOT_DIR}/Textures
	CACHE
	PATH
	"Planet texture installation directory (e.g. C:/Orbiter2016/Textures)"
)

if(DEFINED ENV{ORBITER_PLANET_TEXTURE_INSTALL_DIR})
  set(ORBITER_PLANET_TEXTURE_INSTALL_DIR $ENV{ORBITER_PLANET_TEXTURE_INSTALL_DIR})
  message(STATUS "Using environment: ORBITER_PLANET_TEXTURE_INSTALL_DIR=${ORBITER_PLANET_TEXTURE_INSTALL_DIR}")
endif()

string(COMPARE NOTEQUAL ${ORBITER_PLANET_TEXTURE_INSTALL_DIR} "${ORBITER_BINARY_ROOT_DIR}/Textures" CUSTOM_PLANET_TEXTURE_DIR)

if (${CUSTOM_PLANET_TEXTURE_DIR})
	string(REPLACE  "/" "\\" ORBITER_PLANET_TEXTURE_INSTALL_DIR_W "${ORBITER_PLANET_TEXTURE_INSTALL_DIR}")

	configure_file(${ORBITER_SOURCE_DIR}/Orbiter.cfg.in ${ORBITER_BINARY_ROOT_DIR}/Orbiter.cfg)
	install(FILES ${ORBITER_BINARY_ROOT_DIR}/Orbiter.cfg DESTINATION ${ORBITER_INSTALL_ROOT_DIR})
endif()


# =====================================================================================
# Find required and optional build tools

# D3D9Client is Direct3D 9, which has no native Linux implementation, so it
# cannot be built here and needs replacing by a new graphics client.
#
# XRSound is a different case: the irrKlang 1.6.0 64-bit archive carries
# bin/linux-gcc-64/libIrrKlang.so alongside the MSVC binaries, so the same
# download serves both platforms and nothing has to be built from source.
# Sound/XRSound/CMakeLists.txt now selects the right subdirectory, so it is on
# by default here too.
if(WIN32)
	set(ORBITER_DEFAULT_D3D9 ON)
else()
	set(ORBITER_DEFAULT_D3D9 OFF)
endif()
set(ORBITER_DEFAULT_XRSOUND ON)

# The Vulkan graphics client -- the Linux renderer that replaces D3D9Client,
# which is what ORBITER_DEFAULT_D3D9 above turns OFF here.
#
# IT IS NOW ON BY DEFAULT ON NON-WINDOWS, and the comment that used to sit
# here is why this needed changing rather than leaving. It read:
#
#     Off by default: it does not render a scene yet, and a client that
#     registers but draws nothing is worse than no client at all.
#
# That was true when it was written and stopped being true a long way back.
# The client renders terrain, vessels, the HUD, every MFD, the virtual
# cockpit, particle streams and engine exhaust. Leaving the default at OFF
# meant a plain `cmake -B build` on Linux produced an Orbiter with NO graphics
# client at all -- the console session -- and nothing in the configure output
# said so. Every build in this tree passed -DORBITER_BUILD_VULKANCLIENT=ON by
# hand, which is exactly how a stale default survives.
#
# The switch is kept, and it is still forced OFF on Windows: OVP/CMakeLists.txt
# guards the subdirectory with NOT WIN32, because the client is Vulkan on
# GLFW/X11 and the Windows build has D3D9Client.
if(WIN32)
	set(ORBITER_DEFAULT_VULKAN OFF)
else()
	set(ORBITER_DEFAULT_VULKAN ON)
endif()
option(ORBITER_BUILD_VULKANCLIENT
	"Build the Vulkan graphics client (Linux)"
	${ORBITER_DEFAULT_VULKAN}
)

option(
	ORBITER_BUILD_D3D9CLIENT
	"Build the D3D9 graphics client"
	${ORBITER_DEFAULT_D3D9}
)

option(
	ORBITER_BUILD_XRSOUND
	"Build XRSound module to enable sound in Orbiter"
	${ORBITER_DEFAULT_XRSOUND}
)

option(ORBITER_MAKE_DOC
	"Create documentation"
	OFF
)

option(ORBITER_MAKE_TESTS
	"Build unit tests"
	ON
)

option(ORBITER_SANITIZER
	"Build binaries with Address Sanitizer"
	OFF
)

option(ORBITER_TRACY_PROFILER
	"Build orbiter with profiler support"
	OFF
)

include(cmake/tracy.cmake)

if(ORBITER_BUILD_XRSOUND)
	set(IRRKLANG_DIR "" CACHE PATH "Path to the irrKlang library")
endif()

if(ORBITER_BUILD_D3D9CLIENT)
	find_package(DXSDK REQUIRED)
	set(DXSDK_DIR
		${DXSDK_DIR}
		CACHE
		PATH
		"Installation directory of DirectX SDK (June 2010)"
	)
	if(${DXSDK_DIR} STREQUAL "DXSDK_DIR-NOTFOUND")
		message(FATAL_ERROR "DirectX SDK not found. The D3D9 graphics client will not be built.")
	endif()
endif()

if (ORBITER_MAKE_DOC)
	find_package(Doxygen REQUIRED)
endif()

# Enable Address SANitizer
include(cmake/sanitizer.cmake)
if (ORBITER_SANITIZER AND MSVC)
	enable_sanitizer(${ORBITER_SANITIZER})
endif()


# Given a source directory (srcdir) and a target root directory (tgtroot),
# generate a list of all files found in srcdir (srclist) and a list of output files
# when the source directory is copied to the target root (tgtlist)
# For a copy_directory command, srclist can be used as dependencies, and tgtlist as output

function(srcdir_to_tgtlist srcdir tgtroot srclist tgtlist)
	file(GLOB_RECURSE infiles
		RELATIVE ${srcdir}
		${srcdir}/*
	)
	set(local_srclist ${infiles})
	list(TRANSFORM local_srclist PREPEND "${srcdir}/")
	set(local_tgtlist ${infiles})
	list(TRANSFORM local_tgtlist PREPEND "${tgtroot}/")
	set(srclist ${local_srclist} PARENT_SCOPE)
	set(tgtlist ${local_tgtlist} PARENT_SCOPE)
endfunction()


# Installation
install(DIRECTORY Scenarios Textures Meshes Config GravityModels Script Flights BinAssets/
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}
)

set(CMAKE_MODULE_PATH
	${ORBITER_BINARY_ROOT_DIR}
    ${CMAKE_MODULE_PATH}
)

# Package required runtime libraries
INSTALL(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}
	COMPONENT RuntimeLibraries
)

# readme and license
INSTALL(FILES readme.txt
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}
)
INSTALL(FILES LICENSE
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}
)
#COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/Doc/TransX\ readme.txt ${ORBITER_BINARY_DOC_DIR}/TransX/TransX\ readme.txt

# Sub-projects
# On non-Windows, <windows.h> and the other Win32 headers are resolved from
# Src/Orbiter/Linux. Every target needs this, not just the Orbiter executable:
# DlgCtrl, Orbitersdk and each vessel/plugin module include them too. Adding it
# here, before the subdirectories, covers all of them with one directory entry.
# The Windows build never executes this and uses the real SDK headers.
if(NOT WIN32)
	include_directories(BEFORE ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux)
endif()

# ===========================================================================
# orbiter_module_resources(<target> <rcfile> [extra include dirs...])
# ===========================================================================
#
# The per-module counterpart of the Orbiter.rc conversion in
# Src/Orbiter/CMakeLists.txt. On Windows rc.exe compiles every module's .rc
# into that module's own resource section and CreateDialogParam(hInst, ...)
# finds it there. ELF has no resource section, so rc2cpp.py does the same job
# and the generated table registers itself with the core at dlopen -- see
# Src/Orbiter/Linux/ResourceRegistry.cpp for the whole mechanism.
#
# Without this a module's .rc is listed as a CMake source and SILENTLY IGNORED,
# because nothing on Linux compiles a .rc. Every dialog it declares then fails
# to open with no error: FindDialogTemplate returns null and
# CreateDialogParam returns null. That is what made the Scenario Editor's menu
# button do nothing at all.
#
# Windows keeps using rc.exe; this is a no-op there.
function(orbiter_module_resources tgt rcfile)
	if(WIN32)
		return()
	endif()

	find_package(Python3 REQUIRED COMPONENTS Interpreter)

	get_filename_component(_rc_abs  "${rcfile}" ABSOLUTE)
	get_filename_component(_rc_dir  "${_rc_abs}" DIRECTORY)
	set(_out "${CMAKE_CURRENT_BINARY_DIR}/${tgt}_DialogTemplates.cpp")

	set(_incs -I ${_rc_dir}
	          -I ${CMAKE_SOURCE_DIR}/Src/Orbiter
	          -I ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux
	          -I ${CMAKE_SOURCE_DIR}/Orbitersdk/include)
	foreach(_d ${ARGN})
		list(APPEND _incs -I ${_d})
	endforeach()

	# REGISTERED UNDER THE MODULE'S FILE NAME, NOT ITS CMAKE TARGET NAME.
	#
	# The registered name is what CreateDialogParam matches an HINSTANCE
	# against, and an HINSTANCE resolves to the loaded object's path -- so the
	# key has to be the name the .so is written under. Those differ: XRSound's
	# target is XRSound_dll and its OUTPUT_NAME is XRSound, so registering the
	# target name would put its templates under a name no module can ever
	# present. TARGET_FILE_BASE_NAME is the built file without prefix or
	# suffix, which is exactly what Win32Dlg.cpp's moduleNameOf derives.
	add_custom_command(
		OUTPUT ${_out}
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux/rc2cpp.py
			${_rc_abs} ${_out}
			--module $<TARGET_FILE_BASE_NAME:${tgt}> ${_incs}
		DEPENDS
			${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux/rc2cpp.py
			${_rc_abs}
		COMMENT "Converting ${tgt} dialog templates"
		VERBATIM
	)

	target_sources(${tgt} PRIVATE ${_out})
	target_include_directories(${tgt} PRIVATE
		${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux)
endfunction()

add_subdirectory(Extern)

# NO DEBUG POSTFIX ON ANYTHING ORBITER BUILDS, and this must come AFTER Extern.
#
# VulkanSceneGraph's cmake/vsgMacros.cmake:17 does
#
#     set(CMAKE_DEBUG_POSTFIX "d" CACHE STRING "add a postfix, usually d on windows")
#
# which is a CACHE variable, so it applies to the WHOLE project the moment VSG
# is pulled in -- and Extern is added before Src, OVP and Sound, so every
# Orbiter target created afterwards inherits it.
#
# That is not cosmetic. Orbiter loads its modules by exact bare name from
# Orbiter.cfg -- "Atlantis", "Vsop87", "VulkanClient" -- so a Debug build
# producing Atlantisd.so ships a module the simulator cannot load. It also
# breaks the celbody link graph, because the per-body modules link the parent
# ephemeris library by path.
#
# A plain (non-cache) set here shadows the cache entry for every subdirectory
# added below, without fighting VSG over the cache itself. VSG's own targets,
# already added above, keep their postfix -- which is what VSG wants and
# nothing here depends on.
#
# This was previously worked around in ONE place, OVP/VulkanClient/CMakeLists.txt,
# with a per-target DEBUG_POSTFIX "". That property is now redundant but is
# left in place: it is correct on its own terms and costs nothing.
#
# Only a FRESH configure ever showed this. A build directory created before
# VSG arrived kept its old cache and its already-built, un-postfixed modules,
# and ninja is satisfied by files that already exist.
set(CMAKE_DEBUG_POSTFIX "")

add_subdirectory(Src)
add_subdirectory(Utils)
add_subdirectory(Orbitersdk)
add_subdirectory(OVP)
add_subdirectory(Sound)
add_subdirectory(Html)

if(ORBITER_MAKE_DOC)
	add_subdirectory(Doc)
endif()

# =============================================================================
# Set up Distribution package options

configure_file(${CMAKE_SOURCE_DIR}/cmake/CPackOptions.cmake.in
	${CMAKE_BINARY_DIR}/OrbiterCPackOptions.cmake)

# THE PACKAGE GENERATORS ARE PER-PLATFORM, and this was the last Windows-only
# step left in the build.
#
# WIX is the Windows Installer XML toolset and has no Linux implementation, so
# a bare `cpack` in a Linux build tree stopped with
#     CPack Error: Could not create CPack generator: WIX
# and listed the generators it does have. Configure, build and `cmake --install`
# all worked; only the packaging step did not, which is why it went unnoticed.
#
# The Windows list is unchanged. TGZ is the Linux counterpart of the ZIP that
# rides alongside WIX there -- an archive of exactly the install tree, which is
# what `cmake --install` already produces correctly. DEB and RPM are
# deliberately NOT added: this tree installs everything under one directory
# (ORBITER_INSTALL_ROOT_DIR = "Orbiter") rather than into /usr, so a distro
# package built from it would put an entire game tree under /usr/Orbiter.
# Making it FHS-correct is a packaging job, not a generator switch.
if(WIN32)
	set(CPACK_GENERATOR "WIX;ZIP")
else()
	set(CPACK_GENERATOR "TGZ")
endif()
set(CPACK_PACKAGE_NAME OpenOrbiter)
set(CPACK_PACKAGE_VENDOR Orbitersim)
set(CPACK_PACKAGE_DESCRIPTION "Orbiter is an Open-source spaceflight simulator for Windows PCs.")
set(CPACK_PACKAGE_HOMEPAGE_URL http://orbit.medphys.ucl.ac.uk)
set(CPACK_PACKAGE_EXECUTABLES Orbiter;OpenOrbiter)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_COMPONENT_Unspecified_DISPLAY_NAME "Orbiter Core Components")
set(CPACK_COMPONENT_Unspecified_REQUIRED TRUE)
set(CPACK_COMPONENT_RuntimeLibraries_DESCRIPTION "System libraries used by Orbiter. Install if Orbiter fails to launch.")
set(CPACK_RESOURCE_FILE_LICENSE ${ORBITER_SOURCE_ROOT_DIR}/cmake/LICENSE.rtf)
set(CPACK_RESOURCE_FILE_README ${ORBITER_SOURCE_ROOT_DIR}/README.md)
set(CPACK_WIX_UI_BANNER ${ORBITER_SOURCE_ROOT_DIR}/cmake/install_banner.bmp)
set(CPACK_WIX_UI_DIALOG ${ORBITER_SOURCE_ROOT_DIR}/cmake/install_bkg.bmp)
set(CPACK_WIX_PRODUCT_ICON ${ORBITER_SOURCE_ROOT_DIR}/cmake/orbiter_install.ico)
set(CPACK_WIX_UPGRADE_GUID AB3F6483-A1A4-4ECD-ADAE-D33DF3A5AADA)
set(CPACK_WIX_COMPONENT_INSTALL)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_BINARY_DIR}/OrbiterCPackOptions.cmake")

include(OrbiterCPackOptions)
include(CPack)

cpack_add_component(Unspecified
	DISPLAY_NAME "OrbiterCore"
	DESCRIPTION "Orbiter Core Components"
	REQUIRED
)
cpack_add_component(RuntimeLibraries
	DISPLAY_NAME "RuntimeLibraries"
	DESCRIPTION "System libraries used by Orbiter. Install if Orbiter fails to launch."
)

# There appears to be a CMake (3.20.5) bug that prevents CMAKE_INSTALL_PREFIX to be set
# correctly in the CPACK_PRE_BUILD_SCRIPTS, so the script has been disabled for now
# This means that the planet texture installation directory is not reset to default
# for the packages. This needs to be fixed

#set(CPACK_PRE_BUILD_SCRIPTS ${ORBITER_SOURCE_ROOT_DIR}/cmake/cpack_install.cmake)

if (ORBITER_MAKE_TESTS)
      enable_testing()
      add_subdirectory(Tests)
endif()
