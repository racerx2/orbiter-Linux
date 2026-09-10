# Copyright (c) Martin Schweiger
# Licensed under the MIT License
#
# CONVERTED FROM OVP/D3D9Client/CMakeLists.txt, read end to end (254 lines).
#
# The build file is the last place the two clients differ, and it differs in
# exactly the ways the source did:
#
#  1. NO DirectX SDK. DXSDK_DIR, DXSDK_LIB_DIR, DXSDK_LIB_DIR2, d3d9.lib,
#     d3dx9.lib and d3dx9d.lib all go together. What replaces them is
#     Vulkan::Vulkan plus glslang and SPIRV-Tools, because the client compiles
#     its own GLSL at run time where D3DX compiled HLSL -- see
#     VulkanUtil.cpp's CompileShaderStage.
#
#  2. NO WINDOWS IMPORT LIBRARIES. kernel32, user32, gdi32, winspool, comdlg32,
#     advapi32, shell32, ole32, oleaut32, uuid, odbc32, odbccp32, version and
#     msimg32 are the Win32 API, and Src/Orbiter/Linux provides what this
#     client uses of it. Nothing links in their place except the C library.
#
#  3. THE .rc IS KEPT AND IS CONVERTED TWICE. D3D9Client.rc is not a Windows
#     artefact to be dropped: read end to end it declares TEN DIALOGS, a
#     BITMAP, a STRINGTABLE and a VERSIONINFO block, and this client opens
#     nine of those dialogs -- IDD_D3D9SETUP and IDD_D3D9CREDITS from
#     VideoTab.cpp, IDD_D3D9MESHDEBUG and IDD_DEBUGVIEW from
#     DebugControls.cpp, IDD_D3D9SCATTER from AtmoControls.cpp, and
#     IDD_MESHDEBUG, IDD_MATERIAL, IDD_SCENEDEBUG and IDD_MICROTEXTOOLS from
#     WindowMgr.cpp.
#
#     So VulkanClient.rc is listed as a source exactly as the Windows file
#     lists D3D9Client.rc, and the two Linux generators below turn it into the
#     things ELF cannot carry in a resource section: rc2cpp.py, through
#     orbiter_module_resources(), emits the dialog templates that
#     FindDialogTemplate looks up, and rcstrings.py emits the two exported
#     strings ModuleTab::RefreshLists reads. Neither runs on Windows, where
#     rc.exe does both jobs.
#
#     AN EARLIER NOTE HERE SAID THE .rc CARRIED "only a version block and an
#     icon, neither of which the Linux loader reads, so nothing is generated
#     from it". That was wrong on every count, and it was wrong invisibly: a
#     .rc listed as a source with no generator is silently ignored, so every
#     dialog the client opened returned null with no error, and the client
#     listed itself in the Modules pane with no description under
#     "Miscellaneous". Only the VERSIONINFO block is genuinely inert here.
#
#  4. NO imgui_impl_dx9.cpp. The ImGui backends belong to the core here --
#     Src/Orbiter/Linux/UIHost.cpp runs imgui_impl_glfw and imgui_impl_vulkan.
#     See note 3 in VulkanClient.cpp.
#
#  5. NO InterfaceBuilder. samples/InterfaceBuilder/InterfaceBuilder.exe is a
#     Windows binary that generates gcCoreAPI.h from gcCore.h + gcCore.cpp.
#     OVP/CMakeLists.txt already handles the header the samples need by
#     pointing INCLUDE_TARGET_DIR at the client's own directory, and
#     Src/Orbiter/Linux aliases gcCoreAPI.h to gcCore.h. So the custom command,
#     its target and the two install(FILES) that depended on it are gone.
#
#  6. THE SHADERS ARE .glsl, and they are installed beside their .tech
#     technique tables -- the file that replaces the technique/pass blocks an
#     .fx carried inline. See VulkanEffect.h.
#
#  7. THE SAMPLES ARE NOT ADDED HERE. OVP/CMakeLists.txt already adds all four
#     from the D3D9Client tree, where they still live, and they contain no
#     Direct3D -- they link only ${OrbiterTgt} and Orbitersdk.
#
# The four things the source needed and the Windows file did not provide:
# stb (the image loader that replaces D3DX's), ImGui's headers (the graphics
# panel is an ImGuiDialog), fontconfig (face name -> font file, replacing GDI
# glyph rasterisation -- see VulkanTextMgr) and <filesystem> (the directory
# walks that replace FindFirstFile and _findfirst).

