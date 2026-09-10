# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# SAFESEH linker flag must be turned off because the DX7 libraries don't support it'
# This is an MSVC link.exe option and is meaningless to any other linker;
# SAFESEH is a 32-bit Windows structured-exception-handling feature with no
# counterpart on ELF, and the DX7 libraries that motivated it are not built
# here at all.
if(MSVC)
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -SAFESEH:NO")
endif()
FetchContent_MakeAvailable(imgui)
FetchContent_MakeAvailable(implot)

# Sources for all Orbiter executable targets
set(common_src
# General source files
	Astro.cpp
	Camera.cpp
	cmdline.cpp
	Config.cpp
	console_ng.cpp
	Element.cpp
	elevmgr.cpp
	Help.cpp
	Input.cpp
	Keymap.cpp
	LightEmitter.cpp
	Mesh.cpp
	Nav.cpp
	Orbiter.cpp
	PlaybackEd.cpp
	Psys.cpp
	Script.cpp
	Shadow.cpp
	State.cpp
	Vecmat.cpp
	VectorMap.cpp
    ConsoleManager.cpp
	TimeData.cpp
# Launchpad
	Launchpad.cpp
	LpadTab.cpp
	TabAbout.cpp
	TabExtra.cpp
	TabModule.cpp
	TabOptions.cpp
	TabScenario.cpp
	TabVideo.cpp
# Dialogs
	CustomControls.cpp
	DialogWin.cpp
	DlgCamera.cpp
	DlgCapture.cpp
	DlgFocus.cpp
	DlgFunction.cpp
	DlgHelp.cpp
	DlgInfo.cpp
	DlgMap.cpp
	DlgMenuCfg.cpp
	DlgMgr.cpp
	DlgOptions.cpp
	DlgRecorder.cpp
	DlgTacc.cpp
# htmlctrl.c embeds Internet Explorer through OLE/COM (IWebBrowser2,
# IHTMLDocument2, IDocHostUIHandler). That has no native Linux equivalent, so
# on non-Windows it is replaced by Linux/HtmlCtrl.cpp, which implements the
# same three-function API. See that file for what is and is not preserved.
$<$<BOOL:${WIN32}>:htmlctrl.c>
	Select.cpp
	OptionsPages.cpp
# Body classes
	Body.cpp
	BodyIntegrator.cpp
	PinesGrav.cpp
	Celbody.cpp
	Planet.cpp
	Rigidbody.cpp
	Star.cpp
# Vessel classes
	FlightRecorder.cpp
	SuperVessel.cpp
	Vessel.cpp
	Vesselbase.cpp
	Vesselstatus.cpp
# Surface base classes
	Base.cpp
	Baseobj.cpp
# Cockpit classes
	Defpanel.cpp
	hud.cpp
	MenuInfoBar.cpp
	Pane.cpp
	Panel.cpp
	Panel2D.cpp
	VCockpit.cpp
# MFD classes
	Mfd.cpp
	MfdAlign.cpp
	MfdComms.cpp
	MfdDocking.cpp
	MfdHsi.cpp
	MfdLanding.cpp
	MfdMap.cpp
	MfdMap_old.cpp
	MfdOrbit.cpp
	MfdSurface.cpp
	MfdSync.cpp
	MfdTransfer.cpp
	MfdUser.cpp
# API implementations
	CamAPI.cpp
	CelSphereAPI.cpp
	DrawAPI.cpp
	GraphicsAPI.cpp
	MFDAPI.cpp
	ModuleAPI.cpp
	OrbiterAPI.cpp
# Graphics utils
	D3d7util.cpp
	D3dmath.cpp
	Di7frame.cpp
# Utils
	Log.cpp
	Memstat.cpp
	Util.cpp
	ZTreeMgr.cpp
# Resources
	Orbiter.rc
	Orbiter.ico
# ImGui backends. Orbiter drives ImGui through two halves: the platform
# backend (window and input) and the renderer backend.
#
# On Windows the platform half is imgui_impl_win32 and the renderer half is
# supplied by the graphics client through its clbkImGui* callbacks, so no
# renderer backend is compiled here.
#
# On Linux both halves are needed up front, because the Launchpad is drawn
# before any graphics client is loaded -- the Launchpad is what selects one.
# So the GLFW platform backend and the Vulkan renderer backend are both
# compiled in and owned by UIHost.cpp.
$<$<BOOL:${WIN32}>:${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp>
$<$<NOT:$<BOOL:${WIN32}>>:${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp>
$<$<NOT:$<BOOL:${WIN32}>>:${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp>
)

set(Orbiter_includes
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/Orbitersdk/include
	${CMAKE_SOURCE_DIR}/OVP
	${CMAKE_CURRENT_BINARY_DIR}
	${HTMLHELP_INCLUDE_DIR}
)

# Non-Windows builds resolve <windows.h>, <commctrl.h> and the other Win32
# headers from Src/Orbiter/Linux, so that directory has to precede all others.
# The Windows build never adds it and continues to use the real SDK headers,
# which is why this is a prepend rather than an edit to the list above.
if(NOT WIN32)
	list(PREPEND Orbiter_includes ${CMAKE_CURRENT_SOURCE_DIR}/Linux)

	# Orbiter.rc is compiled into the executable's resource section by rc.exe
	# on Windows. ELF has no resource section, so rc2cpp.py performs the same
	# conversion at build time and emits a C++ table which is compiled in. The
	# .rc stays the single source of truth for both platforms and is never
	# modified; the converter runs it through the C preprocessor so every
	# #define in resource.h resolves exactly as rc.exe would resolve it.
	#
	# DEPENDS lists the headers the preprocessor reads, so editing an id in
	# resource.h regenerates the table rather than leaving a stale one behind.
	set(ORBITER_DIALOG_TEMPLATES
		${CMAKE_CURRENT_BINARY_DIR}/DialogTemplates.cpp)

	find_package(Python3 REQUIRED COMPONENTS Interpreter)

	add_custom_command(
		OUTPUT ${ORBITER_DIALOG_TEMPLATES}
		COMMAND ${Python3_EXECUTABLE}
			${CMAKE_CURRENT_SOURCE_DIR}/Linux/rc2cpp.py
			${CMAKE_CURRENT_SOURCE_DIR}/Orbiter.rc
			${ORBITER_DIALOG_TEMPLATES}
			-I ${CMAKE_CURRENT_SOURCE_DIR}
			-I ${CMAKE_CURRENT_SOURCE_DIR}/Linux
			-I ${CMAKE_SOURCE_DIR}/Orbitersdk/include
		DEPENDS
			${CMAKE_CURRENT_SOURCE_DIR}/Linux/rc2cpp.py
			${CMAKE_CURRENT_SOURCE_DIR}/Orbiter.rc
			${CMAKE_CURRENT_SOURCE_DIR}/resource.h
			${CMAKE_CURRENT_SOURCE_DIR}/resource2.h
		COMMENT "Converting Orbiter.rc dialog templates"
		VERBATIM
	)

	# Linux-side implementations of the interfaces the shim headers declare.
	# HtmlCtrl.cpp replaces htmlctrl.c, which is excluded above.
	list(APPEND common_src
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/HtmlCtrl.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/Handles.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/Platform.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/Win32Dlg.cpp
		# The resource lookups and the MODULE resource registry. They used to
		# be emitted into DialogTemplates.cpp by rc2cpp.py; they moved here
		# when they had to start searching modules' tables as well. See the
		# header of the file.
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/ResourceRegistry.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/Gdi.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/GcCoreStub.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/DInput.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/WinCodec.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/BmpDecode.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/UIHost.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/UiDriver.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/Linux/LinuxMain.cpp
		${ORBITER_DIALOG_TEMPLATES}
	)
endif()

set(Orbiter_common_libs
	Winmm.dll
	comctl32.lib
	UxTheme.lib
	version.lib
	${HTML_HELP_LIBRARY}
	zlib
	${HTMLHELP_LIB}
	$<TARGET_FILE:Orbitersdk>
	$<TARGET_FILE:DlgCtrl>
)
set(Orbiter_libs
	dxguid.lib
	dinput8.lib
)

# The library lists above name MSVC import libraries. Their Linux counterparts
# are entirely different: the multimedia timer and the Win32 controls are
# provided by the shim rather than by a system library, so what is actually
# needed here is the set of libraries that shim is implemented over.
#
#   glfw     window and input, replacing the Win32 message loop and DirectInput
#   vulkan   the graphics client
#   X11      pulled in by ImGui's GLFW backend, which sets window properties
#            directly on X11 for viewport support
#   dl       dlopen/dlsym behind LoadLibrary/GetProcAddress
#   pthread  CreateThread/CreateMutex/WaitForSingleObject
#   m        the maths library, which MSVC folds into the CRT
if(NOT WIN32)
	find_package(Vulkan REQUIRED)
	find_package(PkgConfig REQUIRED)
	find_package(X11 REQUIRED)
	pkg_check_modules(GLFW REQUIRED glfw3)

	set(Orbiter_common_libs
		zlib
		$<TARGET_FILE:Orbitersdk>
		$<TARGET_FILE:DlgCtrl>
		stb::stb
		${GLFW_LIBRARIES}
		Vulkan::Vulkan
		${X11_LIBRARIES}
		${CMAKE_DL_LIBS}
		pthread
		m
	)
	set(Orbiter_libs "")

	list(APPEND Orbiter_includes ${GLFW_INCLUDE_DIRS} ${X11_INCLUDE_DIR})
endif()

set(Orbiter_depends
	Orbitersdk
	DlgCtrl
)

# Orbiter executable (graphics server version)
add_executable(Orbiter ${common_src})

# /SUBSYSTEM and /ENTRY are MSVC link.exe options. On ELF the entry point is
# the ordinary main() and there is no subsystem concept, but there is one flag
# that matters: -rdynamic (--export-dynamic) puts the executable's own symbols
# into its dynamic symbol table.
#
# That is load-bearing for two parts of this port. Modules resolve GImGui and
# GImPlot from the executable at load time -- the design imconfig.h describes,
# where the exe owns the ImGui context. And GetProcAddress is implemented over
# dlsym, which Memstat.cpp relies on to find GetProcessMemoryInfo by name.
# Without -rdynamic both fail at runtime rather than at link, so it is not
# optional.
if(MSVC)
	set(ORBITER_LINK_FLAGS "/SUBSYSTEM:CONSOLE /ENTRY:WinMainCRTStartup")
else()
	set(ORBITER_LINK_FLAGS "-rdynamic")
endif()

set_target_properties(Orbiter
	PROPERTIES
	LINK_FLAGS "${ORBITER_LINK_FLAGS}"
	RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
	OUTPUT_NAME Orbiter
	VS_DEBUGGER_WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
	COMPILE_DEFINITIONS "NOGRAPHICS;DIRECTINPUT_VERSION=0x0800"
	FOLDER Core
)

target_include_directories(Orbiter PUBLIC ${Orbiter_includes} ${TRACY_CLIENT_INCLUDE} ${imgui_SOURCE_DIR}/ ${imgui_SOURCE_DIR}/backends ${implot_SOURCE_DIR}/)

target_link_libraries(Orbiter ${Orbiter_common_libs} ${Orbiter_libs} ${TRACY_CLIENT})
if(NOT WIN32)
	# openpty, for the session console in Src/Orbiter/Linux/Platform.cpp
	target_link_libraries(Orbiter util)
endif()


add_dependencies(Orbiter ${Orbiter_depends})

# Installation: Orbiter executable (graphics-server versions)
install(TARGETS Orbiter	RUNTIME DESTINATION ${ORBITER_INSTALL_ROOT_DIR})

# If the standard Orbiter executable is not built...
set_target_properties(Orbiter PROPERTIES ENABLE_EXPORTS 1)

# Publishing the import library, so external projects can link modules
# against the core.
#
# This whole step is Windows-only. There is no import library on ELF: a module
# leaves the core's symbols undefined and the dynamic linker binds them at
# dlopen time against the executable, which is exported thanks to -rdynamic.
# With ORBITER_LIB empty, the copy below would run as
#     cmake -E copy  Orbitersdk/lib/
# with no source and fail the build.
if(WIN32)
	# ...we have to use the NG version to export the API links
	install(FILES $<TARGET_LINKER_FILE:Orbiter> DESTINATION ${ORBITER_INSTALL_SDK_DIR}/lib)

	# Copy library to its Orbitersdk/lib location so that external project integrated into the build can find it
	add_custom_command(TARGET ${OrbiterTgt}	POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory ${ORBITER_BINARY_SDK_DIR}/lib
		COMMAND ${CMAKE_COMMAND} -E copy ${ORBITER_LIB} ${ORBITER_BINARY_SDK_DIR}/lib/
	)
endif()

string(TIMESTAMP DATE "%d %b %Y")
string(TIMESTAMP VERSION "%y%m%d")
configure_file(about.hpp.in about.hpp @ONLY)