project(VulkanClient VERSION 31.0)

# D3D9ClientConfig.h.in -> VulkanClientConfig.h.in, and its two macros with
# it: D3D9_VERSION_MAJOR/MINOR become VULKAN_VERSION_MAJOR/MINOR. This was the
# last file in the tree still carrying the Windows name.
#
# THE FILE IS GENERATED AND NEVER READ, on Windows as here. Nothing in either
# client #includes the generated header, and neither project() sets the two
# variables the template substitutes -- `project(D3D9Client VERSION 31.0)`
# defines D3D9Client_VERSION_MAJOR, not D3D9_VERSION_MAJOR -- so both platforms
# write a header whose macros expand to nothing. Carried across in that shape
# rather than repaired: an unset variable is not a Win32 or D3D9 difference,
# and repairing it would be a change to the client rather than a conversion of
# it.
configure_file(VulkanClientConfig.h.in VulkanClientConfig.h)
FetchContent_MakeAvailable(imgui)

# specify the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(VULKAN_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(ShaderDir ${VULKAN_SOURCE_DIR}/shaders)

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)

# glslang and SPIRV-Tools. The client compiles GLSL at run time, which is what
# D3DXCompileShaderFromFile did for HLSL, so the compiler is a link dependency
# rather than a build-time tool. Distributions package the CMake config; the
# pkg-config fallback covers the ones that do not.
find_package(glslang QUIET)
if(NOT glslang_FOUND)
	pkg_check_modules(GLSLANG REQUIRED glslang)
endif()

set(SourceFiles
	AABBUtil.cpp
	AtmoControls.cpp
	BeaconArray.cpp
	CelSphere.cpp
	CloudMgr.cpp
	Cloudmgr2.cpp
	CSphereMgr.cpp
	VulkanClient.cpp
	VulkanConfig.cpp
	VulkanControlPanel.cpp
	VulkanEffect.cpp
	VulkanFrame.cpp
	VulkanPad.cpp
	VulkanPad2.cpp
	VulkanPad3.cpp
	VulkanSurface.cpp
	VulkanTextMgr.cpp
	VulkanUtil.cpp
	DebugControls.cpp
	gcCore.cpp
	gcConst.cpp
	GDIPad.cpp
	HazeMgr.cpp
	IProcess.cpp
	Log.cpp
	MaterialMgr.cpp
	Mesh.cpp
	MeshMgr.cpp
	OapiExtension.cpp
	Particle.cpp
	RingMgr.cpp
	RunwayLights.cpp
	Scene.cpp
	Spherepatch.cpp
	SurfMgr.cpp
	Surfmgr2.cpp
	TileLabel.cpp
	TileMgr.cpp
	Tilemgr2.cpp
	VBase.cpp
	VideoTab.cpp
	VObject.cpp
	VPlanet.cpp
	VPlanetAtmo.cpp
	VStar.cpp
	VVessel.cpp
	WindowMgr.cpp
	ZTreeMgr.cpp
	Tilemgr2_imp.hpp
)

set(IncludeFiles
	AABBUtil.h
	AtmoControls.h
	BeaconArray.h
	CelSphere.h
	CloudMgr.h
	Cloudmgr2.h
	CSphereMgr.h
	VulkanCatalog.h
	VulkanClient.h
	VulkanConfig.h
	VulkanEffect.h
	VulkanFrame.h
	VulkanPad.h
	VulkanSurface.h
	VulkanTextMgr.h
	VulkanTypes.h
	VulkanUtil.h
	DebugControls.h
	GDIPad.h
	HazeMgr.h
	IProcess.h
	Log.h
	MaterialMgr.h
	Mesh.h
	MeshMgr.h
	OapiExtension.h
	Particle.h
	Qtree.h
	resource.h
	RingMgr.h
	RunwayLights.h
	Scene.h
	Spherepatch.h
	SurfMgr.h
	Surfmgr2.h
	TileLabel.h
	TileMgr.h
	Tilemgr2.h
	VBase.h
	VectorHelpers.h
	VideoTab.h
	VObject.h
	VPlanet.h
	VStar.h
	VVessel.h
	WindowMgr.h
	ZTreeMgr.h
	gcConst.h
	gcCore.h
	gcGUI.h
)

# The GLSL translations of the 23 .fx / .hlsl files, plus the technique tables
# that carry what an .fx declared inline. Listed for the IDE; nothing here
# compiles them, because the client compiles them at run time.
#
# GLOBBED RATHER THAN LISTED, which the Windows file does not do and which is
# deliberate. The GLSL translations are being written one at a time, and a
# named-but-absent source is a hard CMake error -- so a literal list would
# break the build for every shader not yet converted, which is the opposite of
# what a source list is for. The names are still recorded, in the install rule
# below and in the porting notes. When the set is complete
# this can go back to a literal list if the churn is ever a problem.
file(GLOB ShaderFiles
	${ShaderDir}/*.glsl
	${ShaderDir}/*.tech
)

source_group(Shaders FILES ${ShaderFiles})

set_source_files_properties(${ShaderFiles} PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(VulkanClient MODULE
	${SourceFiles}
	${IncludeFiles}
	${ShaderFiles}
	VulkanClient.rc
)

# The ten dialog templates. Same call, same reason, as ScnEditor's: on Windows
# rc.exe compiles the .rc into the module's resource section and
# CreateDialogParam(hInst, ...) finds it there; here rc2cpp.py emits a table
# that registers itself at dlopen. Without it the nine dialogs this client
# opens all fail silently -- see note 3 at the top of this file.
#
# No extra include directories are passed: the function already supplies the
# .rc's own directory (for resource.h) and Src/Orbiter/Linux (for winres.h,
# commctrl.h and richedit.h), which is every header this .rc includes.
orbiter_module_resources(VulkanClient ${CMAKE_CURRENT_SOURCE_DIR}/VulkanClient.rc)

# The module description and category, from the same .rc's STRINGTABLE.
#
# ModuleTab::RefreshLists identifies a plugin with LoadString(hMod, 1000) and
# LoadString(hMod, 1001); on Linux those resolve to exported symbols, which
# rcstrings.py generates. Without this the client appears in the Modules pane
# with no description under "Miscellaneous" instead of "Graphics engines".
#
# Generated at CONFIGURE time, for the reason spelled out in
# Src/Plugin/CMakeLists.txt: a build-time custom command would leave the file
# absent when target_sources runs, and the GENERATED property that would
# excuse it is directory-scoped. A .rc changes only when the source tree does.
if(NOT WIN32)
	set(_vkrcstrings ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux/rcstrings.py)
	set(_vkmodstrings ${CMAKE_CURRENT_BINARY_DIR}/VulkanClient_modulestrings.cpp)
	execute_process(
		COMMAND python3 ${_vkrcstrings}
		        ${CMAKE_CURRENT_SOURCE_DIR}/VulkanClient.rc
		        ${_vkmodstrings}
		        -I ${CMAKE_CURRENT_SOURCE_DIR}
		        -I ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux
		        -I ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
		RESULT_VARIABLE _vkrcres
	)
	if(_vkrcres EQUAL 0)
		target_sources(VulkanClient PRIVATE ${_vkmodstrings})
	else()
		message(WARNING "rcstrings failed for VulkanClient; it will show "
		                "under Miscellaneous with no description")
	endif()
endif()

target_include_directories(VulkanClient PUBLIC
	${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux
	${CMAKE_SOURCE_DIR}/Src/Orbiter
	${CMAKE_CURRENT_BINARY_DIR}
	${imgui_SOURCE_DIR}/
	${imgui_SOURCE_DIR}/backends/
	${FONTCONFIG_INCLUDE_DIRS}
)

target_link_libraries(VulkanClient
	$<TARGET_FILE:Orbitersdk>
	stb::stb
	Vulkan::Vulkan
	${FONTCONFIG_LIBRARIES}
)

# glslang's own targets when the CMake config was found, the pkg-config
# libraries otherwise. SPIRV is glslang's SPIR-V generator and is a separate
# library in both packagings.
# glslang-default-resource-limits is a THIRD library and is easy to miss.
# VulkanUtil.cpp's CompileShaderStage calls GetDefaultResources(), which is
# glslang's built-in TBuiltInResource -- the table of implementation limits a
# GLSL compile needs and that HLSL's compiler kept internally. It ships in its
# own archive (libglslang-default-resource-limits.a), so linking glslang and
# SPIRV alone leaves _Z19GetDefaultResourcesv undefined. A MODULE library
# links with undefined symbols allowed on ELF, so the omission does not fail
# the build -- it fails at dlopen, on the first shader compile, which is why
# it is named here rather than left to be discovered.
if(glslang_FOUND)
	target_link_libraries(VulkanClient
		glslang::glslang
		glslang::SPIRV
		glslang::glslang-default-resource-limits
	)
else()
	target_link_libraries(VulkanClient
		${GLSLANG_LIBRARIES}
		SPIRV
		glslang-default-resource-limits
	)
	target_include_directories(VulkanClient PUBLIC ${GLSLANG_INCLUDE_DIRS})
endif()

# <filesystem> is in libstdc++ from GCC 9. Older toolchains need the separate
# library, which was folded in afterwards. Tilemgr2.cpp, VPlanet.cpp and
# VideoTab.cpp all use it, replacing _findfirst and FindFirstFile.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
	target_link_libraries(VulkanClient stdc++fs)
endif()

# -Bsymbolic: A MODULE MUST RESOLVE ITS OWN CLASSES TO ITSELF.
#
# THIS IS THE WINDOWS DLL SYMBOL NAMESPACE, AND IT HAS NO LINE TO CONVERT --
# it is a property Windows gave the reference for free and Linux does not.
#
# A Win32 DLL has its own symbol namespace: D3D9Client.dll defines
# `class RunwayLights` (OVP/D3D9Client/RunwayLights.h) and Orbiter.exe defines
# a COMPLETELY DIFFERENT `class RunwayLights` (Src/Orbiter/Baseobj.h:446,
# derived from BaseObject), and the two never meet. On Linux the executable's
# symbols are global and PREEMPT a shared object's, so the client's own
#
#     for (i = 0; i < numRunwayLights; i++) SAFE_DELETE(runwayLights[i]);
#
# in vBase::~vBase bound to Orbiter's RunwayLights::~RunwayLights and ran it
# over a client object of a different shape:
#
#     Thread 1 SIGSEGV
#     RunwayLights::Deactivate  Src/Orbiter/Baseobj.cpp:2424  delete []dyndata->Vtx
#     RunwayLights::~RunwayLights
#     vBase::~vBase             OVP/VulkanClient/VBase.cpp:321
#     vPlanet::~vPlanet -> Scene::DeleteAllVisuals -> clbkCloseSession
#
# -- which is why every session ended in a segfault during teardown, and why
# the five vkDestroy*-while-still-in-use validation errors appear alongside it:
# the teardown dies part-way through and never finishes releasing anything.
#
# -Bsymbolic binds references INSIDE this module to this module's own
# definitions, which is exactly the DLL rule. It does not change what the
# module exports, so DLLCLBK's visibility("default") entry points and the
# oapi virtual interface are untouched -- that is why this rather than
# -fvisibility=hidden, which would also hide typeinfo and can break
# dynamic_cast across the module boundary.
#
# The collision is not limited to RunwayLights: the client and the core both
# define Mesh, Scene and other common names. This fixes the class of bug, not
# the one instance.
target_link_options(VulkanClient PRIVATE "LINKER:-Bsymbolic")

# Orbiter names its modules bare -- Modules/Plugin/VulkanClient.so, not
# libVulkanClient.so -- and a Debug build must not add a postfix either, or
# Orbiter.cfg's module list stops matching. Same three lines as every other
# converted module directory.
set_target_properties(VulkanClient
	PROPERTIES
	LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Modules/Plugin
	LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Modules/Plugin
	LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Modules/Plugin
	PREFIX ""
	DEBUG_POSTFIX ""
	FOLDER OVP
)

# COMPILE_DEFINITIONS D3D9CLIENT_EXPORTS IS NOT CARRIED OVER, and it is the
# one line of the Windows build file that would have broken this one.
#
# It is not an export macro despite the name -- nothing in the client tests it.
# What it does is unlock six blocks in Orbitersdk/include/DrawAPI.h: the
# `#include "d3dx9.h"` at the top, and five sets of interop constructors and
# conversion operators that let FVECTOR2/3/4 and FMATRIX4 be built from, and
# assigned to, D3DXVECTOR2/3/4 and D3DXMATRIX. The graphics client is the only
# translation unit in the tree that ever needs to convert between the SDK's
# vector types and D3DX's, which is why the SDK hides them behind a macro
# only that client defines.
#
# There is no D3DX here and no d3dx9.h to include, so defining it is a hard
# compile error on the first file that reaches DrawAPI.h. Leaving it out is
# not a loss: the five interop blocks convert to types that do not exist, and
# the converted client has no call site for any of them -- every one was
# rewritten to FVECTOR/FMATRIX during the port.

# The shader directory is copied next to the built module so a run from the
# build tree finds Modules/VulkanClient/*.glsl, which is the path every
# VulkanTechInit and every ImageProcessing constructor asks for.
#
# THE WINDOWS FILE DOES THIS AS `TARGET D3D9Client PRE_BUILD`, AND THAT SHAPE
# IS WRONG ON BOTH PLATFORMS -- kept here as a driven-by-dependency rule
# instead, which is a build-system correction and changes nothing the client
# does at run time.
#
# A PRE_BUILD command hangs off the LIBRARY target, so it runs only when the
# library itself is relinked. Editing a shader alone touches no C++ source, so
# the build reports "no work to do" and the stale copy from the last relink
# keeps being loaded -- silently, because the client compiles whatever GLSL it
# finds and never compares it with the source. Every shader-only edit then
# appears to have had no effect, which is indistinguishable from the edit
# being wrong. It cost a full measurement cycle here before it was noticed.
#
# Driving the copy from a stamp file that DEPENDS on the shader sources makes
# the build system do what it is for: touch a .glsl and the copy re-runs;
# touch nothing and it does not. ${ShaderFiles} is a glob, so a NEW shader
# file still needs cmake to re-run -- that was already true of the source list.
set(_vkShaderStamp ${CMAKE_CURRENT_BINARY_DIR}/vulkan_shaders.stamp)
add_custom_command(
	OUTPUT ${_vkShaderStamp}
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${ShaderDir}/ ${ORBITER_BINARY_MODULE_DIR}/VulkanClient
	COMMAND ${CMAKE_COMMAND} -E touch ${_vkShaderStamp}
	DEPENDS ${ShaderFiles}
	COMMENT "Copying VulkanClient shaders"
)
add_custom_target(VulkanClientShaders DEPENDS ${_vkShaderStamp})
add_dependencies(VulkanClient VulkanClientShaders)

add_dependencies(VulkanClient
	${OrbiterTgt}
	Orbitersdk
)

install(TARGETS VulkanClient
	LIBRARY
	DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)

install(DIRECTORY ${ShaderDir}/ DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Modules/VulkanClient)
